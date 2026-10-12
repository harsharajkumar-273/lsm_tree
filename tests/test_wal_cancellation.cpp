// -ECANCELED handling in the WAL.
//
// io_uring completes a request with -ECANCELED when the thread that submitted it
// exits while it is still in flight (observed on tmpfs; ext4 usually finishes
// the write inline). Nothing is wrong with the write, so the WAL must resubmit
// it - same buffer, length and offset - and must not treat the record as complete
// until a final completion arrives. Previously the cancellation was recorded as a
// permanent failure: the record never reached the log and further appends were
// refused.
//
// Two layers of coverage:
//   * deterministic: fault injection turns chosen completions into -ECANCELED, so
//     CI does not depend on kernel timing;
//   * real: submitting threads exit with writes in flight, on every supported
//     filesystem (ext4/whatever temp is, and tmpfs when it supports O_DIRECT).
#include "../src/db/lsm_engine.h"
#include "../src/util/fault.h"
#include "../src/wal/wal.h"
#include "test_util.h"
#include <atomic>
#include <fcntl.h>
#include <iostream>
#include <thread>
#include <unistd.h>

using namespace testutil;

static std::string valueFor(int t, int i) { return "val_" + std::to_string(t) + "_" + std::to_string(i) + std::string((t * 31 + i) % 90, 'x'); }
static std::string keyFor(int t, int i) { return "t" + std::to_string(t) + "_" + std::to_string(i); }

// Every expected record exactly once, with the right value, in per-thread order.
static void expectExactly(const std::vector<WAL::Entry>& got, int threads, int per_thread, const std::string& ctx) {
    CHECK_MSG(got.size() == static_cast<size_t>(threads) * per_thread,
              ctx + ": recovered " + std::to_string(got.size()) + " records, expected " + std::to_string(threads * per_thread));
    std::vector<int> next(threads, 0);
    for (const auto& e : got) {
        CHECK(e.type == WAL::Entry::Type::PUT);
        int t = 0, i = 0;
        CHECK_MSG(std::sscanf(e.key.c_str(), "t%d_%d", &t, &i) == 2, ctx + ": unparseable key " + e.key);
        CHECK_MSG(t >= 0 && t < threads, ctx + ": bad thread in " + e.key);
        CHECK_MSG(i == next[t], ctx + ": " + e.key + " out of order, expected index " + std::to_string(next[t]));
        CHECK_MSG(e.value == valueFor(t, i), ctx + ": wrong value for " + e.key);
        ++next[t];
    }
    for (int t = 0; t < threads; ++t) CHECK_MSG(next[t] == per_thread, ctx + ": thread " + std::to_string(t) + " lost records");
}

static std::string walPath(const std::string& name) { return freshDir(name) + "/wal.log"; }

static std::vector<WAL::Entry> recoverFile(const std::string& path) { WAL w(path); return w.recover(); }

// ---- deterministic ----------------------------------------------------------

static void injectedCancelIsRetried() {
    for (auto mode : {WAL::Durability::Async, WAL::Durability::Durable}) {
        const std::string path = walPath("cancel_inject");
        {
            WAL wal(path, mode);
            fault::arm("wal.cqe_cancel", 3, fault::Action::Fail, 4);   // 4 consecutive cancelled completions
            for (int i = 0; i < 60; ++i) wal.logPut(keyFor(0, i), valueFor(0, i));
            wal.sync();
            fault::disarmAll();
            CHECK_MSG(wal.cancelRetries() >= 4, "injected cancellations were not retried");
        }
        expectExactly(recoverFile(path), 1, 60, "injected cancel");
    }
    std::cout << "  injected -ECANCELED is resubmitted; every record recovered exactly once, Async and Durable OK\n";
}

