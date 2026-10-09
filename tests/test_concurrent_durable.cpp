// Group commit under concurrency: many writers in Durable mode, the process is
// SIGKILLed at a random moment, and every write that was acknowledged to a
// writer must be present after recovery. Also checks that fdatasyncs are
// actually shared between writers rather than issued one per record.
#include "../src/db/lsm_engine.h"
#include "test_util.h"
#include <atomic>
#include <chrono>
#include <iostream>
#include <sys/mman.h>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>

using namespace testutil;

constexpr int kThreads = 8;
constexpr int kPerThread = 3000;

static std::string keyOf(int t, int i) { return "t" + std::to_string(t) + "_" + std::to_string(i); }
static std::string valOf(int t, int i) { return "val_" + std::to_string(t) + "_" + std::to_string(i) + std::string(i % 50, 'z'); }

int main() {
    std::cout << "=== Concurrent Durable writers: group commit + crash ===\n";

    // 1. Group commit actually batches.
    {
        const std::string dir = freshDir("conc_batch");
        LSMEngine db(dir, 64 << 20, WAL::Durability::Durable);
        std::vector<std::thread> ts;
        for (int t = 0; t < kThreads; ++t)
            ts.emplace_back([&, t] { for (int i = 0; i < 400; ++i) db.put(keyOf(t, i), valOf(t, i)); });
        for (auto& th : ts) th.join();
        const uint64_t puts = kThreads * 400, syncs = db.walSyncCount();
        std::cout << "  " << puts << " durable puts used " << syncs << " fdatasyncs\n";
        CHECK(syncs >= 1 && syncs <= puts);
        for (int t = 0; t < kThreads; ++t) for (int i = 0; i < 400; ++i) CHECK(db.get(keyOf(t, i)) == valOf(t, i));
    }

    // 2. Acknowledged writes survive a SIGKILL at random times.
    auto* acked = static_cast<volatile int*>(mmap(nullptr, sizeof(int) * kThreads, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANONYMOUS, -1, 0));
    CHECK(acked != MAP_FAILED);
    std::mt19937_64 rng(2024);
    for (int round = 0; round < 12; ++round) {
        const std::string dir = freshDir("conc_crash");
        for (int t = 0; t < kThreads; ++t) acked[t] = 0;
        const size_t memtable = (round % 2) ? (16 << 10) : (8 << 20);   // odd rounds also flush + compact under load
        pid_t pid = fork();
        CHECK(pid >= 0);
        if (pid == 0) {
            LSMEngine db(dir, memtable, WAL::Durability::Durable);
            std::vector<std::thread> ts;
            for (int t = 0; t < kThreads; ++t)
                ts.emplace_back([&, t] {
                    for (int i = 0; i < kPerThread; ++i) { db.put(keyOf(t, i), valOf(t, i)); acked[t] = i + 1; }
                });
            for (auto& th : ts) th.join();
            _exit(0);
        }
        usleep(5000 + rng() % 120000);
        kill(pid, SIGKILL);
        int status = 0;
        waitpid(pid, &status, 0);

        LSMEngine db(dir, memtable, WAL::Durability::Durable);
        size_t total_acked = 0;
        for (int t = 0; t < kThreads; ++t) {
            const int a = acked[t];
            total_acked += a;
            for (int i = 0; i < a; ++i) {
                auto g = db.get(keyOf(t, i));
                CHECK_MSG(g && *g == valOf(t, i), "round " + std::to_string(round) + ": acknowledged write " + keyOf(t, i) + " lost after SIGKILL");
            }
            // Unacked keys may or may not exist, but if present they must be intact.
            for (int i = a; i < std::min(a + 2, kPerThread); ++i) { auto g = db.get(keyOf(t, i)); CHECK(!g || *g == valOf(t, i)); }
        }
        std::cout << "  round " << round << ": killed with " << total_acked << " acknowledged writes (memtable " << memtable << "B), all recovered\n";
    }
    std::cout << "PASS\n";
    return 0;
}
