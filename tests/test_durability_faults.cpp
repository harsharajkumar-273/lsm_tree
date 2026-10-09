// Fault injection around WAL writes, memtable flushes and compaction.
//
// Each scenario arms one fault point, drives a random workload, and compares the
// engine against a reference map under the contract in DURABILITY.md:
//
//   * an operation whose call returned normally has its documented guarantee;
//   * an operation whose call threw has UNKNOWN outcome - it may or may not have
//     taken effect, but the engine must then be internally consistent: reading
//     the key shows either the old or the new state, nothing else;
//   * after the fault, the engine keeps working, and a clean reopen and a
//     "process died here" snapshot both recover exactly what the live engine
//     reported.
#include "../src/db/lsm_engine.h"
#include "../src/util/fault.h"
#include "test_util.h"
#include <iostream>
#include <thread>

using namespace testutil;

struct Config {
    const char* name;
    size_t keyspace, nops, memtable;
    int max_value;
};
static const Config kSmall = {"small", 40, 700, 1024, 200};
// >1000 live keys so a compaction writes several L1 files and "mid publish" exists.
static const Config kWide  = {"wide", 2600, 9000, 24 * 1024, 16};

static void applyOp(LSMEngine& db, const Op& op) {
    if (op.kind == OpKind::PUT) db.put(op.key, op.value); else db.del(op.key);
}

static void expectEqual(const Model& got, const Model& want, const std::string& ctx) {
    CHECK_MSG(got == want, ctx + ": engine state " + describe(got) + " != expected " + describe(want));
}

// Runs one scenario. Returns how many times the fault fired (0 = point not reached).
static int runThrowScenario(const Config& cfg, const char* point, uint64_t nth, uint64_t seed,
                            WAL::Durability dur) {
    const std::string ctx = std::string(cfg.name) + "/" + point + "#" + std::to_string(nth) +
                            "/seed" + std::to_string(seed) + (dur == WAL::Durability::Durable ? "/durable" : "/async");
    const std::string dir = freshDir("fault_" + std::to_string(seed) + "_" + point);
    auto ops = makeOps(seed, cfg.nops, cfg.keyspace, 25, cfg.max_value);

    Model model;
    int fired = 0;
    {
        LSMEngine db(dir, cfg.memtable, dur);
        fault::arm(point, nth, fault::Action::Throw);
        for (size_t i = 0; i < ops.size(); ++i) {
            const Op& op = ops[i];
            try {
                applyOp(db, op);
                apply(model, op);
            } catch (const fault::Injected&) {
                ++fired;
                // Unknown outcome: the key must read as either its old or new state.
                Model after = model; apply(after, op);
                auto live = db.get(op.key);
                auto old_it = model.find(op.key), new_it = after.find(op.key);
                const bool is_old = (live.has_value() == (old_it != model.end())) && (!live || *live == old_it->second);
                const bool is_new = (live.has_value() == (new_it != after.end())) && (!live || *live == new_it->second);
                CHECK_MSG(is_old || is_new, ctx + ": after a thrown " + op.key + " the live value matches neither state");
                if (!is_old) model = after;   // adopt what the engine shows
            }
            // Idle-point "died here" check: a copy of the directory must recover
            // the same state the live engine reports.
            if (i % (cfg.nops / 5) == cfg.nops / 5 - 1) {
                fault::Pause no_faults;
                db.sync();
                const std::string clone = cloneDir(dir, "fault_clone_" + std::to_string(seed));
                LSMEngine recovered(clone, cfg.memtable, dur);
                expectEqual(snapshot(recovered, cfg.keyspace), model, ctx + " [snapshot @op " + std::to_string(i) + "]");
            }
        }
        fault::disarmAll();
        expectEqual(snapshot(db, cfg.keyspace), model, ctx + " [live after fault]");
    }   // clean close
    {
        LSMEngine db(dir, cfg.memtable, dur);
        expectEqual(snapshot(db, cfg.keyspace), model, ctx + " [reopen]");
        // Still writable and still correct after the fault and the reopen.
        db.put("k0", "post"); model["k0"] = "post";
        db.flush();
        expectEqual(snapshot(db, cfg.keyspace), model, ctx + " [write after reopen]");
    }
    return fired;
}