// Retried writes use the original offset: with cancellations hitting records in
// the middle of a burst, neighbours must neither be overwritten nor displaced.
static void retryPreservesOffsetAndBuffer() {
    const std::string path = walPath("cancel_offset");
    {
        WAL wal(path);
        for (int i = 0; i < 40; ++i) {
            if (i % 7 == 3) fault::arm("wal.cqe_cancel", 1, fault::Action::Fail, 2);
            wal.logPut(keyFor(0, i), valueFor(0, i));
            wal.sync();
            fault::disarmAll();
        }
    }
    expectExactly(recoverFile(path), 1, 40, "offset preservation");
    std::cout << "  resubmission keeps the original offset and buffer contents OK\n";
}

static void persistentCancellationIsBoundedAndReported() {
    for (uint64_t repeat : {uint64_t(WAL::kMaxWriteAttempts), uint64_t(100000)}) {
        const std::string path = walPath("cancel_permanent");
        WAL wal(path);
        fault::arm("wal.cqe_cancel", 1, fault::Action::Fail, repeat);
        wal.logPut("k", "v");
        std::string msg;
        try { wal.sync(); } catch (const std::runtime_error& e) { msg = e.what(); }
        const uint64_t hits = fault::hitCount("wal.cqe_cancel");
        fault::disarmAll();
        CHECK_MSG(!msg.empty(), "a write cancelled forever was reported as success");
        CHECK_MSG(msg.find("cancel") != std::string::npos, "permanent failure does not say it was a cancellation: " + msg);
        CHECK_MSG(hits <= static_cast<uint64_t>(WAL::kMaxWriteAttempts), "retries are unbounded: " + std::to_string(hits) + " attempts");
        CHECK(wal.cancelRetries() == static_cast<uint64_t>(WAL::kMaxWriteAttempts - 1));

        bool refused = false;
        try { wal.logPut("k2", "v2"); } catch (const std::runtime_error&) { refused = true; }
        CHECK_MSG(refused, "log kept accepting records after a permanent write failure");
    }
    std::cout << "  persistent cancellation: " << WAL::kMaxWriteAttempts - 1 << " retries then a clear permanent failure, log refuses further appends OK\n";
}

// Durable: put returns only after the (possibly retried) write completed AND the
// fdatasync that covers it. Asserted through the hit counters of the sync points
// and by recovering a byte-for-byte copy of the log the instant put() returns.
static void durableAcknowledgedOnlyAfterCompletionAndSync() {
    const std::string path = walPath("cancel_durable");
    WAL wal(path, WAL::Durability::Durable);
    fault::arm("wal.after_fdatasync", 1000000, fault::Action::Throw);    // never fires: counts hits
    fault::arm("wal.cqe_cancel", 1, fault::Action::Fail, 2);
    wal.logPut("durable_key", "durable_value");
    CHECK_MSG(fault::hitCount("wal.cqe_cancel") >= 3, "expected 2 cancelled + 1 final completion before the ack");
    CHECK_MSG(fault::hitCount("wal.after_fdatasync") == 1, "Durable put returned without a successful fdatasync");
    CHECK(wal.syncCount() == 1);
    fault::disarmAll();

    const std::string copy = freshDir("cancel_durable_copy") + "/wal.log";
    std::filesystem::copy_file(path, copy);
    auto got = recoverFile(copy);
    CHECK_MSG(got.size() == 1 && got[0].key == "durable_key" && got[0].value == "durable_value", "acknowledged Durable record absent from the log at ack time");

    // A permanently cancelled Durable write is never acknowledged.
    const std::string path2 = walPath("cancel_durable_fail");
    WAL wal2(path2, WAL::Durability::Durable);
    fault::arm("wal.cqe_cancel", 1, fault::Action::Fail, 100000);
    bool acked = true;
    try { wal2.logPut("k", "v"); } catch (const std::runtime_error&) { acked = false; }
    fault::disarmAll();
    CHECK_MSG(!acked, "Durable put acknowledged a write that never completed");
    std::cout << "  Durable put: acknowledged only after completion + fdatasync; permanent failure is never acknowledged OK\n";
}

// ---- real thread exit ---------------------------------------------------------

struct Target { std::string name, dir; };

static bool supportsODirect(const std::string& dir) {
    std::filesystem::create_directories(dir);
    int fd = open((dir + "/probe").c_str(), O_WRONLY | O_CREAT | O_DIRECT, 0644);
    if (fd < 0) return false;
    close(fd);
    std::filesystem::remove(dir + "/probe");
    return true;
}

