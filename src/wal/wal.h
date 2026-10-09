#pragma once
#include <string>
#include <vector>
#include <mutex>
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <liburing.h>

// WAL (Write-Ahead Log) using Linux io_uring for Asynchronous Zero-Copy Logging.
class WAL {
public:
    // Largest log recover() will read into memory.
    //
    // Recovery buffers the whole file, so this bounds the allocation. 1 GiB is
    // far above any log a healthy engine produces — flushMemtable() clears the
    // WAL after every flush — while staying small enough to allocate on a
    // modest machine. Exceeding it throws rather than risking the OOM killer.
    static constexpr uint64_t MAX_RECOVERY_BYTES = 1ULL << 30;   // 1 GiB

    // A request submitted from a thread that exits while it is in flight can be
    // completed with -ECANCELED by the kernel, although nothing is wrong with the
    // write. Retried this many times in total before it is reported as a permanent
    // failure, so a persistently cancelling environment cannot loop forever.
    static constexpr int kMaxWriteAttempts = 6;

    struct Entry {
        enum class Type { PUT = 0x01, DEL = 0x02 };
        Type        type;
        std::string key;
        std::string value;
    };

    // What "the write call returned" promises. See DURABILITY.md.
    //
    //   Async   - returns once the record is *submitted* to io_uring. It may
    //             still be in flight; it survives a process crash only if the
    //             kernel completes it, and survives power loss only after a
    //             later sync(). Errors surface on a later call or on sync().
    //   Durable - returns once the record's write has completed AND fdatasync
    //             has made it stable. Concurrent callers share one fdatasync
    //             (group commit).
    enum class Durability { Async, Durable };

    // Names a position in the log. Valid until the next clear(); a ticket from
    // an earlier epoch is treated as already satisfied, because clear() is only
    // called after the entries it discards are durable in an SSTable.
    struct Ticket {
        uint64_t epoch = 0;
        uint64_t end   = 0;   // log offset one past the record
    };

    explicit WAL(const std::string& path, Durability durability = Durability::Async);
    ~WAL();

    // append*: submit the record and return. Never waits for durability, so a
    // caller holding its own lock can release it before calling waitDurable().
    Ticket appendPut(const std::string& key, const std::string& value);
    Ticket appendDel(const std::string& key);

    // Blocks until everything up to `t` is stable on disk. Throws if the log
    // has recorded a write or sync failure.
    void waitDurable(Ticket t);

    // Makes every record appended so far durable (a barrier, valid in either mode).
    void sync();

    // append + (Durable mode only) waitDurable.
    void logPut(const std::string& key, const std::string& value);
    void logDel(const std::string& key);

    Durability durability() const { return durability_; }

    // Number of fdatasync calls issued. With group commit this is smaller than
    // the number of durable appends whenever writers overlap.
    uint64_t syncCount() const { return sync_calls_.load(); }

    // Writes resubmitted after an -ECANCELED completion (observability only).
    uint64_t cancelRetries() const { return cancel_retries_.load(); }
    std::vector<Entry> recover() const;
    void clear();

private:
    int fd_;
    std::string path_;
    Durability durability_;

    // Group-commit state. Lock order: sync_mu_ before mu_, never the reverse.
    // sync_mu_ is NOT held across fdatasync, so appends continue meanwhile and
    // are picked up by the next leader.
    std::mutex sync_mu_;
    std::condition_variable sync_cv_;
    bool sync_in_progress_ = false;
    std::atomic<uint64_t> epoch_{0};   // bumped by clear() while holding sync_mu_ and mu_
    std::atomic<uint64_t> sync_calls_{0};
    std::atomic<uint64_t> cancel_retries_{0};
    uint64_t durable_end_ = 0;         // highest offset known stable in this epoch; guarded by sync_mu_
    struct io_uring ring_;
    // Tracks whether ring_ currently holds an initialised io_uring. Without it,
    // a clear() that fails partway leaves the destructor exiting a ring that
    // has already been exited.
    bool ring_ready_ = false;
    mutable std::mutex mu_;

    // Tagged on each SQE so the completion can be checked against what was
    // asked for. The buffer pointer alone was not enough to recognise a short
    // write, since the expected length was not recorded anywhere.
    //
    // offset and attempts exist so a request that completes with -ECANCELED can
    // be resubmitted exactly as first issued: same buffer, same length, same file
    // position. The buffer stays owned by this record until a completion that
    // is final (success or permanent failure) is observed.
    struct PendingWrite {
        void*    buf;
        size_t   bytes;
        uint64_t offset;
        int      attempts;   // submissions so far, including the first
    };

    // Resubmits `p` after a cancellation. Returns 1 if it is in flight again, 0 if
    // it could not be queued (caller still owns the buffer), -1 if the submit call
    // itself failed (failure recorded; the buffer must be retained). Requires mu_.
    int resubmit(PendingWrite* p);

    // First completion failure seen, retained rather than thrown. reap() runs
    // from the destructor, where throwing would terminate the process, so it
    // records and the next writeEntry() reports.
    std::string failure_;

    // Outstanding submitted CQEs in-flight that must be reaped before exiting ring.
    size_t pending_cqes_ = 0;

    // Byte position the next record is written to.
    //
    // Writes previously used offset -1, letting the kernel resolve the position
    // to end-of-file under O_APPEND. That is atomic per write — appends cannot
    // interleave — but it ties record order to *completion* order, and
    // writeEntry() releases mu_ after submitting rather than after completing.
    // Several appends are therefore in flight at once, and io_uring guarantees
    // no ordering between them. Measured on this codebase: 1,046 of 30,000
    // records landed out of submission order, the first at index 11.
    //
    // For a write-ahead log that is a correctness problem, not a cosmetic one:
    // replaying put(k, v1) after put(k, v2) leaves the wrong value, so recovery
    // can produce a state the engine was never in.
    //
    // Assigning the offset here, under mu_, fixes each record's position at
    // submission time. Completion order stops mattering.
    uint64_t write_offset_ = 0;

    void recordFailure(const std::string& detail);

    Ticket writeEntry(Entry::Type type, const std::string& key, const std::string& value);
    void reap();
    void drain();
    static uint32_t crc32(uint8_t type, const std::string& key, const std::string& value);
};
