// Process-crash recovery: a child engine is SIGKILLed at an injected fault point
// (or torn on disk afterwards) and a fresh engine must recover a state allowed by
// the durability contract in DURABILITY.md.
//
// Failure model: the process dies abruptly - no destructors, no final flush.
// The kernel (and so the page cache) survives. Power-loss behaviour cannot be
// exercised from user space; the contract states what it relies on instead.
//
// Allowed recovered states, with N ops issued, A = ops whose call returned,
// S = ops covered by the last sync() that returned:
//   Durable mode : prefix(A) or prefix(A+1)           - exactly, nothing lost
//   Async mode   : every op < S present; any subset of ops in [S, A+1) may be
//                  missing, but nothing outside the op history may appear
#include "../src/db/lsm_engine.h"
#include "../src/util/fault.h"
#include "../src/wal/wal.h"
#include "test_util.h"
#include <fcntl.h>
#include <iostream>
#include <sys/mman.h>
#include <sys/wait.h>
#include <unistd.h>

using namespace testutil;

struct Progress {            // lives in MAP_SHARED memory, survives the child
    volatile uint64_t acked;     // ops whose put/del returned
    volatile uint64_t synced;    // ops covered by a returned sync()
    volatile uint64_t started;   // ops whose call began
};

static Progress* sharedProgress() {
    void* p = mmap(nullptr, sizeof(Progress), PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANONYMOUS, -1, 0);
    CHECK(p != MAP_FAILED);
    auto* pr = static_cast<Progress*>(p);
    pr->acked = pr->synced = pr->started = 0;
    return pr;
}

struct Case {
    const char* point;
    uint64_t nth;
    size_t keyspace, nops, memtable;
    int sync_every;       // Async: call sync() after this many ops (0 = never)
};

// Child body. Never returns.
[[noreturn]] static void childRun(const std::string& dir, const std::vector<Op>& ops, const Case& c,
                                  WAL::Durability dur, Progress* pr) {
    LSMEngine db(dir, c.memtable, dur);
    if (c.point) fault::arm(c.point, c.nth, fault::Action::Crash);
    for (size_t i = 0; i < ops.size(); ++i) {
        pr->started = i + 1;
        if (ops[i].kind == OpKind::PUT) db.put(ops[i].key, ops[i].value); else db.del(ops[i].key);
        pr->acked = i + 1;
        if (dur == WAL::Durability::Async && c.sync_every && (i + 1) % c.sync_every == 0) {
            db.sync();
            pr->synced = i + 1;
        }
    }
    _exit(0);   // finished without the fault firing: still no destructor / flush
}

// Is `got` reachable under the Async rule? Per key: take the last op < S touching
// the key (the guaranteed state), and allow the state after any later op < upper.
static bool asyncAllowed(const Model& got, const std::vector<Op>& ops, size_t S, size_t upper,
                         size_t keyspace, std::string* why) {
    for (size_t k = 0; k < keyspace; ++k) {
        const std::string key = "k" + std::to_string(k);
        std::vector<std::optional<std::string>> allowed;
        std::optional<std::string> state;
        bool have_base = false;
        for (size_t i = 0; i < upper && i < ops.size(); ++i) {
            if (ops[i].key != key) continue;
            state = ops[i].kind == OpKind::PUT ? std::optional<std::string>(ops[i].value) : std::nullopt;
            if (i < S) { allowed.clear(); allowed.push_back(state); have_base = true; }
            else       { if (!have_base) { allowed.push_back(std::nullopt); have_base = true; } allowed.push_back(state); }
        }
        if (!have_base) allowed.push_back(std::nullopt);
        auto it = got.find(key);
        std::optional<std::string> actual = it == got.end() ? std::nullopt : std::optional<std::string>(it->second);
        bool ok = false;
        for (auto& a : allowed) if (a == actual) ok = true;
        if (!ok) {
            *why = "key " + key + " recovered as " + (actual ? "'" + actual->substr(0, 12) + "...'" : std::string("<absent>")) +
                   " which no permitted history yields (S=" + std::to_string(S) + ", upper=" + std::to_string(upper) + ")";
            return false;
        }
    }
    return true;
}

