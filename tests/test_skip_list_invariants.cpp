#include "../src/memtable/skip_list.h"
#include "../src/memtable/arena.h"
#include <iostream>
#include <thread>
#include <vector>
#include <string>
#include <unordered_set>
#include <cassert>

/*
 * Structural invariants of the lock-free SkipList under concurrent insertion.
 *
 * Written for issue #19, which reported that the higher-level linking retry
 * could produce a cycle — specifically `newNode->next[i] == newNode` — after the
 * level-0 CAS makes the new node visible to find().
 *
 * I could not reproduce that, and I believe the mechanism cannot occur. The
 * argument, recorded here so the reasoning survives with the test:
 *
 *   A cycle needs find() to return newNode as one of succs[i]. find() advances
 *   only while the next key is strictly less than the search key:
 *
 *       while (next != nullptr && next->key < key) { x = next; next = ...; }
 *
 *   newNode->key == key, not < key, so the traversal halts *at* newNode on every
 *   level rather than passing through it. Being visible to the walk and being
 *   returned by it are different things.
 *
 *   It cannot be a pred either: predecessors are strictly less than the search
 *   key by construction.
 *
 *   Nor can a second node carry the same key. When two threads race on one key,
 *   both find() miss and both allocate, but only one level-0
 *   compare_exchange_strong succeeds. The loser falls to the outer retry, find()
 *   now reports the key present, and it takes the overwrite path — so there is
 *   no equal-keyed sibling for find() to hand back.
 *
 * The test exists so that invariant is checked rather than assumed. If a future
 * change to the linking logic does introduce a cycle, this catches it instead of
 * a production find() hanging.
 *
 * Three properties are verified on every level:
 *   1. no node is visited twice        (a cycle)
 *   2. keys strictly increase          (the ordering invariant the search needs)
 *   3. the walk terminates in bounds   (a runaway traversal)
 */

void pass(const std::string& test) {
    std::cout << "  \033[32m✓\033[0m " << test << "\n";
}
void fail(const std::string& test, const std::string& msg) {
    std::cout << "  \033[31m✗\033[0m " << test << " — " << msg << "\n";
    std::exit(1);
}

namespace {

constexpr int  kThreads        = 8;
constexpr int  kInsertsPerThread = 4000;
// Deliberately smaller than the total insert count, so threads collide on the
// same keys and exercise the CAS retry path rather than inserting disjointly.
constexpr int  kKeySpace       = 3000;
constexpr int  kRounds         = 12;

struct WalkResult {
    bool   cycle          = false;
    bool   orderViolation = false;
    bool   runaway        = false;
    size_t visited        = 0;
    std::string detail;
};

/*
 * Walk one level, checking all three invariants.
 *
 * The visited set is what detects a cycle; the bound is a backstop in case a
 * cycle somehow evades it, so a failure reports rather than hangs the suite.
 */
WalkResult walkLevel(const SkipList& list, int level, size_t bound) {
    WalkResult r;
    std::unordered_set<const void*> seen;

    const Node* x = list.getHead();
    const Node* prev = nullptr;

    while (x != nullptr) {
        if (!seen.insert(static_cast<const void*>(x)).second) {
            r.cycle = true;
            r.detail = "revisited a node at level " + std::to_string(level);
            return r;
        }

        // head_ carries an empty sentinel key, so it is excluded from the
        // ordering comparison.
        if (prev != nullptr && prev != list.getHead()) {
            if (!(prev->key < x->key)) {
                r.orderViolation = true;
                r.detail = "level " + std::to_string(level) + ": key '" + prev->key +
                           "' is not less than following key '" + x->key + "'";
                return r;
            }
        }

        if (++r.visited > bound) {
            r.runaway = true;
            r.detail = "level " + std::to_string(level) + " exceeded " +
                       std::to_string(bound) + " nodes without terminating";
            return r;
        }

        prev = x;
        x = x->next[level].load(std::memory_order_acquire);
    }

    return r;
}

}  // namespace

int main() {
    std::cout << "===========================================\n";
    std::cout << "  Running Unit Test: SkipList Invariants\n";
    std::cout << "===========================================\n";

    size_t totalInserts = 0;

    for (int round = 0; round < kRounds; ++round) {
        Arena arena;
        SkipList list(arena);

        std::vector<std::thread> threads;
        threads.reserve(kThreads);

        for (int t = 0; t < kThreads; ++t) {
            threads.emplace_back([&list, t]() {
                for (int i = 0; i < kInsertsPerThread; ++i) {
                    // Interleaved so concurrent threads land on the same keys at
                    // roughly the same moment, rather than each sweeping its own
                    // contiguous range.
                    const int k = (i * kThreads + t) % kKeySpace;
                    list.insert("key" + std::to_string(k),
                                "v" + std::to_string(t) + "_" + std::to_string(i));
                }
            });
        }
        for (auto& th : threads) th.join();

        totalInserts += static_cast<size_t>(kThreads) * kInsertsPerThread;

        // A level can hold at most kKeySpace distinct keys plus the head, so
        // anything beyond that means the walk is not terminating.
        const size_t bound = static_cast<size_t>(kKeySpace) + 16;

        for (int level = 0; level < SkipList::MAX_HEIGHT; ++level) {
            const WalkResult r = walkLevel(list, level, bound);
            if (r.cycle)          fail("no cycles on any level", r.detail);
            if (r.orderViolation) fail("keys strictly increase on every level", r.detail);
            if (r.runaway)        fail("every level walk terminates", r.detail);
        }

        // Level 0 must hold every distinct key exactly once: it is the level the
        // engine iterates when flushing a memtable, so a gap here is data loss.
        const WalkResult level0 = walkLevel(list, 0, bound);
        const size_t distinctKeys = level0.visited - 1;   // less the head sentinel
        if (distinctKeys != static_cast<size_t>(kKeySpace)) {
            fail("level 0 holds every distinct key",
                 "expected " + std::to_string(kKeySpace) + " keys, walked " +
                 std::to_string(distinctKeys));
        }
    }

    pass("no cycles on any level");
    pass("keys strictly increase on every level");
    pass("every level walk terminates");
    pass("level 0 holds every distinct key");

    std::cout << "\n  " << kRounds << " rounds x "
              << (kThreads * kInsertsPerThread) << " concurrent inserts ("
              << totalInserts << " total) across " << SkipList::MAX_HEIGHT
              << " levels: no cycle, no ordering violation.\n";

    std::cout << "===========================================\n";
    std::cout << "  All SkipList invariant tests passed.\n";
    std::cout << "===========================================\n";
    return 0;
}
