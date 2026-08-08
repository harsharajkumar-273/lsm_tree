#include "sstable.h"
#include <fstream>
#include <stdexcept>
#include <fcntl.h>
#include <unistd.h>
#include <cstring>
#include <cerrno>
#include <filesystem>
#include <limits>

namespace {

// Flushes a directory entry, which is what makes a newly created name durable.
//
// leveled_compactor.cpp has an equivalent helper for its renames. That one is
// silent on failure because it runs after the rename has already committed;
// this one runs while the caller can still be told the table is not safe, so it
// throws instead.
void fsyncParentDir(const std::string& file_path) {
    const std::filesystem::path parent =
        std::filesystem::path(file_path).parent_path();
    const std::string dir = parent.empty() ? std::string(".") : parent.string();

    int fd = ::open(dir.c_str(), O_RDONLY | O_DIRECTORY);
    if (fd < 0) {
        throw std::runtime_error("SSTable: cannot open directory for fsync: " + dir + ": " +
                                 std::strerror(errno));
    }
    if (::fsync(fd) != 0) {
        const int err = errno;
        ::close(fd);
        throw std::runtime_error("SSTable: directory fsync failed for " + dir + ": " +
                                 std::strerror(err));
    }
    if (::close(fd) != 0) {
        throw std::runtime_error("SSTable: directory close after fsync failed for " + dir +
                                 ": " + std::strerror(errno));
    }
}

// Removes a half-written temporary before reporting the original failure.
//
// Errors here are deliberately swallowed: the caller is already being told the
// write failed, and a cleanup problem must not replace that with a less useful
// message. A leftover ".tmp" is also swept on startup by the compactor, so it
// cannot be mistaken for a real table either way.
void discardPartial(const std::string& tmp_path) {
    std::error_code ec;
    std::filesystem::remove(tmp_path, ec);
}

}  // namespace

