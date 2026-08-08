#include "sstable.h"
#include <fstream>
#include <stdexcept>
#include <algorithm>

SSTable::SSTable(const std::string& path) : path_(path) {
    std::ifstream in(path, std::ios::binary | std::ios::ate);
    if (!in.is_open())
        throw std::runtime_error("SSTable: cannot open: " + path);

    uint64_t file_size = in.tellg();
    if (file_size < 16)
        throw std::runtime_error("SSTable: file too small: " + path);

    in.seekg(-16, std::ios::end);
    uint64_t index_offset = readUint64(in);
    uint64_t bloom_offset = readUint64(in);
    index_offset_ = index_offset;

    // index_offset and bloom_offset are read from the footer, so they are as
    // untrusted as anything else on disk. A corrupted pair sends the seeks
    // below to arbitrary positions.
    if (index_offset > file_size || bloom_offset > file_size || index_offset > bloom_offset) {
        throw std::runtime_error("SSTable: footer offsets fall outside the file: " + path);
    }

    in.seekg(index_offset);
    uint32_t index_count = readUint32(in);

    // Bound the count by what the index region can physically hold.
    //
    // index_.resize(index_count) previously took a 32-bit value straight from
    // disk, so a corrupted count reserved up to 4 billion entries before a
    // single one was read — the std::bad_alloc this issue describes.
    //
    // Every entry costs at least a 4-byte key length, zero key bytes, and an
    // 8-byte block offset, so twelve bytes is the floor. The region runs from
    // just past the count to the start of the bloom section. Dividing rather
    // than multiplying keeps the comparison free of any product that could
    // overflow.
    constexpr uint64_t kMinIndexEntryBytes = 4 + 8;
    const uint64_t index_region = bloom_offset - index_offset;
    const uint64_t available = index_region >= 4 ? index_region - 4 : 0;

    if (static_cast<uint64_t>(index_count) > available / kMinIndexEntryBytes) {
        throw std::runtime_error("SSTable: index entry count exceeds file contents: " + path);
    }

    index_.resize(index_count);
    for (auto& [key, offset] : index_) {
        key    = readString(in);
        offset = readUint64(in);
    }

    in.seekg(bloom_offset);
    bloom_ = deserializeBloom(in);

    if (!index_.empty()) {
        smallest_key_ = index_.front().first;
        in.seekg(index_.back().second);
        Entry e;
        while (static_cast<uint64_t>(in.tellg()) < index_offset)
            e = readEntry(in);
        largest_key_ = e.key;
    }
}

std::optional<std::optional<std::string>> SSTable::get(const std::string& key) const {
    if (!bloom_->mayContain(key))
        return std::nullopt;

    if (index_.empty()) return std::nullopt;

    int lo = 0, hi = static_cast<int>(index_.size()) - 1;
    while (lo < hi) {
        int mid = lo + (hi - lo + 1) / 2;
        if (index_[mid].first <= key) lo = mid;
        else hi = mid - 1;
    }

    uint64_t block_start_offset = index_[lo].second;
    uint64_t block_end_offset   = (static_cast<size_t>(lo + 1) < index_.size())
                                  ? index_[lo + 1].second
                                  : index_offset_;

    std::ifstream in(path_, std::ios::binary);
    if (!in.is_open()) return std::nullopt;

    in.seekg(block_start_offset);
    while (static_cast<uint64_t>(in.tellg()) < block_end_offset && in.peek() != EOF) {
        Entry e = readEntry(in);
        if (e.key == key) return e.value;
        if (e.key > key)  return std::nullopt;
    }
    return std::nullopt;
}

std::vector<SSTable::Entry> SSTable::readAll() const {
    std::ifstream in(path_, std::ios::binary);
    if (!in.is_open()) return {};
    std::vector<Entry> entries;
    // Read the index_offset from the footer
    in.seekg(-16, std::ios::end);
    uint64_t idx_off = readUint64(in);
    in.seekg(0);
    while (static_cast<uint64_t>(in.tellg()) < idx_off && in.peek() != EOF) {
        entries.push_back(readEntry(in));
    }
    return entries;
}

