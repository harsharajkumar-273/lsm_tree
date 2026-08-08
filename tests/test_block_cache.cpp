#include "../src/sstable/lru_block_cache.h"
#include "../src/sstable/sstable.h"
#include <iostream>
#include <cassert>
#include <filesystem>
#include <chrono>

void pass(const std::string& test) {
    std::cout << "  \033[32m✓\033[0m " << test << "\n";
}
void fail(const std::string& test, const std::string& msg) {
    std::cout << "  \033[31m✗\033[0m " << test << " — " << msg << "\n";
    std::exit(1);
}

int main() {
    std::cout << "===========================================\n";
    std::cout << "  Running Unit Test: LRU Block Cache & Pread\n";
    std::cout << "===========================================\n";

    // 1. Test standalone BlockCache PUT / GET / Eviction
    {
        BlockCache cache(2); // capacity 2 blocks
        std::vector<SSTable::Entry> blockA = {{"key1", "val1"}};
        std::vector<SSTable::Entry> blockB = {{"key2", "val2"}};
        std::vector<SSTable::Entry> blockC = {{"key3", "val3"}};

        cache.put("file1", 0, blockA);
        cache.put("file1", 4096, blockB);

        auto resA = cache.get("file1", 0);
        if (!resA || resA->front().key != "key1") fail("LRU Cache", "Failed to retrieve blockA");

        // Adding blockC should evict blockB (since blockA was recently accessed via get)
        cache.put("file1", 8192, blockC);

        auto resB = cache.get("file1", 4096);
        if (resB.has_value()) fail("LRU Eviction", "Expected blockB to be evicted");
        pass("LRU Block Cache Put, Get & Eviction Logic");
    }

    // 2. Test SSTable Reads with Block Cache & Persistent File Handle
    std::filesystem::remove_all("/tmp/test_block_cache_dir");
    std::filesystem::create_directories("/tmp/test_block_cache_dir");
    std::string sst_path = "/tmp/test_block_cache_dir/cache_test.sst";

    std::vector<SSTable::Entry> entries;
    for (int i = 0; i < 200; ++i) {
        entries.push_back({"ckey_" + std::to_string(1000 + i), "cval_" + std::to_string(1000 + i)});
    }
    SSTable::write(sst_path, entries);

    SSTable sst(sst_path);
    if (sst.fd() < 0) fail("File Handle Reuse", "Expected open file descriptor fd_ >= 0");
    pass("Persistent File Descriptor Opened Successfully");

    // First read -> Cache MISS (populates cache via pread)
    auto t0 = std::chrono::high_resolution_clock::now();
    auto v1 = sst.get("ckey_1050");
    auto t1 = std::chrono::high_resolution_clock::now();
    double miss_us = std::chrono::duration<double, std::micro>(t1 - t0).count();

    if (!v1 || *v1 != "cval_1050") fail("SSTable Get", "Failed to retrieve ckey_1050 on cache miss");

    // Second read -> Cache HIT (retrieved instantly from LRU Block Cache in RAM)
    t0 = std::chrono::high_resolution_clock::now();
    auto v2 = sst.get("ckey_1050");
    t1 = std::chrono::high_resolution_clock::now();
    double hit_us = std::chrono::duration<double, std::micro>(t1 - t0).count();

    if (!v2 || *v2 != "cval_1050") fail("SSTable Get", "Failed to retrieve ckey_1050 on cache hit");
    pass("Cache HIT and MISS correctness verified");

    std::cout << "  - Cache MISS latency: " << miss_us << " us\n";
    std::cout << "  - Cache HIT latency:  " << hit_us << " us\n";

    std::cout << "\033[32mLRU Block Cache & Pread tests passed successfully.\033[0m\n\n";
    return 0;
}
