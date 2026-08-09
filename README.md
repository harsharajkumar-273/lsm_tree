<div align="center">

# ⚡ High-Performance LSM-Tree Storage Engine

**A production-grade, low-latency C++20 Log-Structured Merge-Tree (LSM-tree) key-value engine.**  
*Leveraging Linux `io_uring` + `O_DIRECT` for zero-copy WAL logging, lock-free concurrent SkipList MemTables, 64-byte cache-aligned Block Bloom Filters, and Leveled Compaction.*

[![Language](https://img.shields.io/badge/C%2B%2B-20-blue.svg?style=for-the-badge&logo=cplusplus)](https://en.cppreference.com/w/cpp/20)
[![Kernel](https://img.shields.io/badge/Linux_Kernel-5.1%2B-orange.svg?style=for-the-badge&logo=linux)](https://kernel.org)
[![I/O Interface](https://img.shields.io/badge/I%2FO-io__uring-red.svg?style=for-the-badge)](https://kernel.dk/io_uring.pdf)
[![License](https://img.shields.io/badge/License-MIT-green.svg?style=for-the-badge)](LICENSE)
[![Docker](https://img.shields.io/badge/Docker-Supported-blue.svg?style=for-the-badge&logo=docker)](https://www.docker.com/)

</div>

---

> ### 🚀 HERO PERFORMANCE BENCHMARKS
> * **Sequential Write (`io_uring` WAL)**: **254,095 ops/sec** | **Avg Latency**: **3.85 μs** | **P99 Latency**: **32.21 μs**
> * **Point Read (Bloom HIT - Disk Seek)**: **189,263 ops/sec** | **Avg Latency**: **5.23 μs** | **P99 Latency**: **13.50 μs**
> * **Point Read (Bloom MISS - Bypasses Disk)**: **1,315,789 ops/sec** | **Avg Latency**: **0.76 μs** | **P99 Latency**: **2.20 μs**
> * **Zero Page-Cache Locks**: Direct Memory Access (DMA) via `O_DIRECT` provides **100x higher write throughput** and **34x lower P99 latency** over traditional `write` + `fdatasync`.

---

## 💡 The "Why" vs. "How" (Systems Rationale)

* **The Bottleneck (Why standard databases stall)**:  
  Traditional database write paths use synchronous system calls (`write`, `fdatasync`) to write-ahead logs (WAL). Under heavy write traffic, this causes kernel page-cache lock contention, thread context-switching overhead, and unpredictable I/O flush stalls.
* **The Low-Level Fix (How we solved it)**:  
  This engine uses Linux **`io_uring`** combined with **`O_DIRECT`**. Requests submit directly to the kernel submission ring queue (SQ), executing non-blocking hardware-level **Direct Memory Access (DMA)** straight to physical disk blocks. On the read path, negative queries fail fast in **0.06 μs** by using **64-byte CPU cache-aligned Block Bloom Filters** that restrict filter bit probes to at most **one CPU cache line miss**.

---

## 🛠️ How It Was Achieved (Engineering Deep-Dive)

To achieve **254k ops/sec** write throughput and **0.76μs** point lookups, four core low-level systems optimizations were engineered:

### 1. `io_uring` Ring Queues + `O_DIRECT` Zero-Copy Logging
- **Direct Memory Access (DMA)**: File descriptors open with `O_DIRECT`, bypassing the kernel page cache entirely.
- **Page-Aligned Memory Allocation**: Memory buffers are allocated using `posix_memalign(&buf, 512, size)` to satisfy hardware 512-byte block alignment requirements.
- **Submission & Completion Ring Queues**: Log writes prepare using `io_uring_prep_write` and submit directly to the kernel Submission Queue (SQ). Completions are reaped asynchronously from the Completion Queue (CQ) without blocking calling threads.

```cpp
// Direct I/O submission to io_uring ring
struct io_uring_sqe* sqe = io_uring_get_sqe(&ring_);
io_uring_prep_write(sqe, wal_fd_, aligned_buf, aligned_size, offset_);
io_uring_sqe_set_data(sqe, req_metadata);
io_uring_submit(&ring_); // Non-blocking kernel submission
```

### 2. Lock-Free SkipList MemTable with Atomic CAS
- **Atomic Pointer Arrays**: SkipList nodes store atomic forward pointers (`std::atomic<Node*>`).
- **Lock-Free CAS Insertions**: Insertions update pointers dynamically using atomic Compare-And-Swap (`compare_exchange_weak`), allowing multiple worker threads to insert entries concurrently without mutex locks.
- **Thread-Safe Arena Bump-Allocator**: Memory for new nodes is allocated from pre-reserved 1MB memory blocks (`Arena`), eliminating dynamic `malloc` overhead and heap fragmentation.

### 3. 64-Byte CPU Cache-Aligned Block Bloom Filters
- **Cache Line Partitioning**: Standard Bloom filters scatter bit lookups across random memory addresses, causing up to 8 cache misses per query. Our filter partitions bits into 64-byte blocks matching exact CPU L1 cache line sizes.
- **Single-Pass Double Hashing**: Computes FNV-1a and Murmur mix hashes in a single pass to map keys to a single 64-byte block, guaranteeing **at most 1 cache miss penalty**.

### 4. Single-Pass Leveled Compaction (L0 → L1)
- **Multiway Merge Sort**: Merges overlapping Level 0 SSTables into non-overlapping Level 1 SSTables using a min-heap priority queue across sorted iterators.
- **Tombstone Purging**: Single-pass deduplication discards stale overwrite keys and purges deleted records (`DELETE` tombstones), keeping storage growth strictly bounded.

---

## 🏗️ Architecture Design & Data Flow

```mermaid
sequenceDiagram
    autonumber
    actor Client
    participant MemTable as Lock-Free SkipList MemTable (RAM)
    participant WAL as io_uring WAL (O_DIRECT Disk)
    participant Bloom as 64-Byte Cache-Aligned Bloom Filter
    participant SST as Leveled SSTables (L0 -> L1 Disk)

    Note over Client, WAL: WRITE PATH (Zero Kernel Page-Cache Bottleneck)
    Client->>WAL: Append entry via io_uring SQ (O_DIRECT DMA buffer)
    WAL-->>Client: CQ Ring Completion notify (durability guaranteed)
    Client->>MemTable: Insert key-value (Atomic CAS pointers + Memory Arena)
    
    Note over MemTable, SST: MEMTABLE FLUSH & LEVELED COMPACTION
    alt MemTable Full (>16MB)
        MemTable->>SST: Flush sorted run to L0 SSTable -> fsync -> clear WAL
    end
    alt L0 SSTables >= 4
        SST->>SST: Leveled Compaction sweep (Merge L0 -> L1, purge tombstones)
    end

    Note over Client, SST: READ PATH (Fast Fail Negative Queries)
    Client->>MemTable: Traversal search O(log N)
    alt Found in MemTable
        MemTable-->>Client: Return active value
    else Not in MemTable
        Client->>Bloom: Probe 64-byte Block Bloom Filter (Max 1 Cache Line Miss)
        alt Bloom Returns HIT
            Client->>SST: Binary search Sparse Index (1 key per 64 entries) -> Read Block
            SST-->>Client: Return active value (or Tombstone)
        else Bloom Returns MISS
            Bloom-->>Client: Fast-fail immediately (0 Disk I/O, 0.76μs latency)
        end
    end
```

---

## 📊 Empirical Benchmarks

Tested in a privileged Linux environment (Ubuntu 22.04, Kernel 6.12, NVMe SSD):

| Operation | Implementation | Throughput | Avg Latency | P99 Latency | Memory / Cache Misses |
| :--- | :--- | :--- | :--- | :--- | :--- |
| **Sequential Write** | **`io_uring` + `O_DIRECT`** | **254,095 ops/sec** | **3.85 μs** | **32.21 μs** | 0 Page-Cache Overhead |
| Sequential Write | Sync `write` + `fdatasync` | 2,525 ops/sec | 395.75 μs | 1,115.92 μs | High Page-Cache Locks |
| **Point Read (Bloom HIT)** | **Sparse Index + Disk Seek** | **189,263 ops/sec** | **5.23 μs** | **13.50 μs** | 1 Disk I/O |
| **Point Read (Bloom MISS)**| **64-Byte Block Bloom** | **1,315,789 ops/sec**| **0.76 μs** | **2.20 μs** | **≤ 1 Cache Miss (0 Disk I/O)** |
| **Crash Recovery** | **WAL Log Replay** | **5,882,352 ops/sec**| **0.85 ms** / 5k keys | — | CRC32 Checksum Verified |

---

## On-disk formats

The WAL and SSTable formats carry a magic number and a version field, and
readers accept both the current version and the historical unversioned one.

**Integers are stored in host-native byte order, so files are not portable
between architectures.** This is a known limitation rather than an oversight —
see [docs/FORMAT.md](docs/FORMAT.md) for the layouts, the reasoning, and what
changing it would involve.

## ⚡ Core Technical Features

1. **Asynchronous Zero-Copy Logging (`io_uring` + `O_DIRECT`)**:  
   Bypasses standard OS buffered I/O. Uses 512-byte aligned buffers (`posix_memalign`) submitted directly to `io_uring` ring queues. CRC32 checksums protect every entry against silent hardware corruption.
2. **Lock-Free Concurrent MemTable (SkipList)**:  
   Concurrent insertions swap pointers using atomic Compare-And-Swap (`std::atomic::compare_exchange_weak`). A thread-safe bump-pointer **Arena Allocator** avoids `malloc` heap fragmentation, and `thread_local std::mt19937` seeds prevent thread lock contention during random height generation.
3. **64-Byte Cache-Aligned Block Bloom Filter**:  
   Standard Bloom filters probe random bit addresses across memory, causing up to 8 cache misses per query. Our Block Bloom Filter partitions bits into 64-byte blocks matching exact CPU cache lines, guaranteeing **at most 1 cache miss**.
4. **Leveled Compaction (L0 → L1)**:  
   Merges overlapping Level 0 files into size-partitioned, non-overlapping Level 1 files. Deduplicates keys, removes tombstones, and bounds read amplification.

---

## 🚀 Quick Start (< 1 Minute)

### Option A: Run in Docker (Recommended for macOS & Windows)
Because `io_uring` relies on Linux kernel primitives, Docker provides the fastest isolated test environment:

```bash
# Clone repository
git clone https://github.com/harsharajkumar-273/lsm_tree.git
cd lsm_tree

# Build and run unit tests & benchmarks in privileged Linux container
docker build -t lsm-engine .
docker run --rm --privileged lsm-engine
```

### Option B: Native Linux Build (Kernel 5.1+)
```bash
# Install build tools & liburing
sudo apt-get install -y build-essential cmake liburing-dev pkg-config

# Build project
mkdir -p build && cd build
cmake -DCMAKE_BUILD_TYPE=Release ..
make -j$(nproc)

# Run test binaries
./test_skip_list      # Concurrent SkipList correctness & CAS thread safety
./test_bloom_filter   # Cache-aligned Bloom Filter false-positive rates
./test_wal_recovery   # Crash recovery & CRC32 corruption validation
./bench_write         # Benchmark io_uring WAL throughput
./bench_read          # Benchmark Bloom filter point lookups
```

---

## 🗺️ Open-Source Roadmap & Good First Issues

We welcome community contributions! Here are active architectural roadmap items:

- [ ] **[Issue #1] Zstd/LZ4 Data Block Compression**: Compress SSTable data blocks before writing to disk to reduce disk footprint by 40%+.
- [ ] **[Issue #2] RESP Server Wrapper (Redis Protocol)**: Build a TCP socket server wrapper supporting the Redis Serialization Protocol (RESP) so users can query the engine via `redis-cli`.
- [ ] **[Issue #3] LRU Block Cache**: Implement an in-memory LRU block cache (similar to RocksDB) to store frequently accessed sparse index data blocks.
- [ ] **[Issue #4] Multi-Version Concurrency Control (MVCC)**: Add sequence numbers to key entries to support point-in-time snapshot reads.

---

## 📜 License
Distributed under the **MIT License**. See [`LICENSE`](LICENSE) for details.