void SSTable::write(const std::string& path, const std::vector<Entry>& entries) {
    // Sorted, unique input is a precondition, so it is checked rather than
    // assumed.
    //
    // The sparse index samples every ENTRIES_PER_BLOCK-th key, and findBlock()
    // binary-searches that index; get() then scans forward from the block it
    // lands on, stopping early once it passes the target key. All three depend
    // on the entries being ordered. Unsorted input does not fail loudly — it
    // produces a well-formed file whose index is wrong, so reads return "not
    // found" for keys that are present, and the corruption is only visible much
    // later at query time.
    //
    // Duplicates are rejected too: two entries with the same key make the block
    // a value resolves to ambiguous, and both existing callers already
    // guarantee uniqueness — the compactor merges through a std::map, and
    // flushMemtable iterates a skiplist.
    //
    // Validated before the file is opened, so an invalid call leaves nothing
    // behind on disk.
    for (size_t i = 1; i < entries.size(); ++i) {
        if (!(entries[i - 1].key < entries[i].key)) {
            throw std::runtime_error(
                "SSTable: entries must be sorted by key and unique; '" + entries[i - 1].key +
                "' is not less than '" + entries[i].key + "' at position " + std::to_string(i));
        }
    }

    // Written under a temporary name and renamed once durable — see the rename
    // at the end of this function for why.
    const std::string tmp_path = path + ".tmp";

    std::ofstream out(tmp_path, std::ios::binary | std::ios::trunc);
    if (!out.is_open())
        throw std::runtime_error("SSTable: cannot create file: " + tmp_path);

    BloomFilter bloom(std::max(entries.size(), size_t(1)));
    std::vector<std::pair<std::string, uint64_t>> index;

    uint64_t current_block_bytes = 0;
    for (size_t i = 0; i < entries.size(); ++i) {
        size_t entry_bytes = 1 + 4 + entries[i].key.size() + 4 + (entries[i].value.has_value() ? entries[i].value->size() : 0);

        if (i == 0 || current_block_bytes >= TARGET_BLOCK_SIZE) {
            index.push_back({entries[i].key, static_cast<uint64_t>(out.tellp())});
            current_block_bytes = 0;
        }

        bloom.insert(entries[i].key);
        writeEntry(out, entries[i]);
        current_block_bytes += entry_bytes;
    }

    uint64_t index_offset = static_cast<uint64_t>(out.tellp());
    if (index.size() > std::numeric_limits<uint32_t>::max()) {
        throw std::length_error("SSTable: index entry count exceeds uint32_t");
    }
    uint32_t index_count  = static_cast<uint32_t>(index.size());
    writeUint32(out, index_count);
    for (auto& [key, offset] : index) {
        writeString(out, key);
        writeUint64(out, offset);
    }

    uint64_t bloom_offset = static_cast<uint64_t>(out.tellp());
    serializeBloom(out, bloom);

    writeUint64(out, index_offset);
    writeUint64(out, bloom_offset);

    // Stream state is checked rather than assumed.
    //
    // ofstream reports failure by setting badbit, not by throwing, so a write
    // that ran out of space simply stopped happening. Every write above then
    // proceeded against a stream already in a failed state, and the caller was
    // handed a truncated file it believed was complete — which matters because
    // flushMemtable clears the WAL once this returns.
    out.flush();
    if (!out) {
        discardPartial(tmp_path);
        throw std::runtime_error("SSTable: failed while writing " + path +
                                 " (out of space?); file is incomplete");
    }

    out.close();
    if (!out) {
        discardPartial(tmp_path);
        throw std::runtime_error("SSTable: failed while closing " + path +
                                 "; buffered data may not have reached disk");
    }

    // fsync the file to ensure durability before the WAL is cleared.
    //
    // Both the open and the fsync used to be best effort: `if (fd >= 0)` meant a
    // failed open skipped the sync entirely and said nothing, and the fsync
    // result was discarded. Either way the caller was told the table was durable
    // when it was not, and the WAL holding the only other copy was then cleared.
    int fd = ::open(tmp_path.c_str(), O_RDONLY);
    if (fd < 0) {
        discardPartial(tmp_path);
        throw std::runtime_error("SSTable: cannot reopen for fsync: " + tmp_path + ": " +
                                 std::strerror(errno));
    }
    if (::fsync(fd) != 0) {
        const int err = errno;
        ::close(fd);
        discardPartial(tmp_path);
        throw std::runtime_error("SSTable: fsync failed for " + tmp_path + ": " +
                                 std::strerror(err));
    }
    if (::close(fd) != 0) {
        const int err = errno;
        discardPartial(tmp_path);
        throw std::runtime_error("SSTable: close after fsync failed for " + tmp_path + ": " +
                                 std::strerror(err));
    }

    // Only now does the table take its real name.
    //
    // Writing straight to `path` meant any failure above left a truncated file
    // under the name the engine scans for, so a partial table was picked up as a
    // real one on the next startup — deferring the corruption from write time to
    // load time rather than preventing it. rename(2) is atomic within a
    // filesystem, so the name either resolves to a fully-synced table or does
    // not exist.
    //
    // This mirrors what leveled_compactor.cpp already does for merged output,
    // including the ".tmp" suffix its startup sweep knows to delete.
    std::error_code ec;
    std::filesystem::rename(tmp_path, path, ec);
    if (ec) {
        discardPartial(tmp_path);
        throw std::runtime_error("SSTable: cannot publish " + path + ": " + ec.message());
    }

    // The contents are durable; make the name that reaches them durable too.
    // Without this a crash can leave the data on disk with no directory entry
    // pointing at it.
    fsyncParentDir(path);
}

void SSTable::writeEntry(std::ofstream& out, const Entry& e) {
    uint8_t is_tombstone = e.value.has_value() ? 0 : 1;
    out.write(reinterpret_cast<const char*>(&is_tombstone), 1);
    writeString(out, e.key);
    writeString(out, e.value.value_or(""));
}

void SSTable::writeString(std::ofstream& out, const std::string& s) {
    // Same 4 GiB ceiling as the WAL: the length field is uint32_t, so a longer
    // string would be written in full but recorded with a truncated length, and
    // every subsequent read of the file would be misaligned.
    if (s.size() > std::numeric_limits<uint32_t>::max()) {
        throw std::length_error(
            "SSTable: string exceeds the 4 GiB limit imposed by the on-disk "
            "length field (" + std::to_string(s.size()) + " bytes)");
    }

    uint32_t len = static_cast<uint32_t>(s.size());
    out.write(reinterpret_cast<const char*>(&len), 4);
    out.write(s.data(), len);
}

void SSTable::writeUint32(std::ofstream& out, uint32_t v) { out.write(reinterpret_cast<const char*>(&v), 4); }
void SSTable::writeUint64(std::ofstream& out, uint64_t v) { out.write(reinterpret_cast<const char*>(&v), 8); }

void SSTable::serializeBloom(std::ofstream& out, const BloomFilter& bf) {
    uint64_t num_hashes = static_cast<uint64_t>(bf.hashCount());
    uint64_t num_blocks = static_cast<uint64_t>(bf.blockCount());
    out.write(reinterpret_cast<const char*>(&num_hashes), 8);
    out.write(reinterpret_cast<const char*>(&num_blocks), 8);
    auto data = bf.serialize();
    out.write(reinterpret_cast<const char*>(data.data()), data.size());
}
