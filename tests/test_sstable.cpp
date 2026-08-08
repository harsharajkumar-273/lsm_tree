#include "../src/sstable/sstable.h"
#include <iostream>
#include <cassert>
#include <filesystem>

void pass(const std::string& test) {
    std::cout << "  \033[32m✓\033[0m " << test << "\n";
}
void fail(const std::string& test, const std::string& msg) {
    std::cout << "  \033[31m✗\033[0m " << test << " — " << msg << "\n";
    std::exit(1);
}

int main() {
    std::cout << "===========================================\n";
    std::cout << "  Running Unit Test: SSTable & Block Boundaries\n";
    std::cout << "===========================================\n";

    std::filesystem::remove_all("/tmp/test_sstable_dir");
    std::filesystem::create_directories("/tmp/test_sstable_dir");
    std::string sst_path = "/tmp/test_sstable_dir/test.sst";

    // 1. Prepare entries spanning multiple 4KB blocks
    std::vector<SSTable::Entry> entries;
    const int num_entries = 500;
    for (int i = 0; i < num_entries; ++i) {
        entries.push_back({"key_" + std::to_string(1000 + i), "val_" + std::to_string(1000 + i)});
    }

    SSTable::write(sst_path, entries);

    // 2. Read back SSTable
    SSTable sst(sst_path);
    if (sst.smallestKey() != "key_1000") fail("SSTable", "Smallest key mismatch");
    if (sst.largestKey() != "key_1499") fail("SSTable", "Largest key mismatch");
    if (sst.indexSize() <= 1) fail("SSTable Block Count", "Expected small dataset to span multiple 4KB blocks");
    pass("Smallest/Largest Key Metadata & 4KB Block Count Assertions Correct");

    // 3. Verify get() lookups across blocks
    for (int i = 0; i < num_entries; i += 25) {
        std::string target_key = "key_" + std::to_string(1000 + i);
        auto val = sst.get(target_key);
        if (!val || *val != "val_" + std::to_string(1000 + i)) {
            fail("SSTable Get", "Failed to retrieve key: " + target_key);
        }
    }
    pass("Point Lookups Across Target 4KB Block Boundaries");

    // 4. Test Large Payload Entry (16KB per entry > 4KB target block size)
    std::string large_sst_path = "/tmp/test_sstable_dir/large_test.sst";
    std::vector<SSTable::Entry> large_entries;
    std::string large_val(16 * 1024, 'Z'); // 16 KB payload
    for (int i = 0; i < 10; ++i) {
        large_entries.push_back({"lkey_" + std::to_string(i), large_val});
    }
    SSTable::write(large_sst_path, large_entries);

    SSTable large_sst(large_sst_path);
    if (large_sst.indexSize() != 10) {
        fail("SSTable Large Block Sizing", "Expected each 16KB large entry to start its own block (expected 10 blocks)");
    }

    auto lval = large_sst.get("lkey_5");
    if (!lval || *lval != large_val) {
        fail("SSTable Large Payload", "Failed to retrieve large payload key lkey_5");
    }
    pass("Large Payload Block Sizing Assertions & Boundaries Correct");

    // 5. Test Non-existent Key
    auto missing = sst.get("key_9999");
    if (missing.has_value()) {
        fail("SSTable Missing Key", "Expected non-existent key to return nullopt");
    }
    pass("Non-existent Key Handled Correctly");

    std::cout << "\033[32mSSTable tests passed successfully.\033[0m\n\n";
    return 0;
}