SSTableIterator::SSTableIterator(const std::string& path, uint64_t data_end_offset)
    : in_(path, std::ios::binary), data_end_offset_(data_end_offset) {
    advance();
}

void SSTableIterator::advance() {
    if (in_.is_open() && static_cast<uint64_t>(in_.tellg()) < data_end_offset_ && in_.peek() != EOF) {
        uint8_t is_tombstone = 0;
        in_.read(reinterpret_cast<char*>(&is_tombstone), 1);
        uint32_t key_len = 0;
        in_.read(reinterpret_cast<char*>(&key_len), 4);
        std::string key(key_len, '\0');
        in_.read(key.data(), key_len);

        uint32_t val_len = 0;
        in_.read(reinterpret_cast<char*>(&val_len), 4);
        std::string val(val_len, '\0');
        in_.read(val.data(), val_len);

        SSTable::Entry e;
        e.key = std::move(key);
        e.value = is_tombstone ? std::nullopt : std::optional<std::string>(std::move(val));
        current_ = std::move(e);
    } else {
        current_ = std::nullopt;
    }
}

SSTable::Entry SSTableIterator::next() {
    SSTable::Entry res = std::move(*current_);
    advance();
    return res;
}

std::unique_ptr<SSTableIterator> SSTable::createIterator() const {
    return std::make_unique<SSTableIterator>(path_, index_offset_);
}

uint64_t SSTable::findBlock(const std::string& key) const {
    if (index_.empty()) return 0;
    int lo = 0, hi = static_cast<int>(index_.size()) - 1;
    while (lo < hi) {
        int mid = lo + (hi - lo + 1) / 2;
        if (index_[mid].first <= key) lo = mid;
        else hi = mid - 1;
    }
    return index_[lo].second;
}

SSTable::Entry SSTable::readEntry(std::ifstream& in) {
    uint8_t is_tombstone;
    in.read(reinterpret_cast<char*>(&is_tombstone), 1);
    Entry e;
    e.key = readString(in);
    std::string val = readString(in);
    e.value = is_tombstone ? std::nullopt : std::optional<std::string>(val);
    return e;
}

std::string SSTable::readString(std::ifstream& in) {
    uint32_t len = 0;
    in.read(reinterpret_cast<char*>(&len), 4);
    std::string s(len, '\0');
    in.read(s.data(), len);
    return s;
}

uint32_t SSTable::readUint32(std::ifstream& in) { uint32_t v = 0; in.read(reinterpret_cast<char*>(&v), 4); return v; }
uint64_t SSTable::readUint64(std::ifstream& in) { uint64_t v = 0; in.read(reinterpret_cast<char*>(&v), 8); return v; }

std::unique_ptr<BloomFilter> SSTable::deserializeBloom(std::ifstream& in) {
    uint64_t num_hashes, num_blocks;
    in.read(reinterpret_cast<char*>(&num_hashes), 8);
    in.read(reinterpret_cast<char*>(&num_blocks), 8);

    // num_blocks comes from the file, and `num_blocks * 64` sized an allocation
    // directly from it. A corrupted or truncated footer could therefore ask for
    // an arbitrary amount of memory before a single byte was validated, and the
    // multiplication itself overflows once num_blocks exceeds 2^58.
    //
    // Bound it by what the file can actually contain: the bloom section runs
    // from here to the start of the 16-byte footer. The comparison is done by
    // division so no product is formed before the check.
    const std::streampos here = in.tellg();
    in.seekg(0, std::ios::end);
    const uint64_t file_size = static_cast<uint64_t>(in.tellg());
    in.seekg(here);

    constexpr uint64_t kFooterBytes = 16;
    constexpr uint64_t kBlockBytes = 64;
    const uint64_t pos = static_cast<uint64_t>(here);
    const uint64_t available = (file_size >= pos + kFooterBytes) ? (file_size - pos - kFooterBytes) : 0;

    if (num_blocks > available / kBlockBytes) {
        throw std::runtime_error("SSTable: bloom filter block count exceeds file contents");
    }

    std::vector<uint8_t> data(num_blocks * kBlockBytes);
    in.read(reinterpret_cast<char*>(data.data()), data.size());
    return std::make_unique<BloomFilter>(BloomFilter::deserialize(num_blocks, num_hashes, data));
}