static uint64_t threadsExitWithWritesInFlight(const Target& tg, WAL::Durability mode, int rounds) {
    constexpr int kThreads = 6, kPer = 1200;
    uint64_t retries = 0;
    for (int r = 0; r < rounds; ++r) {
        const std::string path = tg.dir + "/exit_" + std::to_string(r) + ".wal";
        std::filesystem::remove(path);
        std::atomic<int> threw{0};
        std::string first_error;
        std::mutex em;
        {
            WAL wal(path, mode);
            std::vector<std::thread> ts;
            for (int t = 0; t < kThreads; ++t)
                ts.emplace_back([&, t] {
                    try { for (int i = 0; i < kPer; ++i) wal.logPut(keyFor(t, i), valueFor(t, i)); }
                    catch (const std::exception& e) { if (!threw++) { std::lock_guard<std::mutex> g(em); first_error = e.what(); } }
                });
            for (auto& th : ts) th.join();          // every submitting thread is gone; writes may be in flight
            CHECK_MSG(threw == 0, tg.name + " round " + std::to_string(r) + ": a put failed after submitter exit: " + first_error);
            wal.sync();
            retries += wal.cancelRetries();
        }
        expectExactly(recoverFile(path), kThreads, kPer, tg.name + " round " + std::to_string(r) + (mode == WAL::Durability::Durable ? " durable" : " async"));
        std::filesystem::remove(path);
    }
    return retries;
}

static void engineLevel(const Target& tg) {
    constexpr int kThreads = 4, kPer = 800;
    const std::string dir = tg.dir + "/engine";
    std::filesystem::remove_all(dir);
    {
        LSMEngine db(dir, 64 << 20);
        std::vector<std::thread> ts;
        for (int t = 0; t < kThreads; ++t)
            ts.emplace_back([&, t] { for (int i = 0; i < kPer; ++i) db.put(keyFor(t, i), valueFor(t, i)); });
        for (auto& th : ts) th.join();
        db.sync();
        const std::string clone = tg.dir + "/engine_clone";      // "process died here"
        std::filesystem::remove_all(clone);
        std::filesystem::copy(dir, clone, std::filesystem::copy_options::recursive);
        {
            LSMEngine rec(clone, 64 << 20);
            for (int t = 0; t < kThreads; ++t)
                for (int i = 0; i < kPer; ++i) {
                    auto g = rec.get(keyFor(t, i));
                    CHECK_MSG(g && *g == valueFor(t, i), tg.name + ": engine lost or corrupted " + keyFor(t, i) + " after submitters exited");
                }
        }
        std::filesystem::remove_all(clone);
    }
    std::filesystem::remove_all(dir);
}

int main() {
    std::cout << "=== WAL -ECANCELED handling ===\n";
    injectedCancelIsRetried();
    retryPreservesOffsetAndBuffer();
    persistentCancellationIsBoundedAndReported();
    durableAcknowledgedOnlyAfterCompletionAndSync();

    std::vector<Target> targets;
    targets.push_back({"default-tmp(" + std::filesystem::temp_directory_path().string() + ")", freshDir("cancel_real_default")});
    if (supportsODirect("/dev/shm/lsm_cancel_test")) targets.push_back({"tmpfs(/dev/shm)", "/dev/shm/lsm_cancel_test"});
    else std::cout << "  (tmpfs: O_DIRECT unsupported here, skipped)\n";

    for (const auto& tg : targets) {
        uint64_t retries = 0;
        retries += threadsExitWithWritesInFlight(tg, WAL::Durability::Async, 15);
        retries += threadsExitWithWritesInFlight(tg, WAL::Durability::Durable, 3);
        engineLevel(tg);
        std::cout << "  " << tg.name << ": submitters exit with writes in flight, all records and values recovered ("
                  << retries << " real cancellations retried)\n";
    }
    for (const auto& tg : targets) std::filesystem::remove_all(tg.dir);
    std::cout << "PASS\n";
    return 0;
}
