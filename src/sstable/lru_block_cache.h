#pragma once
#include "sstable.h"
#include <unordered_map>
#include <list>
#include <mutex>
#include <string>
#include <vector>
#include <memory>
#include <optional>
#include <functional>

class BlockCache {
public:
    struct CacheKey {
        std::string sstable_path;
        uint64_t block_offset;

        bool operator==(const CacheKey& other) const {
            return block_offset == other.block_offset && sstable_path == other.sstable_path;
        }
    };

    struct CacheKeyHash {
        std::size_t operator()(const CacheKey& k) const {
            return std::hash<std::string>()(k.sstable_path) ^ (std::hash<uint64_t>()(k.block_offset) << 1);
        }
    };

    explicit BlockCache(size_t capacity_blocks = 1024, size_t max_bytes = 4 * 1024 * 1024)
        : capacity_(capacity_blocks), max_bytes_(max_bytes), current_bytes_(0) {}

    static size_t calculateBlockBytes(const std::vector<SSTable::Entry>& entries) {
        size_t bytes = sizeof(std::vector<SSTable::Entry>) + entries.capacity() * sizeof(SSTable::Entry);
        for (const auto& e : entries) {
            bytes += e.key.capacity();
            if (e.value.has_value()) {
                bytes += e.value->capacity();
            }
        }
        return bytes;
    }

    std::optional<std::vector<SSTable::Entry>> get(const std::string& path, uint64_t block_offset) {
        std::lock_guard<std::mutex> lock(mu_);
        CacheKey key{path, block_offset};
        auto it = map_.find(key);
        if (it == map_.end()) {
            return std::nullopt;
        }
        lru_list_.splice(lru_list_.begin(), lru_list_, it->second);
        return it->second->value;
    }

    void put(const std::string& path, uint64_t block_offset, std::vector<SSTable::Entry> entries) {
        std::lock_guard<std::mutex> lock(mu_);
        CacheKey key{path, block_offset};
        size_t new_bytes = calculateBlockBytes(entries);

        auto it = map_.find(key);
        if (it != map_.end()) {
            current_bytes_ -= it->second->byte_size;
            it->second->value = std::move(entries);
            it->second->byte_size = new_bytes;
            current_bytes_ += new_bytes;
            lru_list_.splice(lru_list_.begin(), lru_list_, it->second);
            evictIfNeeded();
            return;
        }

        lru_list_.push_front({key, std::move(entries), new_bytes});
        map_[key] = lru_list_.begin();
        current_bytes_ += new_bytes;
        evictIfNeeded();
    }

    void invalidatePath(const std::string& path) {
        std::lock_guard<std::mutex> lock(mu_);
        for (auto it = lru_list_.begin(); it != lru_list_.end(); ) {
            if (it->key.sstable_path == path) {
                current_bytes_ -= it->byte_size;
                map_.erase(it->key);
                it = lru_list_.erase(it);
            } else {
                ++it;
            }
        }
    }

    void clear() {
        std::lock_guard<std::mutex> lock(mu_);
        map_.clear();
        lru_list_.clear();
        current_bytes_ = 0;
    }

    size_t currentBytes() const {
        std::lock_guard<std::mutex> lock(mu_);
        return current_bytes_;
    }

    static BlockCache& globalInstance() {
        static BlockCache instance(1024, 4 * 1024 * 1024); // 1,024 blocks or 4 MB capacity
        return instance;
    }

private:
    struct Node {
        CacheKey key;
        std::vector<SSTable::Entry> value;
        size_t byte_size;
    };

    void evictIfNeeded() {
        while (!lru_list_.empty() && (map_.size() > capacity_ || current_bytes_ > max_bytes_)) {
            auto last = lru_list_.end();
            --last;
            current_bytes_ -= last->byte_size;
            map_.erase(last->key);
            lru_list_.pop_back();
        }
    }

    mutable std::mutex mu_;
    size_t capacity_;
    size_t max_bytes_;
    size_t current_bytes_;
    std::list<Node> lru_list_;
    std::unordered_map<CacheKey, std::list<Node>::iterator, CacheKeyHash> map_;
};
