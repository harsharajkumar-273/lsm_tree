// Write-path benchmark that compares implementations at the SAME acknowledgment
// and durability level, and separates "submission" from "durable" throughput.
//
// Why this exists: comparing an io_uring WAL that returns on *submission* with a
// write()+fdatasync() WAL that returns on *durability* measures two different
// promises. This harness defines three acknowledgment levels and only compares
// implementations within a level:
//
//   submitted - put() returned; the record was handed to the kernel. Not durable.
//   durable   - put() returned; the record is on stable storage (fdatasync done).
//
// For every run it reports
//   ack time      time for all N put() calls to return           -> ack throughput
//   drain time    extra time until all N records are completed   -> drain
//                 AND fdatasync'd (sync() / final fdatasync)
//   durable tput  N / (ack + drain)                              -> the number that
//                 is comparable across ALL implementations, because after drain
//                 every implementation has made the same promise.
//
// Each configuration is repeated --runs times with a fresh directory; median,
// min and max are reported, never a single run.
#include "../src/db/lsm_engine.h"
#include "../src/wal/wal.h"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <mutex>
#include <numeric>
#include <sstream>
#include <string>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <sys/utsname.h>
#include <thread>
#include <unistd.h>
#include <vector>

using Clock = std::chrono::steady_clock;
static double secs(Clock::time_point a, Clock::time_point b) { return std::chrono::duration<double>(b - a).count(); }

// ------------------------------------------------------------------ baselines
// Same on-disk record layout and a real CRC32, so the baselines do no less work
// per record than the engine's WAL (the old SyncWAL used a constant CRC).
static uint32_t crc32(const std::string& k, const std::string& v) {
    uint32_t crc = 0xFFFFFFFF;
    auto up = [&](uint8_t b) { crc ^= b; for (int i = 0; i < 8; ++i) crc = (crc >> 1) ^ (0xEDB88320 & -(crc & 1)); };
    up(1); for (unsigned char c : k) up(c); for (unsigned char c : v) up(c);
    return crc ^ 0xFFFFFFFF;
}
static std::vector<char> encode(const std::string& k, const std::string& v) {
    std::vector<char> b(1 + 4 + 4 + k.size() + v.size() + 4);
    char* p = b.data(); *p++ = 1;
    uint32_t kl = k.size(), vl = v.size();
    memcpy(p, &kl, 4); p += 4; memcpy(p, &vl, 4); p += 4;
    memcpy(p, k.data(), kl); p += kl; memcpy(p, v.data(), vl); p += vl;
    uint32_t c = crc32(k, v); memcpy(p, &c, 4);
    return b;
}

// Buffered append WAL. mode: kNone (page cache only), kPerRecord (fdatasync after
// every record, serialised by a mutex), kODsync (O_DSYNC: each write is durable
// on return).
struct PosixWAL {
    enum Mode { kNone, kPerRecord, kODsync };
    int fd; Mode mode; std::mutex mu;
    PosixWAL(const std::string& path, Mode m) : mode(m) {
        fd = open(path.c_str(), O_WRONLY | O_CREAT | O_APPEND | (m == kODsync ? O_DSYNC : 0), 0644);
        if (fd < 0) { perror("open"); exit(1); }
    }
    ~PosixWAL() { close(fd); }
    void put(const std::string& k, const std::string& v) {
        auto b = encode(k, v);
        std::lock_guard<std::mutex> g(mu);
        if (write(fd, b.data(), b.size()) != (ssize_t)b.size()) { perror("write"); exit(1); }
        if (mode == kPerRecord) fdatasync(fd);
    }
    void drainDurable() { fdatasync(fd); }   // makes kNone comparable at the end
};

// ------------------------------------------------------------------ harness
struct Impl {
    std::string name;
    std::string ack;                                           // acknowledgment level of put()
    std::function<void(const std::string& dir)> setup;
    std::function<void(const std::string&, const std::string&)> put;
    std::function<void()> drain;                               // complete + make durable
    std::function<uint64_t()> logBytes;                        // bytes the implementation put in the log
    std::function<void()> teardown;
};

