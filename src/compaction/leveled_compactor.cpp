#include "leveled_compactor.h"
#include <map>
#include <filesystem>
#include <sstream>
#include <iomanip>
#include <iostream>
#include <fcntl.h>
#include <unistd.h>
#include <algorithm>

#include <queue>

namespace {

// Flushes a file's contents to stable storage.
void fsyncFile(const std::string& path) {
    int fd = ::open(path.c_str(), O_RDONLY);
    if (fd < 0) return;
    ::fsync(fd);
    ::close(fd);
}

// Flushes a directory entry, which is what makes a rename durable.
void fsyncDir(const std::string& dir) {
    int fd = ::open(dir.c_str(), O_RDONLY | O_DIRECTORY);
    if (fd < 0) return;
    ::fsync(fd);
    ::close(fd);
}

struct HeapNode {
    SSTable::Entry entry;
    size_t sst_index; // Index in sstables vector (higher = newer table)

    bool operator>(const HeapNode& other) const {
        if (entry.key != other.entry.key) {
            return entry.key > other.entry.key;
        }
        return sst_index < other.sst_index; // Higher sst_index wins
    }
};

} // namespace

std::string LeveledCompactor::makeSSTPath(int level, int counter) const {
    std::ostringstream oss;
    oss << data_dir_ << "/sst-" << level << "-" << std::setw(6) << std::setfill('0') << counter << ".sst";
    return oss.str();
}

void LeveledCompactor::run(std::vector<std::unique_ptr<SSTable>>& sstables, int& sst_counter) {
    // Count Level 0 tables
    std::vector<std::string> l0_paths;
    std::vector<std::string> l1_paths;
    
    for (const auto& sst : sstables) {
        std::string filename = std::filesystem::path(sst->path()).filename().string();
        if (filename.rfind("sst-0-", 0) == 0) {
            l0_paths.push_back(sst->path());
        } else if (filename.rfind("sst-1-", 0) == 0) {
            l1_paths.push_back(sst->path());
        }
    }

    if (l0_paths.size() < L0_THRESHOLD) {
        return; // Compaction threshold not met
    }

    std::cout << "[Compaction] Merging " << l0_paths.size() << " L0 tables with "
              << l1_paths.size() << " L1 tables...\n";

    // Sweep temporaries abandoned by an earlier interrupted compaction.
    {
        std::error_code ec;
        for (const auto& entry : std::filesystem::directory_iterator(data_dir_, ec)) {
            if (entry.path().extension() == ".tmp") {
                // Best effort by design: a temporary that cannot be removed is
                // retried on the next run and is never loaded in the meantime,
                // since the engine only picks up files matching sst-N-*.sst.
                std::error_code rm_ec;
                std::filesystem::remove(entry.path(), rm_ec);
            }
        }
    }

    int l1_start = 0;
    for (const auto& p : l1_paths) {
        const std::string stem = std::filesystem::path(p).stem().string(); // sst-1-000007
        const size_t dash = stem.rfind('-');
        if (dash != std::string::npos) {
            try {
                l1_start = std::max(l1_start, std::stoi(stem.substr(dash + 1)) + 1);
            } catch (const std::exception&) {
                // Unparseable name: ignore it
            }
        }
    }

    // Phase 1 - Streaming K-Way merge using Min-Heap
    std::vector<std::pair<std::string, std::string>> staged; // temporary -> final
    int l1_counter = l1_start;

    try {
        std::priority_queue<HeapNode, std::vector<HeapNode>, std::greater<HeapNode>> min_heap;
        std::vector<std::unique_ptr<SSTableIterator>> iterators;
        iterators.reserve(sstables.size());

        for (size_t i = 0; i < sstables.size(); ++i) {
            iterators.push_back(sstables[i]->createIterator());
            if (iterators[i]->hasNext()) {
                min_heap.push(HeapNode{iterators[i]->next(), i});
            }
        }

        std::vector<SSTable::Entry> current_l1_buffer;
        current_l1_buffer.reserve(L1_MAX_FILE_ENTRIES);

        auto flushBuffer = [&]() {
            if (current_l1_buffer.empty()) return;
            std::string final_path = makeSSTPath(1, l1_counter++);
            std::string tmp_path = final_path + ".tmp";
            SSTable::write(tmp_path, current_l1_buffer);
            fsyncFile(tmp_path);
            staged.emplace_back(tmp_path, final_path);
            current_l1_buffer.clear();
        };

        while (!min_heap.empty()) {
            std::string current_key = min_heap.top().entry.key;
            HeapNode winner = min_heap.top();
            min_heap.pop();

            if (iterators[winner.sst_index]->hasNext()) {
                min_heap.push(HeapNode{iterators[winner.sst_index]->next(), winner.sst_index});
            }

            // Consume older duplicates of current_key
            while (!min_heap.empty() && min_heap.top().entry.key == current_key) {
                size_t duplicate_idx = min_heap.top().sst_index;
                min_heap.pop();
                if (iterators[duplicate_idx]->hasNext()) {
                    min_heap.push(HeapNode{iterators[duplicate_idx]->next(), duplicate_idx});
                }
            }

            // Stream non-tombstone entry to L1 buffer
            if (winner.entry.value.has_value()) {
                current_l1_buffer.push_back(winner.entry);
                if (current_l1_buffer.size() >= L1_MAX_FILE_ENTRIES) {
                    flushBuffer();
                }
            }
        }

        flushBuffer();
    } catch (...) {
        for (const auto& [tmp, fin] : staged) {
            // Also best effort, and deliberately silent: this runs during
            // unwinding, and a cleanup failure must not displace the exception
            // that actually explains what went wrong.
            std::error_code ec;
            std::filesystem::remove(tmp, ec);
        }
        throw;
    }

    // Phase 2 - publish. Each rename is atomic, so a table is either absent or
    // present and complete; it is never observed half-written.
    for (const auto& [tmp, fin] : staged) {
        std::filesystem::rename(tmp, fin);
    }
    fsyncDir(data_dir_);

    // Phase 3 - only now are the inputs expendable; their contents are durable
    // under the new names.
    //
    // Unlike the two best-effort removals above, a failure here is a
    // correctness hazard rather than untidiness. An input that survives is
    // still named sst-N-*.sst, so loadSSTables() picks it up on the next start
    // alongside the L1 tables that replaced it — resurrecting keys the merge
    // superseded, and undoing deletes whose tombstones were dropped during
    // compaction.
    //
    // It is reported rather than thrown. By this point the replacements are
    // written, fsynced and renamed into place, so the compaction has in fact
    // succeeded; raising here would tell the caller otherwise and leave the
    // engine's in-memory table list inconsistent with the files on disk. The
    // damage is deferred to the next restart, and an operator who sees this can
    // remove the file before that happens.
    for (const auto& sst : sstables) {
        std::error_code ec;
        std::filesystem::remove(sst->path(), ec);
        if (ec) {
            std::cerr << "[Compaction] Failed to remove compacted input " << sst->path()
                      << ": " << ec.message()
                      << ". It will be reloaded on restart and may resurrect superseded keys.\n";
        }
    }
    fsyncDir(data_dir_);

    sstables.clear();
    for (const auto& [tmp, fin] : staged) {
        sstables.push_back(std::make_unique<SSTable>(fin));
    }

    std::cout << "[Compaction] Compaction finished. " << staged.size() << " Level 1 tables written.\n";
    sst_counter = 0; // Reset L0 counter for subsequent flushes
}
