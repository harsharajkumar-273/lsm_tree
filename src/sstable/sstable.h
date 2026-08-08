#pragma once
#include "block_bloom_filter.h"
#include <string>
#include <vector>
#include <optional>
#include <memory>
#include <cstdint>
#include <fstream>

class SSTableIterator;

class SSTable {
public:
    static constexpr size_t TARGET_BLOCK_SIZE = 4096;

    struct Entry {
        std::string key;
        std::optional<std::string> value;
    };

    static void write(const std::string& path, const std::vector<Entry>& entries);

    explicit SSTable(const std::string& path);
    ~SSTable();

    SSTable(const SSTable&) = delete;
    SSTable& operator=(const SSTable&) = delete;
    SSTable(SSTable&& other) noexcept;
    SSTable& operator=(SSTable&& other) noexcept;

    std::optional<std::optional<std::string>> get(const std::string& key) const;
    std::vector<Entry> readAll() const;
    std::unique_ptr<SSTableIterator> createIterator() const;

    const std::string& smallestKey() const { return smallest_key_; }
    const std::string& largestKey()  const { return largest_key_; }
    const std::string& path()        const { return path_; }
    uint64_t indexOffset()            const { return index_offset_; }
    size_t indexSize()                const { return index_.size(); }
    int fd()                          const { return fd_; }

private:
    std::string path_;
    int fd_ = -1;
    std::vector<std::pair<std::string, uint64_t>> index_;
    std::unique_ptr<BloomFilter> bloom_;
    std::string smallest_key_;
    std::string largest_key_;
    uint64_t file_size_ = 0;
    uint64_t index_offset_ = 0;

    uint64_t findBlock(const std::string& key) const;

    static void writeEntry(std::ofstream& out, const Entry& e);
    static Entry readEntry(std::ifstream& in, uint64_t max_len);
    static void writeString(std::ofstream& out, const std::string& s);
    // max_len bounds the length prefix taken from disk. A stored string cannot
    // be longer than the file containing it, so callers pass the file size.
    static std::string readString(std::ifstream& in, uint64_t max_len);
    static void writeUint32(std::ofstream& out, uint32_t v);
    static uint32_t readUint32(std::ifstream& in);
    static void writeUint64(std::ofstream& out, uint64_t v);
    static uint64_t readUint64(std::ifstream& in);

    static void serializeBloom(std::ofstream& out, const BloomFilter& bf);
    static std::unique_ptr<BloomFilter> deserializeBloom(std::ifstream& in);
};

class SSTableIterator {
public:
    SSTableIterator(const std::string& path, uint64_t data_end_offset);

    bool hasNext() const { return current_.has_value(); }
    SSTable::Entry peek() const { return *current_; }
    SSTable::Entry next();

private:
    std::ifstream in_;
    uint64_t data_end_offset_;
    std::optional<SSTable::Entry> current_;

    void advance();
};
