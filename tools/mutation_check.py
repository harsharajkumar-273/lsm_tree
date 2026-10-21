#!/usr/bin/env python3
"""Mutation check: plant known bugs in the engine and confirm the test suite fails.

Each mutation rewrites one snippet of production code, rebuilds, runs ctest, and
restores the file. A mutation is CAUGHT if at least one test fails, SURVIVED if
the whole suite still passes. Exit status is non-zero if anything survives that
is not listed in KNOWN_UNDETECTABLE.

Usage: tools/mutation_check.py [build_dir] [name_filter]   (filter: substrings separated by "|")
"""
import os, subprocess, sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
BUILD = sys.argv[1] if len(sys.argv) > 1 else os.path.join(ROOT, "build_mut")
FILTER = sys.argv[2] if len(sys.argv) > 2 else ""

# (name, file, old, new)
MUTATIONS = [
    ("compaction keeps OLDEST duplicate", "src/compaction/leveled_compactor.cpp",
     "return sst_index < other.sst_index; // Higher sst_index wins",
     "return sst_index > other.sst_index;"),
    ("get() treats tombstone as miss (deleted keys resurrect)", "src/db/lsm_engine.cpp",
     "if (mem_result.has_value()) {\n        return mem_result.value();",
     "if (mem_result.has_value() && mem_result->has_value()) {\n        return mem_result.value();"),
    ("flush clears WAL before the SSTable is written", "src/db/lsm_engine.cpp",
     "    fault::hit(\"flush.before_sst\");",
     "    wal_.clear();\n    fault::hit(\"flush.before_sst\");"),
    ("compaction publish has no rollback", "src/compaction/leveled_compactor.cpp",
     "            std::filesystem::remove(i < published ? staged[i].second : staged[i].first, ec);",
     "            (void)ec;"),
    ("compaction removes inputs newest-first (tombstone before older value)", "src/compaction/leveled_compactor.cpp",
     "    for (const auto& sst : sstables) {\n        fault::crashPoint(\"compact.mid_remove\");",
     "    for (auto rit = sstables.rbegin(); rit != sstables.rend(); ++rit) {\n        const auto& sst = *rit;\n        fault::crashPoint(\"compact.mid_remove\");"),
    ("put() skips the WAL", "src/db/lsm_engine.cpp",
     "        ticket = wal_.appendPut(key, value);",
     "        ticket = WAL::Ticket{};"),
    ("recovery stops at a hole in the log", "src/wal/wal_reader.cpp",
     "// Hit padding \u2014 skip to next 512-byte boundary\n            offset = ((entry_start / 512) + 1) * 512;\n            continue;",
     "break;"),
    ("recovery accepts a bad CRC", "src/wal/wal_reader.cpp",
     "if (stored_crc != expected_crc) {", "if (false && stored_crc != expected_crc) {"),
    ("SSTables loaded oldest-last (L0 shadows nothing)", "src/db/lsm_engine.cpp",
     "            return a.level > b.level;", "            return a.level < b.level;"),
    ("Durable put returns before write completion/fsync", "src/db/lsm_engine.cpp",
     "    if (wal_.durability() == WAL::Durability::Durable) wal_.waitDurable(ticket);\n}\n\nvoid LSMEngine::del",
     "}\n\nvoid LSMEngine::del"),
    ("fdatasync is never called (only page cache survives SIGKILL)", "src/wal/wal_writer.cpp",
     "|| ::fdatasync(fd) != 0)", "|| false)"),
    ("sync() is a no-op", "src/wal/wal_writer.cpp",
     "void WAL::sync() {\n    Ticket t;", "void WAL::sync() {\n    return;\n    Ticket t;"),
    ("failed WAL completion is swallowed", "src/wal/wal_writer.cpp",
     "    if (!failure_.empty()) {\n        throw std::runtime_error(\"WAL: refusing to write, \" + failure_);\n    }",
     ""),
    ("fdatasync failure is ignored", "src/wal/wal_writer.cpp",
     "if (error.empty() && epoch_.load() == epoch) durable_end_",
     "error.clear();\n        if (error.empty() && epoch_.load() == epoch) durable_end_"),
    ("-ECANCELED is not retried (treated as permanent failure)", "src/wal/wal_writer.cpp",
     "if (pending->attempts < kMaxWriteAttempts) {", "if (false) {"),
    ("-ECANCELED retries are unbounded", "src/wal/wal_writer.cpp",
     "if (pending->attempts < kMaxWriteAttempts) {", "if (true) {"),
    ("retry resubmits at offset 0 instead of the original offset", "src/wal/wal_writer.cpp",
     "io_uring_prep_write(sqe, fd_, p->buf, p->bytes, p->offset);", "io_uring_prep_write(sqe, fd_, p->buf, p->bytes, 0);"),
    ("cancelled attempt is counted as complete", "src/wal/wal_writer.cpp",
     "continue;   // pending_cqes_ unchanged: one in flight before, one after",
     "if (pending_cqes_ > 0) --pending_cqes_;\n                        continue;"),
    ("overwrite no longer replaces the value", "src/memtable/skip_list.h",
     "succs[0]->value.store(Node::makeSlot(arena_, value), std::memory_order_release);",
     ""),
    ("scan() prefers the OLDEST version of a duplicate key", "src/db/lsm_engine.cpp",
     "return a->rank() > b->rank();", "return a->rank() < b->rank();"),
    ("scan() returns tombstones as empty values", "src/db/lsm_engine.cpp",
     "if (value) out.emplace_back(std::move(key), std::move(*value));",
     "out.emplace_back(std::move(key), value.value_or(\"\"));"),
    ("scan() never advances shadowed sources (spins)", "src/db/lsm_engine.cpp",
     "            stale->next();\n", ""),
    ("scan() skips a table whose last key equals start", "src/db/lsm_engine.cpp",
     "if (t.largestKey() < start) continue;", "if (t.largestKey() <= start) continue;"),
    ("scan() ignores the memtable", "src/db/lsm_engine.cpp",
     "    sources.push_back(std::make_unique<MemtableSource>(*memtable_, start, 0));\n", ""),
]

