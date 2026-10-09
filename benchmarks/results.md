# Benchmark results

## 1. Write path: submission vs durable throughput

Produced by [`bench_durability`](bench_durability.cpp); raw per-run data is in
[`results_durability_raw.csv`](results_durability_raw.csv). Reproduce with:

```bash
cmake -B build && cmake --build build -j
./build/bench_durability            # full run, ~15 min; --quick for a smoke run
```

### What is being compared

Earlier numbers in this repo put an `io_uring` WAL that returns on *submission*
next to a `write()+fdatasync` WAL that returns on *durability*, then quoted a
"100x speedup". Those are different promises, so this harness separates them:

| column | meaning |
|---|---|
| **ack level** | what `put()` returning promises. `submitted`: handed to the kernel, not durable. `durable`: write completed **and** `fdatasync` finished. Only compare rows with the same ack level. |
| **ack throughput** | `ops / time until the last put() returned` |
| **drain** | extra time after the last `put()` until every record is completed **and** `fdatasync`ed (`WAL::sync()` / final `fdatasync`) |
| **durable throughput** | `ops / (ack + drain)`. Every implementation has made the same promise by then, so this column **is** comparable across all rows. |

Implementations: the engine's `io_uring + O_DIRECT` WAL in both modes
(`WAL::Durability::Async` / `Durable`, see [`DURABILITY.md`](../DURABILITY.md)); the
same via `LSMEngine::put` (memtable large enough that nothing flushes); and three
conventional baselines that write the identical record format with a real CRC32:
buffered `write()` (+ one `fdatasync` at the end for the durable column),
`write()+fdatasync` after every record, and `O_DSYNC`.

Workers stay alive until the drain is measured: an `io_uring` task that exits waits
for its in-flight requests, which would otherwise hide completion time inside the
ack window.

## Environment
- CPU: model name	: Intel(R) Xeon(R) Processor @ 2.80GHz (4 hardware threads)
- RAM: MemTotal:       16480952 kB
- Kernel: Linux 6.18.44-fc-v80
- Storage under /tmp/lsm_bench_full: /dev/vda (ext4), rotational=1
- Build: compiler 13.3.0, CMAKE_BUILD_TYPE=Release (-O3)
- fdatasync floor (512 B append + fdatasync, median of 300): 245.034 us
- Per config: 10000 ops, 16-byte keys, 5 fresh-directory runs; values of the sizes shown


Each cell is the median of 5 runs, each in a freshly created directory, with
(min–max) in parentheses. Latency percentiles pool all runs. "log bytes / logical"
is on-disk WAL size over key+value bytes: the `O_DIRECT` log pads every record to
512 bytes.

### Findings

1. **Submission throughput is not write throughput.** The `io_uring` WAL with
   `ack=submitted` acknowledges 18k–128k records/s depending on record size, but the
   kernel then needs about **1 second to complete 10,000 of them at every record
   size** (drain column: 0.8–1.5 s). Counting completion, sustained throughput is
   **5.2k–10.6k ops/s**, i.e. 15x below the ack figure for 16-byte values and 3x
   below it for 4 KB. The previous README's "254,095 writes/sec" was a submission
   rate measured before the drain.
2. **On this machine `io_uring + O_DIRECT` is not faster than conventional I/O at
   any matched level.** Buffered `write()` plus one final `fdatasync` (same
   promise: everything durable after the barrier) reaches 664k / 333k / 72k / 21k
   durable ops/s at 16 B / 128 B / 1 KB / 4 KB values, vs 8.2k / 8.7k / 7.2k / 5.9k
   for the `io_uring` WAL: 80x down to 3.5x. The drain implies ~100 µs per record
   for `O_DIRECT` appends; the cause was not profiled (extending `O_DIRECT` writes
   on ext4 are a likely one). Small records are also padded to 512 B, which is the
   16x write amplification in the last column.
3. **Per-record durability (`ack=durable`).** With one writer there is nothing to
   batch, and group commit is slower than a plain `write()+fdatasync` (1.5k–2.4k vs
   2.0k–3.5k ops/s) because each record pays an `O_DIRECT` completion *and* an
   `fdatasync`. With 8 concurrent writers it overtakes both durable baselines, which
   serialise on the device flush: **5.7k–5.9k vs 3.4k–3.8k ops/s at 16–128 B, 5.0k
   vs 3.0k–3.3k at 1 KB, 4.2k vs 1.8k at 4 KB** (1.5x–2.4x). Tail latency is much
   better (p99.9 6–9 ms vs 490–565 ms at small sizes) but median latency is worse
   (1.3 ms vs 0.25 ms): group commit trades per-call latency for throughput.
