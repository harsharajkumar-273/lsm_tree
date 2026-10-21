// LSMEngine::scan() checked against a std::map reference.
//
// Covers scans that span the memtable, overlapping L0 tables and compacted L1
// tables; tombstones in a newer layer hiding values in an older one; bounds
// that fall mid-block and between tables; limits; reopen; and scans running
// while writers flush and compact underneath them.
#include "../src/db/lsm_engine.h"
#include "test_util.h"
#include <atomic>
#include <iostream>
#include <thread>

using namespace testutil;
using KV = LSMEngine::KV;

static std::vector<KV> modelScan(const Model& m, const std::string& start,
                                 const std::optional<std::string>& end, size_t limit) {
    std::vector<KV> out;
    for (auto it = m.lower_bound(start); it != m.end() && out.size() < limit; ++it) {
        if (end && !(it->first < *end)) break;
        out.emplace_back(it->first, it->second);
    }
    return out;
}

static void checkScan(const LSMEngine& db, const Model& m, const std::string& start,
                      const std::optional<std::string>& end, size_t limit, const std::string& ctx) {
    const auto got  = db.scan(start, end, limit);
    const auto want = modelScan(m, start, end, limit);
    const std::string range = "[" + start + ", " + (end ? *end : std::string("+inf")) + ") limit " +
                              std::to_string(limit);
    CHECK_MSG(got.size() == want.size(), ctx + " " + range + ": got " + std::to_string(got.size()) +
                                             " entries, want " + std::to_string(want.size()));
    for (size_t i = 0; i < got.size(); ++i) {
        CHECK_MSG(got[i].first == want[i].first,
                  ctx + " " + range + ": key #" + std::to_string(i) + " is '" + got[i].first +
                      "', want '" + want[i].first + "'");
        CHECK_MSG(got[i].second == want[i].second,
                  ctx + " " + range + ": stale value for '" + got[i].first + "'");
    }
}

// Zero-padded so lexicographic order matches numeric order, which makes the
// random bounds below land on, between, and outside real keys.
static std::string key(size_t i) {
    std::string s = std::to_string(i);
    return "k" + std::string(s.size() < 5 ? 5 - s.size() : 0, '0') + s;
}

static void edgeCases() {
    const std::string dir = freshDir("scan_edges");
    LSMEngine db(dir, 1 << 20);

    CHECK(db.scan("").empty());
    db.put("b", "2");
    db.put("a", "1");
    db.put("c", "3");

    CHECK((db.scan("") == std::vector<KV>{{"a", "1"}, {"b", "2"}, {"c", "3"}}));
    CHECK((db.scan("b") == std::vector<KV>{{"b", "2"}, {"c", "3"}}));
    CHECK((db.scan("a", std::string("c")) == std::vector<KV>{{"a", "1"}, {"b", "2"}}));
    CHECK((db.scan("", std::nullopt, 2) == std::vector<KV>{{"a", "1"}, {"b", "2"}}));
    CHECK(db.scan("", std::nullopt, 0).empty());
    CHECK(db.scan("c", std::string("c")).empty());   // empty half-open range
    CHECK(db.scan("c", std::string("a")).empty());   // inverted range
    CHECK(db.scan("d").empty());

    // A value in an SSTable deleted in the memtable must not reappear, and an
    // overwrite in the memtable must shadow the flushed value.
    db.flush();
    db.del("b");
    db.put("c", "3'");
    CHECK((db.scan("") == std::vector<KV>{{"a", "1"}, {"c", "3'"}}));

    // The same with the tombstone flushed into a newer table than the value.
    db.flush();
    CHECK((db.scan("") == std::vector<KV>{{"a", "1"}, {"c", "3'"}}));
    // And with the value re-put above the tombstone.
    db.put("b", "2'");
    CHECK((db.scan("") == std::vector<KV>{{"a", "1"}, {"b", "2'"}, {"c", "3'"}}));

    // Bounds equal to a table's first or last key: start is inclusive, so a
    // table whose largest key is `start` still contributes that key.
    const std::string dir2 = freshDir("scan_edges_bounds");
    LSMEngine db2(dir2, 1 << 20);
    db2.put("m", "1");
    db2.put("p", "2");
    db2.flush();
    CHECK((db2.scan("p") == std::vector<KV>{{"p", "2"}}));
    CHECK((db2.scan("a", std::string("m")).empty()));
    CHECK((db2.scan("m", std::string("n")) == std::vector<KV>{{"m", "1"}}));
    std::cout << "  edge cases OK\n";
}

