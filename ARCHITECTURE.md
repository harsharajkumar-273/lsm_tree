# LSM-Tree Architecture Deep-Dive

This document details the low-level systems design and architecture of this high-performance Log-Structured Merge-tree (LSM-tree) storage engine.

---

## 1. The Write Path

### Write-Ahead Log (WAL) with `io_uring`
To guarantee durability before returning a successful write to the client, every insertion (`PUT` or `DELETE`) is appended to a Write-Ahead Log. Standard synchronous file writes block the calling thread, stalling writes. This engine utilizes Linux's next-generation asynchronous I/O interface, **`io_uring`**, combined with **`O_DIRECT`**:
- **Zero-Copy & Direct I/O**: File descriptors are opened with the `O_DIRECT` flag, bypassing the OS page cache entirely. Writes copy bytes directly from user-space memory buffers to physical disk blocks.
- **Buffer Alignment**: `O_DIRECT` requires that buffers, memory offsets, and file offsets be aligned to the block size (512 bytes). Writes are rounded up to 512-byte multiples, allocating page-aligned buffers via `posix_memalign`.
- **io_uring Submission**: A write request is prepared using `io_uring_prep_write` and submitted to the Submission Queue (SQ).
- **Concurrency & Serialization**: While queue completions are reaped asynchronously, concurrent threads submit requests sequentially via a mutex lock to guarantee that log entries are appended in chronological order.

### Concurrent MemTable (SkipList)
Once logged, the key-value pair is inserted into the memory table (MemTable), which is built on a concurrent lock-free **SkipList**:
- **Lock-Free CAS Updates**: Insertion links node pointers dynamically using atomic Compare-And-Swap (`compare_exchange_weak`/`compare_exchange_strong`) operations. This allows multiple threads to insert entries concurrently without locking the index structure.
- **In-Place Overwrite Semantics**: If a key already exists, threads perform a direct in-place update of the value (`succs[0]->value = value`) in the existing node, avoiding duplicate nodes and preventing stale read results.
- **Thread-Safe Arena Bump Allocator**: To avoid malloc memory overhead, nodes are allocated from a custom bump-pointer `Arena`. Memory is allocated in chunks (1MB) and protected by a mutex lock, which serializes allocation while keeping SkipList traversals lock-free.
- **Thread-Local Height Generation**: Height generation uses a `thread_local` random number generator (`std::mt19937`), eliminating lock contention on shared seed variables.

---

## 2. The Read Path

Lookups query components from newest to oldest:
1. **MemTable**: Checked first using SkipList acquisition.
2. **Level 0 SSTables**: If not found in memory, SSTables flushed from MemTable are queried in reverse chronological order (newest to oldest). Since L0 ranges overlap, all L0 tables must be checked.
3. **Level 1 SSTables**: Partitioned, non-overlapping tables. A binary search on the tables' bounds identifies the single table that could contain the key.

### Sparse Indexing and Binary Search
Each SSTable file contains a data section of key-value records, followed by a sparse index.
- Every 64th key (configurable via `ENTRIES_PER_BLOCK`) is recorded in the sparse index along with its byte offset in the file.
- Lookups perform a binary search on the in-memory sparse index to locate the starting offset of the target 64-entry block, narrowing down disk scans.

### Cache-Aligned Block Bloom Filter
To prevent costly disk reads for keys not in the database, each SSTable maintains an in-memory Block Bloom Filter:
- **Cache-Line Alignment**: The filter is split into 64-byte blocks (matching CPU cache lines). Checking a key checks bits within a single 64-byte block.
- **Zero Cache Misses**: Traditional Bloom filters scatter bit lookups across random memory addresses, causing up to 8 cache misses per query. Our Block Bloom filter guarantees that checking a key incurs exactly **one** cache-miss penalty.
- **Double Hashing**: Computes FNV-1a and Murmur mix hashes in a single pass to map keys to blocks and indices efficiently.

---

## 3. Compaction (L0 → L1 Leveled Compaction)

When Level 0 accumulates 4 or more SSTables, a compaction sweep is triggered to transition them into Level 1:
1. **Deduplication and Purge**: All active Level 0 and Level 1 files are opened. The compactor reads their entries, keeping only the newest update for each key and discarding tombstones.
2. **File Partitioning**: Merged entries are segmented into non-overlapping blocks of 1,000 records.
3. **Atomic Staged Commit**: Merged tables are written to temporary `.tmp` files and `fsync`ed to disk. Each `.tmp` file is then atomically renamed to its final Level 1 path (`sst-1-xxxxxx.sst`), ensuring crash-consistency before old input SSTables are unlinked.