4. **The `fdatasync` floor here is ~245 µs**, so about 4k durable ops/s is the
   ceiling for anything that flushes once per record.

Caveats: one machine, a virtualised disk (`/dev/vda`, ext4; the kernel reports
`rotational=1` but the flush cost suggests a network/virtio-backed volume), 4
vCPUs, and a single dataset size of 10,000 operations (0.3–40 MB logical). Results
on local NVMe with a volatile write cache will differ, mainly in the `fdatasync`
cost.

### 1 thread(s), 16 B values (10000 ops, 312 KiB logical)

| implementation | ack level | ack throughput (ops/s) | drain (ms) | durable throughput (ops/s) | p50 / p99 / p99.9 put (us) | log bytes / logical |
|---|---|---:|---:|---:|---|---:|
| io_uring+O_DIRECT WAL, ack=submitted | submitted | 124854 (77728–144711) | 1143.6 (937.5–1168.1) | **8226** (8082–9379) | 5.1 / 52.5 / 144.2 | 16.0x |
| write() WAL, ack=submitted (page cache) | submitted | 783207 (622980–822997) | 2.3 (1.9–2.4) | **664181** (557430–697778) | 1.0 / 3.2 / 26.6 | 1.4x |
| io_uring+O_DIRECT WAL, ack=durable (group commit) | durable | 1819 (1639–1956) | 0.0 (0.0–0.0) | **1819** (1639–1956) | 493.8 / 1656.2 / 3915.0 | 16.0x |
| write()+fdatasync per record, ack=durable | durable | 2629 (2585–3518) | 0.1 (0.1–0.2) | **2628** (2585–3518) | 335.0 / 832.6 / 2702.8 | 1.4x |
| O_DSYNC write(), ack=durable | durable | 3465 (3369–3538) | 0.1 (0.1–0.2) | **3465** (3369–3538) | 264.5 / 725.0 / 2448.3 | 1.4x |
| LSMEngine::put, ack=submitted | submitted | 99481 (76291–193171) | 1175.4 (1065.7–1187.3) | **7940** (7713–8356) | 5.9 / 63.1 / 157.7 | 16.0x |
| LSMEngine::put, ack=durable | durable | 2391 (2321–2575) | 0.0 (0.0–0.0) | **2391** (2321–2575) | 364.7 / 1157.8 / 3158.0 | 16.0x |

### 1 thread(s), 128 B values (10000 ops, 1406 KiB logical)

| implementation | ack level | ack throughput (ops/s) | drain (ms) | durable throughput (ops/s) | p50 / p99 / p99.9 put (us) | log bytes / logical |
|---|---|---:|---:|---:|---|---:|
| io_uring+O_DIRECT WAL, ack=submitted | submitted | 86282 (56834–114082) | 1033.2 (1018.9–1127.7) | **8682** (8100–8827) | 6.2 / 83.0 / 172.9 | 3.6x |
| write() WAL, ack=submitted (page cache) | submitted | 441453 (351932–462964) | 6.8 (5.6–8.2) | **333419** (291895–367419) | 1.8 / 6.1 / 39.3 | 1.1x |
| io_uring+O_DIRECT WAL, ack=durable (group commit) | durable | 2384 (2172–2555) | 0.0 (0.0–0.0) | **2384** (2172–2555) | 372.7 / 1228.7 / 3415.0 | 3.6x |
| write()+fdatasync per record, ack=durable | durable | 3485 (3186–4040) | 0.1 (0.1–0.2) | **3485** (3186–4040) | 254.3 / 833.2 / 2224.1 | 1.1x |
| O_DSYNC write(), ack=durable | durable | 3507 (3205–3747) | 0.1 (0.1–0.2) | **3506** (3205–3747) | 257.4 / 946.9 / 2728.7 | 1.1x |
| LSMEngine::put, ack=submitted | submitted | 88604 (78230–108704) | 1007.5 (996.2–1103.6) | **8821** (8233–9095) | 6.6 / 53.7 / 162.8 | 3.6x |
| LSMEngine::put, ack=durable | durable | 2352 (1750–2530) | 0.0 (0.0–0.0) | **2352** (1750–2530) | 416.6 / 1117.5 / 3163.6 | 3.6x |

