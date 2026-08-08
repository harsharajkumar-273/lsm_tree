#include "../src/db/lsm_engine.h"
#include <iostream>
#include <chrono>
#include <vector>
#include <numeric>
#include <algorithm>
#include <filesystem>
#include <fcntl.h>
#include <unistd.h>
#include <cstring>

// Synchronous WAL implementation for benchmarking comparison
class SyncWAL {
public:
    explicit SyncWAL(const std::string& path) {
        fd_ = open(path.c_str(), O_WRONLY | O_APPEND | O_CREAT, 0644);
    }
    ~SyncWAL() {
        if (fd_ >= 0) close(fd_);
    }
    // Serialises one record into the same layout the real WAL uses.
    std::vector<char> encode(const std::string& key, const std::string& value) {
        // Prepare sequential buffer
        std::vector<char> buf(1 + 4 + 4 + key.size() + value.size() + 4);
        char* p = buf.data();
        *p++ = 0x01; // type PUT
        uint32_t klen = key.size(), vlen = value.size();
        std::memcpy(p, &klen, 4); p += 4;
        std::memcpy(p, &vlen, 4); p += 4;
        std::memcpy(p, key.data(), klen); p += klen;
        std::memcpy(p, value.data(), vlen); p += vlen;
        uint32_t crc = 0xDEADBEEF; // dummy CRC for sync bench speed
        std::memcpy(p, &crc, 4);

        return buf;
    }

    // write() only — no fdatasync. Records land in the page cache and are lost
    // on a kernel crash or power failure, so this is the weakest of the three
    // durability points measured here.
    void writeNoSync(const std::string& key, const std::string& value) {
        std::vector<char> buf = encode(key, value);
        [[maybe_unused]] ssize_t written = write(fd_, buf.data(), buf.size());
    }

    // write() + fdatasync — durable per record.
    void writeSync(const std::string& key, const std::string& value) {
        std::vector<char> buf = encode(key, value);
        [[maybe_unused]] ssize_t written = write(fd_, buf.data(), buf.size());
        fdatasync(fd_);
    }
private:
    int fd_;
};