static void runCrashCase(const Case& c, uint64_t seed, WAL::Durability dur) {
    const std::string tag = std::string(c.point ? c.point : "end") + "#" + std::to_string(c.nth) + "/seed" +
                            std::to_string(seed) + (dur == WAL::Durability::Durable ? "/durable" : "/async");
    const std::string dir = freshDir("crash_" + std::to_string(seed) + "_" + (c.point ? c.point : "end"));
    auto ops = makeOps(seed, c.nops, c.keyspace, 25, 120);
    Progress* pr = sharedProgress();

    pid_t pid = fork();
    CHECK(pid >= 0);
    if (pid == 0) childRun(dir, ops, c, dur, pr);
    int status = 0;
    CHECK(waitpid(pid, &status, 0) == pid);
    const bool killed = WIFSIGNALED(status) && WTERMSIG(status) == SIGKILL;
    CHECK_MSG(killed || (WIFEXITED(status) && WEXITSTATUS(status) == 0), tag + ": child failed for a reason other than the injected crash");

    const size_t A = pr->acked, S = pr->synced, started = pr->started;
    LSMEngine db(dir, c.memtable, dur);
    Model got = snapshot(db, c.keyspace);

    if (dur == WAL::Durability::Durable) {
        const size_t upper = std::min<size_t>(started, ops.size());   // the in-flight op may have landed
        bool ok = false;
        for (size_t n = A; n <= upper && !ok; ++n) ok = (got == prefixState(ops, n));
        CHECK_MSG(ok, tag + ": Durable recovery is not prefix(" + std::to_string(A) + ") or prefix(" +
                          std::to_string(upper) + "); recovered " + describe(got) + " vs acked-prefix " +
                          describe(prefixState(ops, A)));
    } else {
        std::string why;
        CHECK_MSG(asyncAllowed(got, ops, S, std::min<size_t>(started, ops.size()), c.keyspace, &why), tag + ": " + why);
    }
    munmap(pr, sizeof(Progress));
}

// ---- torn / damaged log files, produced offline --------------------------------

static size_t recordBytes(const Op& op) {
    return 1 + 4 + 4 + op.key.size() + (op.kind == OpKind::PUT ? op.value.size() : 0) + 4;
}
static size_t alignedBytes(const Op& op) { return (recordBytes(op) + 511) & ~size_t(511); }

static void writeWal(const std::string& dir, const std::vector<Op>& ops) {
    WAL wal(dir + "/wal.log", WAL::Durability::Durable);
    for (auto& op : ops) { if (op.kind == OpKind::PUT) wal.logPut(op.key, op.value); else wal.logDel(op.key); }
}

static void tornTail() {
    std::mt19937_64 rng(77);
    auto ops = makeOps(5, 60, 12, 30, 400);
    for (int trial = 0; trial < 40; ++trial) {
        const std::string dir = freshDir("torn_tail");
        writeWal(dir, ops);
        uintmax_t total = std::filesystem::file_size(dir + "/wal.log");
        uintmax_t cut = rng() % (total + 1);
        std::filesystem::resize_file(dir + "/wal.log", cut);

        // Records whose body lies entirely inside the kept bytes survive; a record cut
        // anywhere inside its body must be rejected, never half-applied.
        size_t m = 0; uintmax_t off = 0;
        for (; m < ops.size(); ++m) { if (off + recordBytes(ops[m]) > cut) break; off += alignedBytes(ops[m]); }
        LSMEngine db(dir);
        CHECK_MSG(snapshot(db, 12) == prefixState(ops, m), "torn tail at byte " + std::to_string(cut) + ": expected exactly prefix(" + std::to_string(m) + ")");
    }
    std::cout << "  torn WAL tail (40 random truncation points): recovers exactly the intact prefix OK\n";
}

// A record that never reached the disk while later ones did (io_uring may complete
// writes out of order, so Async mode permits this): recovery must skip the hole and
// still apply everything after it.
static void holeInTheMiddle() {
    auto ops = makeOps(6, 50, 10, 30, 300);
    std::mt19937_64 rng(9);
    for (int trial = 0; trial < 20; ++trial) {
        const std::string dir = freshDir("torn_hole");
        writeWal(dir, ops);
        size_t victim = rng() % ops.size();
        uintmax_t off = 0;
        for (size_t i = 0; i < victim; ++i) off += alignedBytes(ops[i]);
        int fd = open((dir + "/wal.log").c_str(), O_WRONLY);
        CHECK(fd >= 0);
        std::string zeros(alignedBytes(ops[victim]), '\0');
        CHECK(pwrite(fd, zeros.data(), zeros.size(), off) == static_cast<ssize_t>(zeros.size()));
        close(fd);

        std::vector<Op> without = ops; without.erase(without.begin() + victim);
        LSMEngine db(dir);
        CHECK_MSG(snapshot(db, 10) == prefixState(without, without.size()), "hole at record " + std::to_string(victim) + " lost or reordered later records");
    }
    std::cout << "  zeroed record in the middle of the WAL: later records still recovered OK\n";
}