### 1 thread(s), 1024 B values (10000 ops, 10156 KiB logical)

| implementation | ack level | ack throughput (ops/s) | drain (ms) | durable throughput (ops/s) | p50 / p99 / p99.9 put (us) | log bytes / logical |
|---|---|---:|---:|---:|---|---:|
| io_uring+O_DIRECT WAL, ack=submitted | submitted | 51738 (44844–54332) | 1196.0 (1070.4–1234.5) | **7155** (6968–7971) | 13.7 / 76.3 / 190.4 | 1.5x |
| write() WAL, ack=submitted (page cache) | submitted | 86224 (60838–96035) | 26.3 (23.5–30.6) | **71671** (52739–74234) | 9.5 / 44.1 / 99.9 | 1.0x |
| io_uring+O_DIRECT WAL, ack=durable (group commit) | durable | 2008 (1821–2080) | 0.0 (0.0–0.0) | **2008** (1821–2080) | 444.0 / 1461.7 / 3663.6 | 1.5x |
| write()+fdatasync per record, ack=durable | durable | 2852 (2599–3111) | 0.1 (0.1–0.1) | **2852** (2599–3111) | 296.3 / 1084.2 / 3200.7 | 1.0x |
| O_DSYNC write(), ack=durable | durable | 2882 (2768–3333) | 0.1 (0.1–0.2) | **2882** (2768–3333) | 288.8 / 997.7 / 2911.8 | 1.0x |
| LSMEngine::put, ack=submitted | submitted | 48904 (33549–55503) | 1083.1 (819.9–1219.6) | **7780** (7144–8945) | 14.4 / 91.5 / 176.0 | 1.5x |
| LSMEngine::put, ack=durable | durable | 2092 (2009–2219) | 0.0 (0.0–0.0) | **2092** (2009–2219) | 413.7 / 1441.5 / 4072.0 | 1.5x |

### 1 thread(s), 4096 B values (10000 ops, 40156 KiB logical)

| implementation | ack level | ack throughput (ops/s) | drain (ms) | durable throughput (ops/s) | p50 / p99 / p99.9 put (us) | log bytes / logical |
|---|---|---:|---:|---:|---|---:|
| io_uring+O_DIRECT WAL, ack=submitted | submitted | 18635 (13570–19223) | 1142.1 (974.9–1271.0) | **5909** (4980–6180) | 52.3 / 160.7 / 399.7 | 1.1x |
| write() WAL, ack=submitted (page cache) | submitted | 25350 (24016–26598) | 92.0 (79.9–103.7) | **20659** (19622–21474) | 35.5 / 95.7 / 219.7 | 1.0x |
| io_uring+O_DIRECT WAL, ack=durable (group commit) | durable | 1546 (1459–1901) | 0.0 (0.0–0.0) | **1546** (1459–1901) | 603.5 / 1573.6 / 3946.8 | 1.1x |
| write()+fdatasync per record, ack=durable | durable | 1988 (1815–2222) | 0.1 (0.1–0.2) | **1988** (1815–2222) | 459.7 / 1172.2 / 3043.3 | 1.0x |
| O_DSYNC write(), ack=durable | durable | 2205 (2057–2363) | 0.1 (0.1–0.4) | **2205** (2056–2363) | 421.9 / 1062.8 / 2997.1 | 1.0x |
| LSMEngine::put, ack=submitted | submitted | 16203 (11947–16785) | 1050.3 (839.9–1286.6) | **5964** (5253–6419) | 57.7 / 193.1 / 397.4 | 1.1x |
| LSMEngine::put, ack=durable | durable | 1240 (1218–1257) | 0.0 (0.0–0.0) | **1240** (1218–1257) | 759.7 / 2083.9 / 4629.4 | 1.1x |

### 8 thread(s), 16 B values (10000 ops, 312 KiB logical)