struct RunResult { double ack_s, drain_s; std::vector<double> lat_us; uint64_t log_bytes; };

static RunResult runOnce(Impl& impl, const std::string& dir, size_t n, size_t vsize, int threads) {
    std::filesystem::remove_all(dir);
    std::filesystem::create_directories(dir);
    impl.setup(dir);
    std::string val(vsize, 'v');
    std::vector<std::vector<double>> lats(threads);
    std::atomic<size_t> next{0};
    // Workers stay alive until the drain has been measured. An io_uring task that
    // exits waits for its in-flight requests, so letting threads exit (or join)
    // before drain() would silently fold completion time into the "ack" window
    // for exactly the implementations that acknowledge early.
    std::mutex gate_mu; std::condition_variable gate_cv; bool release = false;
    std::atomic<int> finished{0};
    std::vector<Clock::time_point> done_at(threads);
    auto worker = [&](int t) {
        lats[t].reserve(n / threads + 1);
        char kb[32];
        for (;;) {
            size_t i = next.fetch_add(1);
            if (i >= n) break;
            snprintf(kb, sizeof kb, "key%013zu", i);          // 16-byte keys
            auto t0 = Clock::now();
            impl.put(kb, val);
            lats[t].push_back(std::chrono::duration<double, std::micro>(Clock::now() - t0).count());
        }
        done_at[t] = Clock::now();
        finished.fetch_add(1);
        std::unique_lock<std::mutex> lk(gate_mu);
        gate_cv.wait(lk, [&] { return release; });
    };
    auto t0 = Clock::now();
    std::vector<std::thread> ts;
    for (int t = 0; t < threads; ++t) ts.emplace_back(worker, t);
    while (finished.load() < threads) std::this_thread::yield();
    auto t1 = *std::max_element(done_at.begin(), done_at.end());   // last put() returned
    impl.drain();
    auto t2 = Clock::now();
    { std::lock_guard<std::mutex> g(gate_mu); release = true; }
    gate_cv.notify_all();
    for (auto& th : ts) th.join();
    RunResult r{secs(t0, t1), secs(t1, t2), {}, impl.logBytes()};
    for (auto& l : lats) r.lat_us.insert(r.lat_us.end(), l.begin(), l.end());
    impl.teardown();
    return r;
}

static double median(std::vector<double> v) { std::sort(v.begin(), v.end()); return v[v.size() / 2]; }
static double pct(std::vector<double>& v, double p) { return v[std::min(v.size() - 1, (size_t)(v.size() * p))]; }

static std::string readFirst(const char* path, const char* prefix = nullptr) {
    std::ifstream f(path); std::string line;
    while (std::getline(f, line)) if (!prefix || line.rfind(prefix, 0) == 0) return line;
    return "?";
}

static void printHardware(const std::string& dir) {
    struct utsname u; uname(&u);
    std::cout << "## Environment\n";
    std::cout << "- CPU: " << readFirst("/proc/cpuinfo", "model name") << " (" << std::thread::hardware_concurrency() << " hardware threads)\n";
    std::cout << "- RAM: " << readFirst("/proc/meminfo", "MemTotal") << "\n";
    std::cout << "- Kernel: " << u.sysname << " " << u.release << "\n";
    struct stat st; std::string dev = "?", fstype = "?";
    if (stat(dir.c_str(), &st) == 0) {
        std::ifstream mi("/proc/self/mountinfo"); std::string line; size_t best = 0;
        unsigned maj = major(st.st_dev), mnr = minor(st.st_dev);
        while (std::getline(mi, line)) {
            unsigned a, b; char mp[512]; int id, pid;
            if (sscanf(line.c_str(), "%d %d %u:%u %*s %511s", &id, &pid, &a, &b, mp) == 5 && a == maj && b == mnr) {
                size_t pos = line.find(" - "); std::istringstream rest(line.substr(pos + 3)); rest >> fstype >> dev;
                best = 1;
            }
        }
        (void)best;
        std::string sys = "/sys/dev/block/" + std::to_string(maj) + ":" + std::to_string(mnr) + "/queue/rotational";
        std::cout << "- Storage under " << dir << ": " << dev << " (" << fstype << "), rotational=" << readFirst(sys.c_str()) << "\n";
    }
    std::cout << "- Build: compiler " << __VERSION__
#ifdef NDEBUG
              << ", NDEBUG"
#endif
              << ", CMAKE_BUILD_TYPE=Release (-O3)\n";
}

