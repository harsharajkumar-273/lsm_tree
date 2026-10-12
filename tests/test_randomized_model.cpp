// Randomized put/get/delete sequences checked against a std::map reference.
//
// Covers repeated overwrites, tombstones, memtable flushes (tiny memtable),
// leveled compaction (L0 threshold is 4) and clean reopen, with the engine and
// the reference compared after every operation batch and across reopens.
#include "../src/db/lsm_engine.h"
#include "test_util.h"
#include <iostream>
#include <vector>

using namespace testutil;

static void verifyAll(const LSMEngine& db, const Model& model, const std::vector<std::string>& universe,
                      const std::string& ctx) {
    for (const auto& k : universe) {
        auto got = db.get(k);
        auto it = model.find(k);
        if (it == model.end()) {
            CHECK_MSG(!got.has_value(), ctx + ": key '" + k + "' should be absent but has value '" + *got + "'");
        } else {
            CHECK_MSG(got.has_value(), ctx + ": key '" + k + "' missing, expected '" + it->second + "'");
            CHECK_MSG(*got == it->second, ctx + ": key '" + k + "' stale/wrong value");
        }
    }
}

static void runSeed(uint64_t seed, size_t nops, size_t keyspace, size_t memtable_bytes,
                    WAL::Durability dur) {
    const std::string dir = freshDir("model_" + std::to_string(seed));
    std::vector<std::string> universe;
    for (size_t i = 0; i < keyspace + 5; ++i) universe.push_back("k" + std::to_string(i));  // includes never-written keys

    auto ops = makeOps(seed, nops, keyspace);
    Model model;
    std::mt19937_64 rng(seed ^ 0x9e3779b97f4a7c15ULL);

    auto db = std::make_unique<LSMEngine>(dir, memtable_bytes, dur);
    size_t reopens = 0, flushes = 0;
    for (size_t i = 0; i < ops.size(); ++i) {
        const Op& op = ops[i];
        if (op.kind == OpKind::PUT) db->put(op.key, op.value); else db->del(op.key);
        apply(model, op);

        // Point read of the key just touched, every time.
        auto got = db->get(op.key);
        auto it = model.find(op.key);
        CHECK_MSG(got.has_value() == (it != model.end()), "seed " + std::to_string(seed) + " op " + std::to_string(i) + " presence mismatch for " + op.key);
        if (got) CHECK(*got == it->second);

        const uint64_t r = rng() % 1000;
        if (r < 8) { db->flush(); ++flushes; }
        if (r >= 8 && r < 12) {              // clean close + reopen
            db.reset();
            db = std::make_unique<LSMEngine>(dir, memtable_bytes, dur);
            ++reopens;
            verifyAll(*db, model, universe, "seed " + std::to_string(seed) + " after reopen at op " + std::to_string(i));
        }
        if (i % 97 == 0) verifyAll(*db, model, universe, "seed " + std::to_string(seed) + " periodic at op " + std::to_string(i));
    }
    verifyAll(*db, model, universe, "seed " + std::to_string(seed) + " final");
    db.reset();
    db = std::make_unique<LSMEngine>(dir, memtable_bytes, dur);
    verifyAll(*db, model, universe, "seed " + std::to_string(seed) + " final reopen");
    std::cout << "  seed " << seed << ": " << nops << " ops, " << flushes << " explicit flushes, "
              << reopens << " reopens, sstables=" << db->sstableCount() << " OK\n";
}

// Hammer a single key: every overwrite must win, across flush and compaction.
static void repeatedOverwrite() {
    const std::string dir = freshDir("model_overwrite");
    LSMEngine db(dir, 512);
    for (int i = 0; i < 3000; ++i) {
        db.put("hot", "v" + std::to_string(i));
        if (i % 7 == 0) db.put("pad" + std::to_string(i), std::string(100, 'p'));  // force flushes/compaction
        auto g = db.get("hot");
        CHECK(g && *g == "v" + std::to_string(i));
    }
    db.del("hot");
    CHECK(!db.get("hot").has_value());
    db.flush();
    CHECK(!db.get("hot").has_value());
    std::cout << "  repeated overwrite of one key through flush/compaction OK\n";
}

// A tombstone must keep shadowing older tables until compaction drops both.
static void tombstoneShadowing() {
    const std::string dir = freshDir("model_tomb");
    {
        LSMEngine db(dir, 1 << 20);
        for (int i = 0; i < 50; ++i) db.put("t" + std::to_string(i), "old");
        db.flush();                               // L0 #0 holds the values
        for (int i = 0; i < 50; i += 2) db.del("t" + std::to_string(i));
        db.flush();                               // L0 #1 holds tombstones
        for (int i = 0; i < 50; ++i) CHECK((i % 2 == 0) != db.get("t" + std::to_string(i)).has_value());
        db.put("t0", "resurrected");              // put-after-delete
        CHECK(*db.get("t0") == "resurrected");
        db.flush();
        db.put("a", "1"); db.flush();             // 4th L0 -> compaction drops tombstones
        for (int i = 2; i < 50; ++i) CHECK((i % 2 == 0) != db.get("t" + std::to_string(i)).has_value());
        CHECK(*db.get("t0") == "resurrected");
    }
    LSMEngine db(dir, 1 << 20);
    for (int i = 2; i < 50; ++i) CHECK((i % 2 == 0) != db.get("t" + std::to_string(i)).has_value());
    CHECK(*db.get("t0") == "resurrected");
    std::cout << "  tombstone shadowing + put-after-delete across compaction and reopen OK\n";
}

int main() {
    std::cout << "=== Randomized model test (engine vs std::map) ===\n";
    repeatedOverwrite();
    tombstoneShadowing();
    const size_t kSeeds = 12;
    for (uint64_t s = 1; s <= kSeeds; ++s) {
        runSeed(s, 4000, 120, 2048, s % 2 ? WAL::Durability::Async : WAL::Durability::Durable);
    }
    runSeed(100, 3000, 5000, 64 * 1024, WAL::Durability::Async);   // wide key space, fewer overwrites
    std::cout << "PASS\n";
    return 0;
}