| implementation | ack level | ack throughput (ops/s) | drain (ms) | durable throughput (ops/s) | p50 / p99 / p99.9 put (us) | log bytes / logical |
|---|---|---:|---:|---:|---|---:|
| io_uring+O_DIRECT WAL, ack=submitted | submitted | 113356 (110969–133869) | 867.2 (813.5–928.5) | **10446** (9968–11086) | 4.9 / 1776.8 / 7732.3 | 16.0x |
| write() WAL, ack=submitted (page cache) | submitted | 486787 (337176–542400) | 2.5 (2.0–3.1) | **433352** (309118–488951) | 1.1 / 214.9 / 4012.3 | 1.4x |
| io_uring+O_DIRECT WAL, ack=durable (group commit) | durable | 5936 (5811–6291) | 0.0 (0.0–0.0) | **5936** (5811–6291) | 1299.2 / 3278.5 / 5785.7 | 16.0x |
| write()+fdatasync per record, ack=durable | durable | 3773 (3713–3911) | 0.1 (0.1–0.3) | **3772** (3712–3911) | 244.1 / 2808.2 / 492724.6 | 1.4x |
| O_DSYNC write(), ack=durable | durable | 3682 (3579–3883) | 0.1 (0.1–0.1) | **3682** (3579–3882) | 246.8 / 2410.6 / 503170.1 | 1.4x |
| LSMEngine::put, ack=submitted | submitted | 118126 (102382–142289) | 945.2 (842.4–1000.0) | **9707** (9164–10787) | 5.2 / 1610.5 / 10138.7 | 16.0x |
| LSMEngine::put, ack=durable | durable | 5395 (4986–5763) | 0.0 (0.0–0.0) | **5395** (4986–5763) | 1443.4 / 4008.4 / 9356.3 | 16.0x |

### 8 thread(s), 128 B values (10000 ops, 1406 KiB logical)

| implementation | ack level | ack throughput (ops/s) | drain (ms) | durable throughput (ops/s) | p50 / p99 / p99.9 put (us) | log bytes / logical |
|---|---|---:|---:|---:|---|---:|
| io_uring+O_DIRECT WAL, ack=submitted | submitted | 128131 (114170–139104) | 861.8 (829.9–945.2) | **10640** (9757–10996) | 4.9 / 1395.9 / 10463.9 | 3.6x |
| write() WAL, ack=submitted (page cache) | submitted | 323543 (292650–334875) | 5.6 (5.3–10.7) | **256557** (244116–281916) | 2.0 / 248.6 / 3985.7 | 1.1x |
| io_uring+O_DIRECT WAL, ack=durable (group commit) | durable | 5741 (5509–5997) | 0.0 (0.0–0.0) | **5741** (5509–5997) | 1359.6 / 3335.0 / 5996.5 | 3.6x |
| write()+fdatasync per record, ack=durable | durable | 3519 (3260–3589) | 0.1 (0.1–0.4) | **3518** (3260–3589) | 258.9 / 3272.5 / 507796.6 | 1.1x |
| O_DSYNC write(), ack=durable | durable | 3353 (3203–3454) | 0.1 (0.1–0.1) | **3353** (3203–3454) | 273.7 / 2288.7 / 565497.7 | 1.1x |
| LSMEngine::put, ack=submitted | submitted | 121927 (92114–139958) | 903.7 (868.6–970.6) | **10122** (9523–10261) | 5.9 / 1863.1 / 10166.4 | 3.6x |
| LSMEngine::put, ack=durable | durable | 5462 (4547–6251) | 0.0 (0.0–0.0) | **5462** (4547–6251) | 1431.8 / 3282.3 / 9644.0 | 3.6x |

### 8 thread(s), 1024 B values (10000 ops, 10156 KiB logical)

| implementation | ack level | ack throughput (ops/s) | drain (ms) | durable throughput (ops/s) | p50 / p99 / p99.9 put (us) | log bytes / logical |
|---|---|---:|---:|---:|---|---:|
| io_uring+O_DIRECT WAL, ack=submitted | submitted | 51575 (49069–54724) | 1306.7 (1231.8–1323.8) | **6620** (6589–7070) | 13.3 / 4664.3 / 15167.3 | 1.5x |
| write() WAL, ack=submitted (page cache) | submitted | 90911 (80879–101924) | 29.8 (28.1–34.2) | **70268** (65921–78196) | 10.7 / 3089.1 / 6347.2 | 1.0x |
| io_uring+O_DIRECT WAL, ack=durable (group commit) | durable | 5015 (4124–5112) | 0.0 (0.0–0.0) | **5015** (4124–5112) | 1599.9 / 3916.8 / 7135.6 | 1.5x |
| write()+fdatasync per record, ack=durable | durable | 3322 (2946–3452) | 0.1 (0.1–0.2) | **3322** (2946–3452) | 275.1 / 70166.5 / 260052.5 | 1.0x |
| O_DSYNC write(), ack=durable | durable | 2957 (2561–3052) | 0.1 (0.1–0.1) | **2957** (2561–3052) | 299.7 / 77111.0 / 286515.1 | 1.0x |
| LSMEngine::put, ack=submitted | submitted | 47133 (44183–49574) | 1276.0 (1208.0–1317.3) | **6731** (6478–7081) | 14.9 / 5029.8 / 17978.7 | 1.5x |
| LSMEngine::put, ack=durable | durable | 4891 (4174–5156) | 0.0 (0.0–0.0) | **4891** (4174–5156) | 1617.5 / 3984.5 / 8277.1 | 1.5x |

