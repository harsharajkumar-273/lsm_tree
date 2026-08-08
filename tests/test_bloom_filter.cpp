#include "../src/sstable/block_bloom_filter.h"
#include <iostream>
#include <cassert>

void pass(const std::string& test) {
    std::cout << "  \033[32m✓\033[0m " << test << "\n";
}
void fail(const std::string& test, const std::string& msg) {
    std::cout << "  \033[31m✗\033[0m " << test << " — " << msg << "\n";
    std::exit(1);
}

int main() {
    std::cout << "===========================================\n";
    std::cout << "  Running Unit Test: Vectorized Block Bloom Filter\n";
    std::cout << "===========================================\n";

    BloomFilter bf(1000, 0.01);

    bf.insert("user:1");
    bf.insert("user:2");
    bf.insert("user:3");

    // No false negatives
    if (!bf.mayContain("user:1")) fail("Bloom Filter", "Expected user:1 to be present");
    if (!bf.mayContain("user:2")) fail("Bloom Filter", "Expected user:2 to be present");
    if (!bf.mayContain("user:3")) fail("Bloom Filter", "Expected user:3 to be present");
    pass("Correctness: No false negatives for inserted keys");

    // False positives calculation over 10,000 queries
    int fp = 0;
    const int QUERIES = 10000;
    for (int i = 0; i < QUERIES; ++i) {
        if (bf.mayContain("missing_key_" + std::to_string(i))) {
            fp++;
        }
    }

    double fp_rate = static_cast<double>(fp) / QUERIES;
    std::cout << "  - False positives: " << fp << " / " << QUERIES << " (" << fp_rate * 100.0 << "%)\n";

    if (fp_rate > 0.05) {
        fail("False positive rate", "FPR is higher than expected threshold (5%): " + std::to_string(fp_rate));
    }
    pass("Correctness: False positive rate under 5%");

    // Test serialization & deserialization
    auto serialized = bf.serialize();
    auto deserialized_bf = BloomFilter::deserialize(bf.blockCount(), bf.hashCount(), serialized);
    
    // Every inserted key is checked, not a subset. user:3 was previously
    // omitted, so a round trip that dropped only the last key would have passed.
    if (!deserialized_bf.mayContain("user:1") ||
        !deserialized_bf.mayContain("user:2") ||
        !deserialized_bf.mayContain("user:3")) {
        fail("Serialization", "Deserialized filter lost keys");
    }
    pass("Serialization and Deserialization successful");

    // The checks above use a filter sized for 1000 keys holding three, where
    // the false positive rate is effectively zero and the 5% assertion cannot
    // fail however broken the filter is. This fills one to the capacity it was
    // sized for, which is the condition the design target describes.
    {
        const size_t CAPACITY = 1000;
        BloomFilter full(CAPACITY, 0.01);
        for (size_t i = 0; i < CAPACITY; ++i) {
            full.insert("capacity_key_" + std::to_string(i));
        }

        // A bloom filter may not report a false negative at any load.
        for (size_t i = 0; i < CAPACITY; ++i) {
            if (!full.mayContain("capacity_key_" + std::to_string(i))) {
                fail("Bloom Filter at capacity",
                     "False negative for capacity_key_" + std::to_string(i));
            }
        }
        pass("Correctness: No false negatives at full capacity");

        int full_fp = 0;
        const int CAPACITY_QUERIES = 100000;
        for (int i = 0; i < CAPACITY_QUERIES; ++i) {
            if (full.mayContain("capacity_absent_" + std::to_string(i))) {
                full_fp++;
            }
        }

        const double full_fp_rate = static_cast<double>(full_fp) / CAPACITY_QUERIES;
        std::cout << "  - At capacity (" << CAPACITY << " keys): " << full_fp
                  << " / " << CAPACITY_QUERIES << " (" << full_fp_rate * 100.0 << "%)\n";

        // Measured at 1.396% on this filter, against a 1% design target. The
        // threshold is deliberately loose enough not to be flaky and tight
        // enough that a filter whose probes collapse onto too few bits fails.
        if (full_fp_rate > 0.05) {
            fail("False positive rate at capacity",
                 "FPR exceeds 5% threshold: " + std::to_string(full_fp_rate));
        }
        pass("Correctness: False positive rate under 5% at full capacity");
    }

    std::cout << "\033[32mBloom Filter tests passed successfully.\033[0m\n\n";
    return 0;
}