// A deleted run of keys at the front of the range must not eat into `limit`.
static void limitSkipsTombstones() {
    const std::string dir = freshDir("scan_limit_tombstones");
    LSMEngine db(dir, 1 << 20);
    for (int i = 0; i < 100; ++i) db.put(key(i), "v");
    db.flush();
    for (int i = 0; i < 90; ++i) db.del(key(i));
    const auto got = db.scan("", std::nullopt, 5);
    CHECK(got.size() == 5);
    CHECK(got.front().first == key(90) && got.back().first == key(94));
    std::cout << "  limit counts only live keys OK\n";
}

static void randomized(uint64_t seed) {
    const std::string dir = freshDir("scan_model_" + std::to_string(seed));
    const size_t keyspace = 600;
    // A small memtable forces many flushes and so several compactions; values
    // up to 1.5 KB make tables span many 4 KB blocks, so scan bounds land in
    // the middle of blocks rather than only on their first keys.
    const size_t memtable_bytes = 16 * 1024;

    std::mt19937_64 rng(seed);
    Model model;
    auto db = std::make_unique<LSMEngine>(dir, memtable_bytes);

    auto randomBound = [&]() { return key(rng() % (keyspace + 20)); };
    auto randomScan = [&](const std::string& ctx) {
        std::string start = (rng() % 8 == 0) ? std::string() : randomBound();
        std::optional<std::string> end;
        if (rng() % 4 != 0) end = randomBound();
        const size_t limit = (rng() % 3 == 0) ? 1 + rng() % 50 : std::numeric_limits<size_t>::max();
        checkScan(*db, model, start, end, limit, ctx);
    };

    size_t scans = 0;
    for (size_t i = 0; i < 6000; ++i) {
        const std::string k = key(rng() % keyspace);
        if (rng() % 100 < 25) {
            db->del(k);
            model.erase(k);
        } else {
            std::string v = "v" + std::to_string(i) + ":" + std::string(rng() % 1500, 'x');
            db->put(k, v);
            model[k] = v;
        }
        if (i % 37 == 0) { randomScan("seed " + std::to_string(seed) + " op " + std::to_string(i)); ++scans; }
        if (rng() % 1000 < 3) {
            db.reset();
            db = std::make_unique<LSMEngine>(dir, memtable_bytes);
            checkScan(*db, model, "", std::nullopt, std::numeric_limits<size_t>::max(),
                      "seed " + std::to_string(seed) + " after reopen at op " + std::to_string(i));
        }
    }
    checkScan(*db, model, "", std::nullopt, std::numeric_limits<size_t>::max(),
              "seed " + std::to_string(seed) + " final");

    // A full scan and a point read of every key must agree.
    for (const auto& [k, v] : db->scan("")) {
        auto g = db->get(k);
        CHECK_MSG(g && *g == v, "scan and get disagree on " + k);
    }
    std::cout << "  seed " << seed << ": " << scans << " random scans, sstables="
              << db->sstableCount() << " OK\n";
}

// Scans running against writers that flush and compact. Each result must be
// strictly increasing and stay inside its bounds; the values themselves race
// with the writers, so only their format is checked.
static void concurrentScans() {
    const std::string dir = freshDir("scan_concurrent");
    LSMEngine db(dir, 8 * 1024);
    std::atomic<bool> stop{false};
    std::atomic<size_t> scans{0};

    std::vector<std::thread> writers;
    for (int w = 0; w < 2; ++w) {
        writers.emplace_back([&, w] {
            std::mt19937_64 rng(1000 + w);
            for (int i = 0; i < 4000; ++i) {
                const std::string k = key(rng() % 500);
                if (rng() % 5 == 0) db.del(k);
                else db.put(k, "w" + std::to_string(w) + "-" + std::to_string(i));
            }
        });
    }
    std::thread reader([&] {
        std::mt19937_64 rng(7);
        while (!stop.load()) {
            const std::string start = key(rng() % 500);
            const std::string end = key(rng() % 500 + 50);
            const auto got = db.scan(start, end);
            for (size_t i = 0; i < got.size(); ++i) {
                CHECK(got[i].first >= start && got[i].first < end);
                CHECK(i == 0 || got[i - 1].first < got[i].first);
                CHECK(!got[i].second.empty() && got[i].second[0] == 'w');
            }
            ++scans;
        }
    });
    for (auto& t : writers) t.join();
    stop = true;
    reader.join();
    std::cout << "  " << scans.load() << " scans concurrent with writers OK\n";
}

int main() {
    std::cout << "test_range_scan\n";
    edgeCases();
    limitSkipsTombstones();
    for (uint64_t seed : {1, 2, 3}) randomized(seed);
    concurrentScans();
    std::cout << "all range scan tests passed\n";
    return 0;
}