// Async mode reports a failed WAL completion on a later call. The contract:
// sync() (or the next append) throws; nothing is silently swallowed; flush()
// makes the memtable durable regardless and the engine recovers.
static void walCompletionError() {
    for (const char* point : {"wal.cqe_error", "wal.short_write"}) {
        const std::string dir = freshDir("fault_cqe");
        Model model;
        {
            LSMEngine db(dir, 1 << 20, WAL::Durability::Async);
            fault::arm(point, 3, fault::Action::Fail);
            bool surfaced = false;
            for (int i = 0; i < 40 && !surfaced; ++i) {
                try {
                    db.put("k" + std::to_string(i), "v" + std::to_string(i));
                    model["k" + std::to_string(i)] = "v" + std::to_string(i);
                } catch (const std::runtime_error&) { surfaced = true; }
            }
            if (!surfaced) {
                try { db.sync(); } catch (const std::runtime_error&) { surfaced = true; }
            }
            fault::disarmAll();
            CHECK_MSG(surfaced, std::string(point) + ": a failed WAL completion was never reported");

            // The log is untrustworthy until flushed; further appends are refused...
            bool refused = false;
            try { db.put("kx", "vx"); } catch (const std::runtime_error&) { refused = true; }
            CHECK_MSG(refused, std::string(point) + ": appended to a log known to have a hole");

            // ...and flushing writes the (memory-resident) state to an SSTable, discards the
            // damaged log, and restores service.
            db.flush();
            db.put("after", "ok"); model["after"] = "ok";
            db.sync();
            for (auto& [k, v] : model) CHECK(db.get(k) == v);
        }
        LSMEngine db(dir, 1 << 20);
        for (auto& [k, v] : model) { auto g = db.get(k); CHECK_MSG(g && *g == v, std::string(point) + ": lost " + k + " after recovery"); }
    }
    std::cout << "  WAL completion error / short write: reported, log refused, flush restores service OK\n";
}

// Durable mode must not acknowledge a write whose fdatasync failed.
static void fdatasyncFailure() {
    const std::string dir = freshDir("fault_fdatasync");
    LSMEngine db(dir, 1 << 20, WAL::Durability::Durable);
    db.put("a", "1");
    fault::arm("wal.fdatasync_error", 1, fault::Action::Fail);
    bool threw = false;
    try { db.put("b", "2"); } catch (const std::runtime_error&) { threw = true; }
    fault::disarmAll();
    CHECK_MSG(threw, "Durable put returned success although fdatasync failed");
    db.put("c", "3");                         // a later successful sync covers it
    CHECK(db.get("a") == std::string("1"));
    std::cout << "  Durable-mode put throws when fdatasync fails OK\n";
}

int main() {
    std::cout << "=== Fault injection: WAL, flush, compaction ===\n";
    walCompletionError();
    fdatasyncFailure();

    struct P { const char* point; std::vector<uint64_t> nths; const Config* cfg; const char* cfg_name() const { return cfg->name; } };
    const std::vector<P> points = {
        {"wal.before_append",       {1, 9, 120},     &kSmall},
        {"wal.after_append",        {1, 9, 120},     &kSmall},
        {"flush.before_sst",        {1, 3},          &kSmall},
        {"flush.after_sst",         {1, 2, 5},       &kSmall},
        {"flush.after_wal_clear",   {1, 4},          &kSmall},
        {"compact.after_merge",     {1, 2},          &kSmall},
        {"compact.after_publish",   {1, 2},          &kSmall},
        {"compact.mid_publish",     {1, 2},          &kWide},
        {"compact.after_merge",     {1},             &kWide},
        {"compact.after_publish",   {1},             &kWide},
    };
    int scenarios = 0, fired_total = 0;
    for (const auto& p : points)
        for (uint64_t nth : p.nths)
            for (uint64_t seed = 1; seed <= (p.cfg == &kWide ? 1u : 2u); ++seed)
                for (auto dur : {WAL::Durability::Async, WAL::Durability::Durable}) {
                    int f = runThrowScenario(*p.cfg, p.point, nth, seed * 7 + nth, dur);
                    ++scenarios; fired_total += f;
                    CHECK_MSG(f > 0 || nth > 3, std::string(p.cfg_name()) + "/" + p.point + " seed=" + std::to_string(seed) + (dur==WAL::Durability::Durable?" durable":" async") + std::string(p.point) + " nth=" + std::to_string(nth) + " never fired - the scenario tests nothing");
                }
    std::cout << "  " << scenarios << " throw-injection scenarios, fault fired in " << fired_total << " operations OK\n";
    std::cout << "PASS\n";
    return 0;
}