# Mutations that cannot be observed by a user-space SIGKILL test because the page
# cache survives a process crash. Documented in DURABILITY.md; they need power-loss
# testing (e.g. dm-flakey / a VM killed mid-write), which CI cannot do.
KNOWN_UNDETECTABLE = {
    "fdatasync is never called (only page cache survives SIGKILL)",
}

def run(cmd, **kw):
    return subprocess.run(cmd, cwd=ROOT, capture_output=True, text=True, **kw)

def main():
    os.makedirs(BUILD, exist_ok=True)
    r = run(["cmake", "-S", ROOT, "-B", BUILD, "-DCMAKE_BUILD_TYPE=Release"])
    if r.returncode: sys.exit(r.stderr)
    base = run(["cmake", "--build", BUILD, "-j4"])
    if base.returncode: sys.exit(base.stdout + base.stderr)
    base = run(["ctest", "--test-dir", BUILD, "--output-on-failure"], timeout=900)
    if base.returncode:
        sys.exit("baseline suite fails; fix before mutation testing\n" + base.stdout[-3000:])

    survived = []
    for name, rel, old, new in MUTATIONS:
        if FILTER and not any(f in name for f in FILTER.split("|")): continue
        path = os.path.join(ROOT, rel)
        src = open(path).read()
        if src.count(old) != 1:
            sys.exit(f"mutation '{name}': pattern found {src.count(old)} times in {rel}")
        open(path, "w").write(src.replace(old, new))
        try:
            b = run(["cmake", "--build", BUILD, "-j4"])
            if b.returncode:
                print(f"BUILD-ERR  {name}\n{b.stderr[-800:]}")
                survived.append(name); continue
            t = run(["ctest", "--test-dir", BUILD, "--timeout", "300"], timeout=1800)
            failed = [l.split()[3] for l in t.stdout.splitlines() if "***Failed" in l or "Failed" in l and "Test #" in l]
            failed += [l.split()[3] for l in t.stdout.splitlines() if "***Exception" in l or "***Timeout" in l]
            if t.returncode == 0:
                tag = "SURVIVED (known: page cache hides it)" if name in KNOWN_UNDETECTABLE else "SURVIVED"
                print(f"{tag:8} {name}")
                if name not in KNOWN_UNDETECTABLE: survived.append(name)
            else:
                print(f"CAUGHT   {name}  <- {', '.join(sorted(set(failed))) or 'ctest failure'}")
        finally:
            open(path, "w").write(src)
    run(["cmake", "--build", BUILD, "-j4"])
    if survived:
        print("\nSURVIVING MUTATIONS:", *survived, sep="\n  ")
        sys.exit(1)
    print("\nAll mutations caught.")

main()
