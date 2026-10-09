# Durability contract

What a caller may rely on, what is tested, and what is not. Every statement
below is either enforced by a test named next to it or listed under
[Not covered](#not-covered).

## Failure model

| Failure | Page cache | Covered by |
|---|---|---|
| Process crash (`SIGKILL`, abort, OOM kill) | survives | `test_crash_recovery`, `test_concurrent_durable` |
| Write/sync returns an error (`EIO`, `ENOSPC`, failed `fdatasync`) | n/a | `test_durability_faults` |
| Torn or partially written log tail | n/a | `test_crash_recovery` (truncation, zeroed records) |
| Kernel crash / power loss | **lost** | *not testable from user space* — relies on `fdatasync` semantics |

## Acknowledgment levels

`LSMEngine(dir, memtable_bytes, durability)` and `WAL(path, durability)` take a
`WAL::Durability`.

### `Async` (default)

`put()`/`del()` return once the WAL record has been **submitted** to `io_uring`
and applied to the memtable.

- The record may still be in flight. After a process crash it can be missing.
- After power loss anything since the last `sync()` (or flush) can be missing.
- Write errors are reported **late**: a failed or short completion is noticed on
  a later `put()`/`del()` (which throws and refuses to append to a log with a
  hole) or on `sync()`. A `put()` that returned normally is therefore not proof
  its record reached the log.
- `sync()` is the barrier: when it returns, every operation that returned before
  the call is on stable storage (WAL `fdatasync`ed, or already in an `fsync`ed
  SSTable).

**Recovered state after a crash.** Let `S` be the operations covered by the last
`sync()` that returned and `A` those whose call returned. Then, per key, the
recovered value is the one produced by the last operation `< S` on that key, or
by any later operation `< A+1` on it. Nothing outside the key's history can
appear. Two consequences worth knowing:

1. Operations after `S` may be lost **individually**, not just as a suffix:
   `io_uring` gives no ordering between completions, so record *n+1* can reach
   the log while record *n* does not. Recovery skips the hole and applies later
   records. (`test_crash_recovery`: zeroed record in the middle.)
2. A key overwritten twice after `S` may recover to its older value.

Enforced by `test_crash_recovery` (SIGKILL at 20 fault points × 3 seeds).

### `Durable`

`put()`/`del()` return only after the record's write **completed** and an
`fdatasync` covering it finished. If either fails the call throws and the outcome
of that one operation is unknown (it may or may not be recovered).

**Recovered state after a crash:** exactly `prefix(A)` or `prefix(A+1)` — every
acknowledged operation, in order, plus possibly the one that was in flight. No
holes: a record is acknowledged only after its own write completed, and records
are acknowledged in submission order per `fdatasync` batch.

Enforced by `test_crash_recovery` (same scenarios, exact prefix comparison) and
`test_concurrent_durable` (8 writers, SIGKILL at random times, every
acknowledged write must be present).

**Group commit.** The engine lock is released before waiting for `fdatasync`;
the first waiter becomes leader, drains completions, and issues one `fdatasync`
that covers every record appended so far. Concurrent writers share it
(`LSMEngine::walSyncCount()` exposes the count). A write is visible to readers
slightly before it is durable; the *caller* is not told it succeeded until it is.

## Flush, compaction and recovery invariants

1. **Flush order.** The SSTable is written to `*.tmp`, `fsync`ed, renamed, and the
   directory `fsync`ed *before* the WAL is cleared. A crash at any point leaves
   the entries in the WAL, in an SSTable, or both; replaying the WAL over an
   SSTable that already holds the same entries is idempotent.
   (`flush.before_sst`, `flush.after_sst`, `flush.after_wal_clear` in both tests.)
2. **Compaction publish.** Outputs are written as `.tmp`, `fsync`ed, renamed to
   new `sst-1-N` names (numbered past everything *on disk*, not just in memory),
   then inputs are removed **L1 first, then L0**. At any crash point the surviving
   files resolve to the same key/value state as before the compaction: new L1
   files hold exactly the merge winners, and a tombstone in an L0 input is never
   removed while an older value it shadows survives.
   (`compact.after_merge`, `mid_publish`, `after_publish`, `mid_remove`,
   `after_remove`.)
3. **Failed compaction leaves no residue.** If publishing throws part-way, the
   renamed outputs are removed and the in-memory table list is unchanged.
   Previously they stayed on disk and, after a later successful compaction,
   shadowed it on reopen.
4. **A thrown `put()` has unknown outcome**, including when the throw comes from a
   flush or compaction triggered by the call. The engine stays consistent and
   readable afterwards; the key reads as its old or new state, never anything
   else. (`test_durability_faults`: 76 scenarios.)
5. **A WAL write failure is cleared by a successful flush.** The failed log is
   discarded once its contents are durable in an SSTable; the engine does not
   stay write-disabled forever.
6. **Torn tail.** Truncating the log at any byte recovers exactly the records
   whose bodies are intact. (`test_crash_recovery`)

## Not covered

- **Power loss / kernel crash.** A `SIGKILL` test cannot observe whether
  `fdatasync` was actually issued or whether the device cache was flushed, because
  the page cache survives. `tools/mutation_check.py` lists the mutation that
  removes the `fdatasync` call as *known undetectable*. Closing this needs a
  VM-level test (kill the VM mid-write) or `dm-flakey`/`dm-log-writes`.
- Disk bit-rot in SSTables (only the footer and lengths are validated; data blocks
  carry no checksum).
- Multiple processes (the directory lock refuses them).
- `O_DIRECT` + `fdatasync` on filesystems that ignore either; `ext4` assumed.