// A flipped bit anywhere in a record must never yield a wrong value: the record is
// dropped (CRC / length / type check) and every other record is unaffected. A flip
// in the zero padding after a record changes nothing.
static void bitFlips() {
    auto ops = makeOps(8, 40, 10, 30, 250);
    std::mt19937_64 rng(31);
    int in_record = 0, in_padding = 0;
    for (int trial = 0; trial < 600; ++trial) {
        const std::string dir = freshDir("bitflip");
        writeWal(dir, ops);
        size_t victim = rng() % ops.size();
        uintmax_t start = 0;
        for (size_t i = 0; i < victim; ++i) start += alignedBytes(ops[i]);
        const size_t body = recordBytes(ops[victim]), span = alignedBytes(ops[victim]);
        const size_t pos = rng() % span;
        int fd = open((dir + "/wal.log").c_str(), O_RDWR);
        CHECK(fd >= 0);
        char c; CHECK(pread(fd, &c, 1, start + pos) == 1);
        c ^= static_cast<char>(1u << (rng() % 8));
        CHECK(pwrite(fd, &c, 1, start + pos) == 1);
        close(fd);

        std::vector<Op> expected = ops;
        if (pos < body) { expected.erase(expected.begin() + victim); ++in_record; } else ++in_padding;
        LSMEngine db(dir);
        CHECK_MSG(snapshot(db, 10) == prefixState(expected, expected.size()),
                  "bit flip at byte " + std::to_string(pos) + " of record " + std::to_string(victim) +
                  (pos < body ? " (inside record) was accepted or damaged other records" : " (padding) changed state"));
    }
    std::cout << "  600 single-bit flips (" << in_record << " in records, " << in_padding << " in padding): damaged record dropped, rest intact OK\n";
}

int main() {
    std::cout << "=== Crash recovery: SIGKILL at fault points, torn logs ===\n";
    tornTail();
    holeInTheMiddle();
    bitFlips();

    const std::vector<Case> cases = {
        {"wal.before_append",     40,  60, 500, 1 << 20, 0},
        {"wal.after_append",      40,  60, 500, 1 << 20, 25},
        {"wal.after_append",      330, 60, 500, 2048,    0},
        {"flush.before_sst",      1,   60, 500, 2048,    0},
        {"flush.after_sst",       1,   60, 500, 2048,    0},
        {"flush.after_sst",       3,   60, 500, 2048,    10},
        {"flush.after_wal_clear", 2,   60, 500, 2048,    0},
        {"compact.after_merge",   1,   60, 800, 1024,    0},
        {"compact.mid_publish",   1,   2600, 9000, 24 * 1024, 0},
        {"compact.mid_publish",   2,   2600, 9000, 24 * 1024, 0},
        {"compact.after_publish", 1,   60, 800, 1024,    0},
        {"compact.after_publish", 2,   60, 800, 1024,    0},
        {"compact.mid_remove",    1,   60, 800, 1024,    0},
        {"compact.mid_remove",    3,   60, 800, 1024,    0},
        {"compact.mid_remove",    6,   60, 800, 1024,    0},
        {"compact.after_remove",  1,   60, 800, 1024,    0},
        {"compact.after_remove",  2,   60, 800, 1024,    0},
        {"wal.before_fdatasync",  17,  60, 300, 1 << 20, 0},
        {"wal.after_fdatasync",   17,  60, 300, 1 << 20, 0},
        {nullptr,                 0,   60, 400, 2048,    20},    // no fault: crash at end of run
    };
    int n = 0;
    for (const auto& c : cases)
        for (uint64_t seed = 1; seed <= 3; ++seed)
            for (auto dur : {WAL::Durability::Async, WAL::Durability::Durable}) {
                // wal.*fdatasync only exists in Durable mode or after sync()
                if (c.point && std::string(c.point).find("fdatasync") != std::string::npos && dur == WAL::Durability::Async && !c.sync_every) continue;
                runCrashCase(c, seed + c.nth * 13, dur);
                ++n;
            }
    std::cout << "  " << n << " SIGKILL scenarios recovered to a permitted state OK\n";
    std::cout << "PASS\n";
    return 0;
}
