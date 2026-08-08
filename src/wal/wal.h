#pragma once
#include <string>
#include <vector>
#include <mutex>
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

    struct Entry {
        enum class Type { PUT = 0x01, DEL = 0x02 };
        Type        type;
        std::string key;
        std::string value;
    };

    explicit WAL(const std::string& path);
    ~WAL();

    void logPut(const std::string& key, const std::string& value);
    void logDel(const std::string& key);
    std::vector<Entry> recover() const;
    void clear();

private:
    int fd_;
    std::string path_;
    struct io_uring ring_;
    // Tracks whether ring_ currently holds an initialised io_uring. Without it,
    // a clear() that fails partway leaves the destructor exiting a ring that
    // has already been exited.
    bool ring_ready_ = false;
    mutable std::mutex mu_;

    // Tagged on each SQE so the completion can be checked against what was
    // asked for. The buffer pointer alone was not enough to recognise a short
    // write, since the expected length was not recorded anywhere.
    struct PendingWrite {
        void*  buf;
        size_t bytes;
    };

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

    void writeEntry(Entry::Type type, const std::string& key, const std::string& value);
    void reap();
    void drain();
    static uint32_t crc32(uint8_t type, const std::string& key, const std::string& value);
};
