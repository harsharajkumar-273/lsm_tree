#pragma once
#include <vector>
#include <string>
#include <cstdint>
#include <cmath>
#include <algorithm>
#include <cstring>

// Vectorized Block Bloom Filter: Aligned to 64-byte CPU cache lines.
// This ensures that checking for a key takes exactly one cache-miss penalty.
//
// Structure:
//   Filter consists of M blocks.
//   Each block is exactly 64 bytes (512 bits) and 64-byte aligned.
//   Hash1 selects the block index.
//   Hash2...HashK select bits ONLY within that chosen block.

class BloomFilter {
public:
    struct alignas(64) Block {
        uint64_t bits[8]; // 8 * 8 bytes = 64 bytes = 512 bits
        Block() { std::fill(std::begin(bits), std::end(bits), 0); }
    };

    explicit BloomFilter(size_t expected_keys, double fpr = 0.01) {
        // Optimal total bits: m = -n * ln(p) / (ln2)^2
        size_t total_bits = static_cast<size_t>(
            -static_cast<double>(expected_keys) * std::log(fpr) / (std::log(2.0) * std::log(2.0))
        );
        
        // Convert to number of 512-bit blocks
        num_blocks_ = (total_bits + 511) / 512;
        if (num_blocks_ == 0) num_blocks_ = 1;
        blocks_.resize(num_blocks_);

        // Optimal hashes for a standard filter is (m/n)ln2.
        // For blocked filters, we typically use a fixed number of hashes (e.g., 8)
        // because we are constrained to 512 bits per block anyway.
        num_hashes_ = 8; 
    }

    void insert(const std::string& key) {
        uint64_t h1, h2;
        hash(key, h1, h2);
        
        size_t block_idx = h1 % num_blocks_;
        Block& block = blocks_[block_idx];

        for (size_t i = 0; i < num_hashes_; ++i) {
            uint64_t bit_idx = (h1 + i * h2) % 512;
            block.bits[bit_idx / 64] |= (1ULL << (bit_idx % 64));
        }
    }

    bool mayContain(const std::string& key) const {
        uint64_t h1, h2;
        hash(key, h1, h2);
        
        size_t block_idx = h1 % num_blocks_;
        const Block& block = blocks_[block_idx];

        for (size_t i = 0; i < num_hashes_; ++i) {
            uint64_t bit_idx = (h1 + i * h2) % 512;
            if (!(block.bits[bit_idx / 64] & (1ULL << (bit_idx % 64)))) {
                return false;
            }
        }
        return true;
    }

    size_t blockCount() const { return num_blocks_; }
    size_t hashCount()  const { return num_hashes_; }

    std::vector<uint8_t> serialize() const {
        std::vector<uint8_t> buf(num_blocks_ * 64);
        std::memcpy(buf.data(), blocks_.data(), buf.size());
        return buf;
    }

    static BloomFilter deserialize(size_t num_blocks, size_t num_hashes, const std::vector<uint8_t>& data) {
        BloomFilter bf(1); // dummy

        // num_blocks and num_hashes arrive from the SSTable footer, i.e. from
        // disk, so they are untrusted. The constructor clamps num_blocks_ to at
        // least 1 precisely because insert() and mayContain() reduce modulo it;
        // assigning the members directly here bypasses that clamp, and a
        // truncated or corrupted file carrying num_blocks == 0 turns the next
        // `h1 % num_blocks_` into a division by zero (SIGFPE).
        if (num_blocks == 0) num_blocks = 1;
        if (num_hashes == 0) num_hashes = 1;

        bf.num_blocks_ = num_blocks;
        bf.num_hashes_ = num_hashes;
        bf.blocks_.resize(num_blocks);
        if (data.size() >= num_blocks * 64) {
            std::memcpy(bf.blocks_.data(), data.data(), num_blocks * 64);
        }
        return bf;
    }

private:
    std::vector<Block> blocks_;
    size_t num_blocks_;
    size_t num_hashes_;

    // Single pass to generate two 64-bit hashes (Double Hashing)
    //
    // Both hashes are accumulated over the key in the same loop, so this is
    // still one pass, but they are now independent of one another.
    //
    // h2 used to be a bit-mix of h1. That made it a pure function of h1, so the
    // pair carried 64 bits of entropy rather than 128 and any two keys sharing
    // h1 shared all eight probe positions — the two hashes double hashing
    // assumes are independent were the same hash twice over.
    //
    // The probe step is also forced odd. Positions are taken as
    // (h1 + i * h2) % 512, and 512 is 2^9, so a step sharing a factor with it
    // walks a subgroup instead of the whole block. A step that is a multiple of
    // 128 makes the eight probes collapse onto fewer than eight distinct bits,
    // and a step of 0 puts all eight on one bit — that arose for roughly 1 key
    // in 100 as measured. An odd step is coprime with 512, so the eight
    // positions are always distinct and can reach every bit in the block.
    void hash(const std::string& key, uint64_t& h1, uint64_t& h2) const {
        h1 = 14695981039346656037ULL; // FNV-1a offset basis
        h2 = 1099511628211ULL;        // distinct basis for the second hash

        for (unsigned char c : key) {
            h1 ^= c;
            h1 *= 1099511628211ULL;

            h2 ^= c;
            h2 *= 0x9E3779B97F4A7C15ULL;   // independent multiplier
            h2 = (h2 << 27) | (h2 >> 37);  // rotate so low bits depend on high ones
        }

        // Final avalanche, then force the step odd.
        h2 ^= h2 >> 33;
        h2 *= 0xff51afd7ed558ccdULL;
        h2 ^= h2 >> 29;
        h2 |= 1ULL;
    }
};