int main() {
    std::cout << "===========================================\n";
    std::cout << "  Benchmark: Write Throughput and Latency\n";
    std::cout << "===========================================\n";

    const int N = 10000;
    std::vector<double> io_uring_latencies;
    io_uring_latencies.reserve(N);

    // 1. Benchmark io_uring writes
    std::filesystem::remove_all("/tmp/lsm_bench_iouring");
    {
        LSMEngine db("/tmp/lsm_bench_iouring");

        auto start = std::chrono::high_resolution_clock::now();
        for (int i = 0; i < N; ++i) {
            std::string key = "key_" + std::to_string(i);
            std::string val = "val_" + std::to_string(i);
            
            auto t0 = std::chrono::high_resolution_clock::now();
            db.put(key, val);
            auto t1 = std::chrono::high_resolution_clock::now();
            
            io_uring_latencies.push_back(std::chrono::duration<double, std::micro>(t1 - t0).count());
        }
        auto end = std::chrono::high_resolution_clock::now();
        double duration_ms = std::chrono::duration<double, std::milli>(end - start).count();

        std::sort(io_uring_latencies.begin(), io_uring_latencies.end());
        double sum = std::accumulate(io_uring_latencies.begin(), io_uring_latencies.end(), 0.0);
        double avg = sum / N;
        double p99 = io_uring_latencies[static_cast<int>(N * 0.99)];

        std::cout << "Sequential write (io_uring WAL) - no per-record fsync:\n";
        std::cout << "  - Throughput:   " << static_cast<int>(N / (duration_ms / 1000.0)) << " ops/sec\n";
        std::cout << "  - Avg Latency:  " << avg << " us\n";
        std::cout << "  - P99 Latency:  " << p99 << " us\n\n";
    }

    // 2. Benchmark traditional synchronous writes (write + fdatasync)
    std::filesystem::remove_all("/tmp/lsm_bench_sync");
    std::filesystem::create_directories("/tmp/lsm_bench_sync");
    {
        SyncWAL sync_wal("/tmp/lsm_bench_sync/sync.log");
        std::vector<double> sync_latencies;
        sync_latencies.reserve(N);

        auto start = std::chrono::high_resolution_clock::now();
        for (int i = 0; i < N; ++i) {
            std::string key = "key_" + std::to_string(i);
            std::string val = "val_" + std::to_string(i);
            
            auto t0 = std::chrono::high_resolution_clock::now();
            sync_wal.writeSync(key, val);
            auto t1 = std::chrono::high_resolution_clock::now();
            
            sync_latencies.push_back(std::chrono::duration<double, std::micro>(t1 - t0).count());
        }
        auto end = std::chrono::high_resolution_clock::now();
        double duration_ms = std::chrono::duration<double, std::milli>(end - start).count();

        std::sort(sync_latencies.begin(), sync_latencies.end());
        double sum = std::accumulate(sync_latencies.begin(), sync_latencies.end(), 0.0);
        double avg = sum / N;
        double p99 = sync_latencies[static_cast<int>(N * 0.99)];

        std::cout << "Sequential write (write + fdatasync) - durable per record:\n";
        std::cout << "  - Throughput:   " << static_cast<int>(N / (duration_ms / 1000.0)) << " ops/sec\n";
        std::cout << "  - Avg Latency:  " << avg << " us\n";
        std::cout << "  - P99 Latency:  " << p99 << " us\n\n";
    }

    // 3. Traditional write() without fdatasync.
    //
    // This arm exists because the two above do not measure the same thing, and
    // a speedup quoted from that pair conflates the interface with the sync.
    //
    // The three arms sit at three different durability points, which is the
    // honest way to read them:
    //
    //   arm 2  write + fdatasync  - on stable storage before returning
    //   arm 1  io_uring + O_DIRECT - bypasses the page cache, reaches the
    //                                device, but is not forced to stable
    //                                storage per record
    //   arm 3  buffered write      - page cache only; lost on kernel crash
    //
    // So no two arms are directly comparable, and this one is not offered as an
    // equivalent to arm 1 — it is the floor, showing what the cost would be
    // with no durability effort at all.
    std::filesystem::remove_all("/tmp/lsm_bench_nosync");
    std::filesystem::create_directories("/tmp/lsm_bench_nosync");
    {
        SyncWAL nosync_wal("/tmp/lsm_bench_nosync/nosync.log");
        std::vector<double> nosync_latencies;
        nosync_latencies.reserve(N);

        auto start = std::chrono::high_resolution_clock::now();
        for (int i = 0; i < N; ++i) {
            std::string key = "key_" + std::to_string(i);
            std::string val = "val_" + std::to_string(i);

            auto t0 = std::chrono::high_resolution_clock::now();
            nosync_wal.writeNoSync(key, val);
            auto t1 = std::chrono::high_resolution_clock::now();

            nosync_latencies.push_back(std::chrono::duration<double, std::micro>(t1 - t0).count());
        }
        auto end = std::chrono::high_resolution_clock::now();
        double duration_ms = std::chrono::duration<double, std::milli>(end - start).count();

        std::sort(nosync_latencies.begin(), nosync_latencies.end());
        double sum = std::accumulate(nosync_latencies.begin(), nosync_latencies.end(), 0.0);
        double avg = sum / N;
        double p99 = nosync_latencies[static_cast<int>(N * 0.99)];

        std::cout << "Sequential write (write, no fsync) - no per-record fsync:\n";
        std::cout << "  - Throughput:   " << static_cast<int>(N / (duration_ms / 1000.0)) << " ops/sec\n";
        std::cout << "  - Avg Latency:  " << avg << " us\n";
        std::cout << "  - P99 Latency:  " << p99 << " us\n\n";
    }

    std::cout << "Note: these three arms sit at three different durability points\n";
    std::cout << "      and none is a like-for-like substitute for another.\n";
    std::cout << "        arm 2  durable per record (fdatasync)\n";
    std::cout << "        arm 1  O_DIRECT, reaches the device, not forced per record\n";
    std::cout << "        arm 3  page cache only - lost on a kernel crash\n";
    std::cout << "      Any single speedup figure quoted from one pair conflates\n";
    std::cout << "      the I/O interface with the durability guarantee. The sync\n";
    std::cout << "      arms also use a fixed CRC rather than computing one, which\n";
    std::cout << "      favours them slightly.\n\n";

    return 0;
}
