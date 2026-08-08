#pragma once
#include "../memtable/skip_list.h"
#include "../wal/wal.h"
#include "../sstable/sstable.h"
#include "../compaction/leveled_compactor.h"
#include <string>
#include <vector>
#include <memory>
#include <optional>
#include <shared_mutex>

class LSMEngine {
public:
    explicit LSMEngine(const std::string& data_dir, size_t memtable_max_bytes = Memtable::DEFAULT_MAX_BYTES);
    ~LSMEngine();

    // Returns by value, not by reference.
    //
    // This returned `const std::string&` bound to its own parameter, so
    // `ensureDir(makePath())` produced a reference to a temporary that died at
    // the end of the full expression. The single call site in the constructor
    // passes a named parameter and is safe, but the function is public and
    // static — nothing stopped a caller elsewhere from passing a temporary, and
    // neither -Wall nor -Wextra warns about it.
    static std::string ensureDir(const std::string& dir);

    void put(const std::string& key, const std::string& value);
    void del(const std::string& key);
    std::optional<std::string> get(const std::string& key) const;
    void flush();

    size_t memtableSize()  const {
        std::shared_lock<std::shared_mutex> lock(mu_);
        return memtable_->size();
    }
    size_t sstableCount()  const {
        std::shared_lock<std::shared_mutex> lock(mu_);
        return sstables_.size();
    }

private:
    // Guards every member below it.
    //
    // put(), del() and flush() replace `memtable_`, push onto `sstables_` and
    // hand that vector to the compactor, which clears and refills it. get()
    // walks the same vector and dereferences the memtable. None of that was
    // synchronised, so a reader indexing `sstables_` while a writer reallocated
    // it read through a dangling pointer, and two writers flushing together
    // corrupted the vector outright.
    //
    // Shared rather than exclusive because reads are the common case and are
    // genuinely parallel-safe: Memtable::get() is const, and SSTable::get()
    // opens its own ifstream per call rather than sharing one.
    mutable std::shared_mutex mu_;

    std::string data_dir_;
    size_t memtable_max_bytes_;

    // Declared — and so acquired — before wal_.
    //
    // Members initialise in declaration order, and wal_ opens the log, creating
    // it if absent. With the lock taken afterwards in the constructor body, two
    // processes could both reach and modify wal.log before either attempted the
    // flock, so the one about to be refused had already touched the log.
    int lock_fd_ = -1;

    WAL         wal_;
    std::unique_ptr<Memtable> memtable_;
    std::vector<std::unique_ptr<SSTable>> sstables_;
    int sst_counter_ = 0;
    LeveledCompactor compactor_;

    // Requires the caller to already hold mu_ exclusively. Called from put(),
    // del() and flush(), all of which take that lock — taking it again here
    // would deadlock, since std::shared_mutex is not recursive.
    static int acquireDirectoryLock(const std::string& dir);

    void flushMemtable();
    void recoverFromWAL();
    void loadSSTables();
    std::string sstPath(int level, int counter) const;
};