// Cost floor of the storage stack: one 512 B O_APPEND write + fdatasync.
static double fdatasyncFloorUs(const std::string& dir) {
    std::filesystem::create_directories(dir);
    int fd = open((dir + "/probe").c_str(), O_WRONLY | O_CREAT | O_APPEND, 0644);
    char b[512] = {1};
    std::vector<double> v;
    for (int i = 0; i < 300; ++i) {
        auto t0 = Clock::now();
        if (write(fd, b, sizeof b) < 0) break;
        fdatasync(fd);
        v.push_back(std::chrono::duration<double, std::micro>(Clock::now() - t0).count());
    }
    close(fd); std::filesystem::remove_all(dir);
    std::sort(v.begin(), v.end());
    return v[v.size() / 2];
}

int main(int argc, char** argv) {
    std::string dir = "/tmp/lsm_bench_durability";
    size_t n = 10000; int runs = 5; std::vector<size_t> sizes = {16, 128, 1024, 4096}; std::vector<int> threadCounts = {1, 8};
    bool csv = false;
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        auto val = [&] { return std::string(argv[++i]); };
        if (a == "--dir") dir = val();
        else if (a == "--ops") n = std::stoul(val());
        else if (a == "--runs") runs = std::stoi(val());
        else if (a == "--csv") csv = true;
        else if (a == "--quick") { n = 1000; runs = 2; sizes = {128, 4096}; threadCounts = {1, 4}; }
        else if (a == "--sizes") { sizes.clear(); std::stringstream s(val()); std::string t; while (std::getline(s, t, ',')) sizes.push_back(std::stoul(t)); }
        else if (a == "--threads") { threadCounts.clear(); std::stringstream s(val()); std::string t; while (std::getline(s, t, ',')) threadCounts.push_back(std::stoi(t)); }
        else { std::cerr << "usage: bench_durability [--dir D] [--ops N] [--runs R] [--sizes a,b] [--threads a,b] [--quick] [--csv]\n"; return 2; }
    }
    std::filesystem::create_directories(dir);

    std::cout << "# Write-path benchmark: submission vs durable throughput\n\n";
    printHardware(dir);
    std::cout << "- fdatasync floor (512 B append + fdatasync, median of 300): " << fdatasyncFloorUs(dir + "/probe") << " us\n";
    std::cout << "- Per config: " << n << " ops, 16-byte keys, " << runs << " fresh-directory runs; values of the sizes shown\n\n";

    // ---- implementations -------------------------------------------------
    std::unique_ptr<WAL> wal; std::unique_ptr<LSMEngine> eng; std::unique_ptr<PosixWAL> pw;
    auto walImpl = [&](const char* name, WAL::Durability d, const char* ack) {
        Impl i{name, ack,
               [&, d](const std::string& dd) { wal = std::make_unique<WAL>(dd + "/wal.log", d); },
               [&](const std::string& k, const std::string& v) { wal->logPut(k, v); },
               [&] { wal->sync(); },
               [&] { return (uint64_t)std::filesystem::file_size(dir + "/run/wal.log"); },
               [&] { wal.reset(); }};
        return i;
    };
    auto engImpl = [&](const char* name, WAL::Durability d, const char* ack) {
        Impl i{name, ack,
               [&, d](const std::string& dd) { eng = std::make_unique<LSMEngine>(dd, size_t(2) << 30, d); },   // no flush during the run
               [&](const std::string& k, const std::string& v) { eng->put(k, v); },
               [&] { eng->sync(); },
               [&] { return (uint64_t)std::filesystem::file_size(dir + "/run/wal.log"); },
               [&] { eng.reset(); }};
        return i;
    };
    auto posixImpl = [&](const char* name, PosixWAL::Mode m, const char* ack) {
        Impl i{name, ack,
               [&, m](const std::string& dd) { pw = std::make_unique<PosixWAL>(dd + "/wal.log", m); },
               [&](const std::string& k, const std::string& v) { pw->put(k, v); },
               [&] { pw->drainDurable(); },
               [&] { return (uint64_t)std::filesystem::file_size(dir + "/run/wal.log"); },
               [&] { pw.reset(); }};
        return i;
    };
    std::vector<Impl> impls = {
        walImpl  ("io_uring+O_DIRECT WAL, ack=submitted",   WAL::Durability::Async,   "submitted"),
        posixImpl("write() WAL, ack=submitted (page cache)", PosixWAL::kNone,          "submitted"),
        walImpl  ("io_uring+O_DIRECT WAL, ack=durable (group commit)", WAL::Durability::Durable, "durable"),
        posixImpl("write()+fdatasync per record, ack=durable", PosixWAL::kPerRecord,   "durable"),
        posixImpl("O_DSYNC write(), ack=durable",           PosixWAL::kODsync,         "durable"),
        engImpl  ("LSMEngine::put, ack=submitted",           WAL::Durability::Async,   "submitted"),
        engImpl  ("LSMEngine::put, ack=durable",             WAL::Durability::Durable, "durable"),
    };

    if (csv) std::cout << "impl,ack,threads,value_bytes,run,ack_s,drain_s,ops,log_bytes\n";
    for (int threads : threadCounts) {
        for (size_t vsize : sizes) {
            const size_t logical = n * (16 + vsize);
            std::cout << "### " << threads << " thread(s), " << vsize << " B values (" << n << " ops, " << logical / 1024 << " KiB logical)\n\n";
            std::cout << "| implementation | ack level | ack throughput (ops/s) | drain (ms) | durable throughput (ops/s) | p50 / p99 / p99.9 put (us) | log bytes / logical |\n";
            std::cout << "|---|---|---:|---:|---:|---|---:|\n";
            for (auto& impl : impls) {
                // Per-record-fsync baselines are serial by construction; skip multi-thread variants of
                // the single-lock implementations only where it would just repeat the 1-thread number.
                std::vector<double> ack_tp, drain_ms, dur_tp; std::vector<double> all_lat; uint64_t logb = 0;
                for (int r = 0; r < runs; ++r) {
                    // The implementations resolve their directory through `run` so file_size() can find the log.
                    std::string rd = dir + "/run";
                    auto res = runOnce(impl, rd, n, vsize, threads);
                    ack_tp.push_back(n / res.ack_s); drain_ms.push_back(res.drain_s * 1000);
                    dur_tp.push_back(n / (res.ack_s + res.drain_s));
                    all_lat.insert(all_lat.end(), res.lat_us.begin(), res.lat_us.end()); logb = res.log_bytes;
                    if (csv) std::cout << impl.name << "," << impl.ack << "," << threads << "," << vsize << "," << r << ","
                                       << res.ack_s << "," << res.drain_s << "," << n << "," << res.log_bytes << "\n";
                }
                std::sort(all_lat.begin(), all_lat.end());
                auto range = [](std::vector<double>& v, const char* fmt) {
                    char b[96]; snprintf(b, sizeof b, fmt, median(v), *std::min_element(v.begin(), v.end()), *std::max_element(v.begin(), v.end())); return std::string(b);
                };
                char lat[96]; snprintf(lat, sizeof lat, "%.1f / %.1f / %.1f", pct(all_lat, .5), pct(all_lat, .99), pct(all_lat, .999));
                char amp[32]; snprintf(amp, sizeof amp, "%.1fx", (double)logb / logical);
                std::cout << "| " << impl.name << " | " << impl.ack << " | " << range(ack_tp, "%.0f (%.0f–%.0f)") << " | "
                          << range(drain_ms, "%.1f (%.1f–%.1f)") << " | " << range(dur_tp, "**%.0f** (%.0f–%.0f)") << " | " << lat << " | " << amp << " |\n";
            }
            std::cout << "\n";
        }
    }
    std::cout << "Median over " << runs << " runs, (min–max) in parentheses.\n";
    std::filesystem::remove_all(dir);
    return 0;
}
