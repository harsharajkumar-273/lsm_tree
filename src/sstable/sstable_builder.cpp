#include "sstable.h"
#include <fstream>
#include <stdexcept>
#include <fcntl.h>
#include <unistd.h>
#include <cstring>
#include <limits>

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

    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out.is_open())
        throw std::runtime_error("SSTable: cannot create file: " + path);

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
    out.flush();
    out.close();

    // fsync the file to ensure durability before the WAL is cleared.
    int fd = open(path.c_str(), O_RDONLY);
    if (fd >= 0) {
        fsync(fd);
        close(fd);
    }
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