### 8 thread(s), 4096 B values (10000 ops, 40156 KiB logical)

| implementation | ack level | ack throughput (ops/s) | drain (ms) | durable throughput (ops/s) | p50 / p99 / p99.9 put (us) | log bytes / logical |
|---|---|---:|---:|---:|---|---:|
| io_uring+O_DIRECT WAL, ack=submitted | submitted | 18438 (17238–18503) | 1383.0 (1233.9–1537.5) | **5199** (4722–5594) | 50.3 / 10778.8 / 22423.8 | 1.1x |
| write() WAL, ack=submitted (page cache) | submitted | 41865 (39073–45255) | 103.6 (102.2–108.8) | **29293** (27419–30946) | 41.4 / 2508.3 / 7933.7 | 1.0x |
| io_uring+O_DIRECT WAL, ack=durable (group commit) | durable | 4164 (3838–4494) | 0.0 (0.0–0.0) | **4164** (3838–4493) | 1871.9 / 4684.2 / 7690.5 | 1.1x |
| write()+fdatasync per record, ack=durable | durable | 1763 (1430–1844) | 0.1 (0.1–0.2) | **1763** (1430–1844) | 2832.5 / 42947.8 / 94300.5 | 1.0x |
| O_DSYNC write(), ack=durable | durable | 1813 (1735–1985) | 0.1 (0.1–0.1) | **1813** (1735–1985) | 3447.1 / 33343.6 / 66346.3 | 1.0x |
| LSMEngine::put, ack=submitted | submitted | 16449 (15769–17214) | 1458.6 (1425.4–1510.5) | **4870** (4663–4904) | 53.6 / 12298.5 / 28632.8 | 1.1x |
| LSMEngine::put, ack=durable | durable | 3892 (3623–4212) | 0.0 (0.0–0.0) | **3892** (3623–4212) | 2009.3 / 4717.0 / 8989.8 | 1.1x |

Median over 5 runs, (min–max) in parentheses.

---

## 2. Read benchmark (not re-measured)

> These figures predate this change and were recorded on a different machine
> (privileged Docker container, NVMe). They have **not** been reproduced here and
> should be treated as unverified until `bench_read` is re-run with hardware,
> repetitions and dataset size recorded as above.

### Point read latency with Bloom filter

We measured the point lookup performance of 5,000 queries in two scenarios:
1. **Bloom filter HIT**: Keys are queried that actually exist in the database (forcing binary search and disk block reads).
2. **Bloom filter MISS**: Keys are queried that do not exist (allowing the cache-aligned Block Bloom filter to return `false` in memory).

### Results
- **Bloom filter HIT (Key present on disk)**:
  - **Throughput**: 189,263 ops/sec
  - **Avg Latency**: 5.23 μs
  - **P99 Latency**: 13.50 μs
- **Bloom filter MISS (Bypasses disk search)**:
  - **Throughput**: 1,315,789 ops/sec
  - **Avg Latency**: 0.76 μs
  - **P99 Latency**: 2.20 μs
- **Interception Rate**: ~97% of non-existent queries are caught in memory without touching disk.

**Key Takeaway**: Without a Bloom filter, 100% of non-existent queries would hit the disk block search path, resulting in a latency of ~75.0 μs. With the 64-byte Block Bloom filter, misses are resolved in memory instead, dropping average miss latency to 0.76 μs — an order of magnitude below the 5.23 μs a hit costs, since a miss never touches the disk at all.
