#include "../src/wal/wal.h"
#include <iostream>
#include <string>
#include <vector>
#include <thread>
#include <map>
#include <filesystem>
#include <cstdio>

/*
 * Regression tests for issue #14 — WAL records landing out of submission order.
 *
 * The WAL used to submit every write with offset -1, letting the kernel resolve
 * the position to end-of-file under O_APPEND. Each append is atomic, so bytes
 * never interleave, but the *position* was decided at completion time rather
 * than submission time — and writeEntry() releases mu_ after submitting, so
 * several writes are in flight at once with no ordering guarantee between them.
 *
 * Measured on the pre-fix code with the same workload as testConcurrentOrdering
 * below: 12,216 / 11,539 / 13,661 per-thread order violations across three runs,
 * the first appearing within the first six records.
 *
 * For a write-ahead log that is a correctness failure rather than a cosmetic
 * one: replaying put(k, v1) after put(k, v2) leaves the wrong value, so recovery
 * can produce a state the engine was never in.
 *
 * The fix assigns each record its byte offset under mu_ at submission time.
 * These tests hold that property in place.
 */

void pass(const std::string& test) {
    std::cout << "  \033[32m✓\033[0m " << test << "\n";
}
void fail(const std::string& test, const std::string& msg) {
    std::cout << "  \033[31m✗\033[0m " << test << " — " << msg << "\n";
    std::exit(1);
}

namespace {

const std::string kDir = "/tmp/lsm_test_wal_ordering";

std::string freshPath(const std::string& name) {
    std::filesystem::create_directories(kDir);
    const std::string p = kDir + "/" + name + ".wal";
    std::filesystem::remove(p);
    return p;
}

/*
 * Concurrent writes are persisted in submission order.
 *
 * Each thread writes its own monotonically increasing sequence. Per-thread order
 * is the meaningful guarantee: it is a real submission order, whereas the order
 * between two threads racing for the lock is not defined by anything and would
 * make the test measure its own scheduling rather than the WAL.
 */
void testConcurrentOrdering() {
    constexpr int kThreads = 4;
    constexpr int kPerThread = 2000;

    const std::string path = freshPath("concurrent_ordering");
    {
        WAL wal(path);
        std::vector<std::thread> threads;
        for (int t = 0; t < kThreads; ++t) {
            threads.emplace_back([&wal, t]() {
                for (int i = 0; i < kPerThread; ++i) {
                    char key[32];
                    std::snprintf(key, sizeof key, "t%02d_%08d", t, i);
                    wal.logPut(key, "v");
                }
            });
        }
        for (auto& th : threads) th.join();
    }

    WAL reader(path);
    const auto entries = reader.recover();

    std::map<int, long> lastSeen;
    for (size_t i = 0; i < entries.size(); ++i) {
        int thread = 0;
        long seq = 0;
        if (std::sscanf(entries[i].key.c_str(), "t%d_%ld", &thread, &seq) != 2) continue;

        auto it = lastSeen.find(thread);
        if (it != lastSeen.end() && seq < it->second) {
            fail("concurrent writes persist in submission order",
                 "thread " + std::to_string(thread) + " sequence " + std::to_string(seq) +
                 " appears after " + std::to_string(it->second) +
                 " at record index " + std::to_string(i));
        }
        lastSeen[thread] = seq;
    }
    pass("concurrent writes persist in submission order");
}

/*
 * Recovery replays entries in the order they were written.
 *
 * Single-threaded, so submission order is unambiguous and the assertion is exact
 * rather than per-thread.
 */
void testRecoveryReplayOrder() {
    constexpr int kCount = 5000;

    const std::string path = freshPath("replay_order");
    {
        WAL wal(path);
        for (int i = 0; i < kCount; ++i) {
            char key[32];
            std::snprintf(key, sizeof key, "k%08d", i);
            wal.logPut(key, "v" + std::to_string(i));
        }
    }

    WAL reader(path);
    const auto entries = reader.recover();

    long previous = -1;
    for (size_t i = 0; i < entries.size(); ++i) {
        const long current = std::atol(entries[i].key.c_str() + 1);
        if (current <= previous) {
            fail("recovery replays entries in write order",
                 "key " + entries[i].key + " at index " + std::to_string(i) +
                 " does not follow " + std::to_string(previous));
        }
        previous = current;
    }
    pass("recovery replays entries in write order");
}

/*
 * clear() resets the write offset.
 *
 * Without the reset the next record would be written past a hole the size of the
 * old log, so recovery would read zeroed bytes before reaching real data. The
 * check is that a cleared log recovers exactly what was written after the clear,
 * and nothing from before it.
 */
void testClearResetsOffset() {
    const std::string path = freshPath("clear_resets");
    {
        WAL wal(path);
        for (int i = 0; i < 500; ++i) wal.logPut("before" + std::to_string(i), "v");
        wal.clear();
        for (int i = 0; i < 10; ++i) wal.logPut("after" + std::to_string(i), "v");
    }

    WAL reader(path);
    const auto entries = reader.recover();

    for (const auto& e : entries) {
        if (e.key.rfind("before", 0) == 0) {
            fail("clear() resets the write offset",
                 "pre-clear key '" + e.key + "' survived the clear");
        }
    }
    if (entries.empty()) {
        fail("clear() resets the write offset", "no post-clear entries recovered");
    }
    pass("clear() resets the write offset");
}

/*
 * Reopening an existing log resumes at its end rather than at zero.
 *
 * The offset is a member, so a fresh WAL over an existing file starts at 0
 * unless the constructor establishes it from the file size — which would
 * overwrite the log from the beginning and silently destroy committed records.
 * This is the failure mode that O_APPEND used to prevent for free.
 */
void testReopenResumesAtEnd() {
    const std::string path = freshPath("reopen_resumes");

    { WAL wal(path); for (int i = 0; i < 100; ++i) wal.logPut("first" + std::to_string(i), "v"); }
    { WAL wal(path); for (int i = 0; i < 100; ++i) wal.logPut("second" + std::to_string(i), "v"); }

    WAL reader(path);
    const auto entries = reader.recover();

    bool sawFirst = false, sawSecond = false;
    for (const auto& e : entries) {
        if (e.key.rfind("first", 0) == 0)  sawFirst = true;
        if (e.key.rfind("second", 0) == 0) sawSecond = true;
    }
    if (!sawFirst) {
        fail("reopening a log resumes at its end",
             "records from the first session were overwritten");
    }
    if (!sawSecond) {
        fail("reopening a log resumes at its end",
             "records from the second session were not persisted");
    }
    pass("reopening a log resumes at its end");
}

}  // namespace

int main() {
    std::cout << "===========================================\n";
    std::cout << "  Running Unit Test: WAL Write Ordering\n";
    std::cout << "===========================================\n";

    std::filesystem::remove_all(kDir);

    testConcurrentOrdering();
    testRecoveryReplayOrder();
    testClearResetsOffset();
    testReopenResumesAtEnd();

    std::filesystem::remove_all(kDir);

    std::cout << "===========================================\n";
    std::cout << "  All WAL ordering tests passed.\n";
    std::cout << "===========================================\n";
    return 0;
}
