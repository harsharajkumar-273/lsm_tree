#pragma once
// Shared helpers for the correctness tests.
//
// CHECK is used instead of assert(): it is unconditional, so a build that
// defines NDEBUG (CMake's default Release flags do) cannot turn the tests into
// no-ops that "pass" without checking anything. Any file that includes this
// header and is compiled with NDEBUG still verifies its claims.
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <map>
#include <optional>
#include <random>
#include <string>
#include <vector>

#define CHECK(cond)                                                              \
    do {                                                                         \
        if (!(cond)) {                                                           \
            std::fprintf(stderr, "CHECK failed: %s\n  at %s:%d\n", #cond,        \
                         __FILE__, __LINE__);                                    \
            std::abort();                                                        \
        }                                                                        \
    } while (0)

#define CHECK_MSG(cond, msg)                                                     \
    do {                                                                         \
        if (!(cond)) {                                                           \
            std::fprintf(stderr, "CHECK failed: %s\n  %s\n  at %s:%d\n", #cond, \
                         std::string(msg).c_str(), __FILE__, __LINE__);          \
            std::abort();                                                        \
        }                                                                        \
    } while (0)

namespace testutil {

using Model = std::map<std::string, std::string>;

enum class OpKind { PUT, DEL };
struct Op {
    OpKind      kind;
    std::string key;
    std::string value;   // PUT only
};

inline void apply(Model& m, const Op& op) {
    if (op.kind == OpKind::PUT) m[op.key] = op.value;
    else                        m.erase(op.key);
}

inline std::string freshDir(const std::string& name) {
    // Scratch space under the system temp dir, unique per test name.
    const std::string dir = (std::filesystem::temp_directory_path() / ("lsm_" + name)).string();
    std::filesystem::remove_all(dir);
    std::filesystem::create_directories(dir);
    return dir;
}

// Deterministic workload over a small key space so overwrites and deletes of
// live keys are frequent. Value sizes vary so records cross block boundaries.
inline std::vector<Op> makeOps(uint64_t seed, size_t n, size_t keyspace,
                               int del_percent = 25, size_t max_value = 200) {
    std::mt19937_64 rng(seed);
    std::vector<Op> ops;
    ops.reserve(n);
    for (size_t i = 0; i < n; ++i) {
        Op op;
        op.key = "k" + std::to_string(rng() % keyspace);
        if (static_cast<int>(rng() % 100) < del_percent) {
            op.kind = OpKind::DEL;
        } else {
            op.kind = OpKind::PUT;
            op.value = "v" + std::to_string(i) + ":" + std::string(rng() % (max_value + 1), 'x');
        }
        ops.push_back(std::move(op));
    }
    return ops;
}

}  // namespace testutil

namespace testutil {

// Everything the engine reports for keys k0..k{n-1}.
template <class DB>
Model snapshot(const DB& db, size_t keyspace) {
    Model m;
    for (size_t i = 0; i < keyspace; ++i) {
        auto v = db.get("k" + std::to_string(i));
        if (v) m["k" + std::to_string(i)] = *v;
    }
    return m;
}

inline std::string describe(const Model& m) {
    std::string s = "{" + std::to_string(m.size()) + " keys";
    size_t shown = 0;
    for (auto& [k, v] : m) { if (shown++ == 6) { s += " ..."; break; } s += " " + k + "=" + v.substr(0, 8); }
    return s + "}";
}

inline Model prefixState(const std::vector<Op>& ops, size_t n) {
    Model m;
    for (size_t i = 0; i < n && i < ops.size(); ++i) apply(m, ops[i]);
    return m;
}

// Copies a live engine directory (WAL, SSTables, LOCK) so a second engine can
// recover from it: an in-process stand-in for "the process died here" when the
// page cache survives. Call only while no operation is in flight.
inline std::string cloneDir(const std::string& src, const std::string& name) {
    const std::string dst = freshDir(name);
    std::filesystem::copy(src, dst, std::filesystem::copy_options::recursive |
                                    std::filesystem::copy_options::overwrite_existing);
    return dst;
}

}  // namespace testutil
