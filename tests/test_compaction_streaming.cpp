#include "../src/db/lsm_engine.h"
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
    std::cout << "  Running Unit Test: Streaming K-Way Compaction\n";
    std::cout << "===========================================\n";

    std::filesystem::remove_all("/tmp/lsm_test_compaction");
    {
        LSMEngine db("/tmp/lsm_test_compaction", 1024); // Small memtable to trigger flushes

        // Write 4 batches to force 4 L0 SSTables
        for (int b = 0; b < 4; ++b) {
            for (int i = 0; i < 50; ++i) {
                db.put("key_" + std::to_string(i), "batch_" + std::to_string(b));
            }
            db.flush(); // Flush MemTable to create L0 SSTable
        }

        // Overwrite some keys and delete key_5
        db.put("key_0", "final_val_0");
        db.del("key_5");
        db.flush(); // 5th L0 SSTable -> Compaction should trigger (L0_THRESHOLD = 4)

        // Query key_0 -> Should yield final_val_0
        auto v0 = db.get("key_0");
        if (!v0 || *v0 != "final_val_0") {
            fail("Compaction Overwrite", "Expected key_0 to have 'final_val_0'");
        }
        pass("Compaction correctly preserves latest updates across multiple L0 runs");

        // Query key_5 -> Should be missing (tombstone purged)
        auto v5 = db.get("key_5");
        if (v5.has_value()) {
            fail("Compaction Tombstone", "Expected deleted key_5 to be purged during compaction");
        }
        pass("Compaction correctly purges tombstones");

        // Query key_49 -> Should yield batch_3
        auto v49 = db.get("key_49");
        if (!v49 || *v49 != "batch_3") {
            fail("Compaction Value", "Expected key_49 to have 'batch_3'");
        }
        pass("Streaming merge data integrity verified across all SSTables");
    }

    std::cout << "\033[32mStreaming Compaction tests passed successfully.\033[0m\n\n";
    return 0;
}
