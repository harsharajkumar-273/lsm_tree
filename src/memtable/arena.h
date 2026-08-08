#pragma once
#include <vector>
#include <mutex>
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <new>
#include <utility>
#include <stdexcept>

// Arena: simple thread-safe bump-pointer allocator for Lock-Free SkipList nodes.
// Memory is reclaimed entirely when the Arena (and Memtable) is destroyed.
class Arena {
public:
    explicit Arena(size_t block_size = 1024 * 1024)
        : block_size_(block_size), offset_(0), current_block_size_(block_size) {
        current_block_ = new char[block_size_];
        blocks_.push_back(current_block_);
    }

    ~Arena() {
        // Run registered destructors before releasing the storage they live in.
        //
        // The arena hands out raw bytes that callers placement-new into, and it
        // used to free the blocks without destroying anything in them. For
        // trivially destructible types that is fine; for anything owning heap
        // memory it is not, and SkipList::Node holds a std::string key and a
        // std::optional<std::string> value. Every node therefore leaked its
        // string allocations, whether or not the node was ever linked in.
        //
        // Reverse order mirrors ordinary destruction order.
        for (auto it = destructors_.rbegin(); it != destructors_.rend(); ++it) {
            it->second(it->first);
        }
        destructors_.clear();

        for (char* b : blocks_) delete[] b;
    }

    // Records an object to destroy when the arena is released.
    //
    // Deliberately takes a plain function pointer rather than std::function:
    // one is registered per object, and a captureless lambda converts to it
    // with no allocation of its own.
    void registerDestructor(void* object, void (*destroy)(void*)) {
        std::lock_guard<std::mutex> lock(mu_);
        destructors_.emplace_back(object, destroy);
    }

    // Returns `bytes` of storage aligned to `alignment`.
    //
    // The bump pointer previously advanced by the raw request size with no
    // rounding, so the address handed back was only ever correctly aligned by
    // accident of the sizes involved. Callers placing types with an alignment
    // requirement — Node holds std::atomic<Node*> — depended on every prior
    // allocation happening to be a multiple of that alignment.
    void* allocate(size_t bytes, size_t alignment = alignof(std::max_align_t)) {
        // padFor reduces modulo `alignment`, so zero would be a division by
        // zero, and a non-power-of-two would not describe a valid alignment.
        if (alignment == 0 || (alignment & (alignment - 1)) != 0) {
            throw std::invalid_argument("Arena::allocate: alignment must be a non-zero power of two");
        }
        // Sizing a fresh block computes `bytes + alignment`; refuse anything
        // that would wrap rather than allocating a block smaller than asked for.
        if (bytes > std::numeric_limits<size_t>::max() - alignment) {
            throw std::bad_alloc();
        }

        std::lock_guard<std::mutex> lock(mu_);

        size_t pad = padFor(current_block_ + offset_, alignment);

        // Written as subtractions because `offset_ + pad + bytes` can wrap, and
        // a wrapped sum compares as small enough to fit — which would advance
        // the bump pointer past the end of the block. offset_ never exceeds
        // current_block_size_, so neither subtraction underflows.
        const size_t remaining = current_block_size_ - offset_;
        if (pad > remaining || bytes > remaining - pad) {
            // A fresh block: size it for the payload plus worst-case padding.
            size_t next_size = std::max(block_size_, bytes + alignment);

            // Capacity is secured before the block exists.
            //
            // The order used to be `new` and then push_back. A push_back that
            // reallocates can throw, and if it did the block had already been
            // allocated but never recorded in blocks_, so ~Arena could not free
            // it. Worse than the leak, current_block_ had by then been
            // reassigned while current_block_size_ still described the previous
            // block — leaving the arena bounded by the wrong size if anything
            // caught the exception and carried on using it.
            //
            // Reserving first means a throw happens while nothing is owned yet,
            // and the subsequent push_back has guaranteed capacity so it cannot
            // reallocate. Member state is then updated only once every step
            // that can fail has already succeeded.
            blocks_.reserve(blocks_.size() + 1);

            char* fresh = new char[next_size];
            blocks_.push_back(fresh);

            // current_block_size_ tracks the block actually in hand. The old
            // code compared against block_size_ even after handing out an
            // oversized block, so the bound no longer described the block.
            current_block_ = fresh;
            current_block_size_ = next_size;
            offset_ = 0;
            pad = padFor(current_block_, alignment);
        }

        void* result = current_block_ + offset_ + pad;
        offset_ += pad + bytes;
        return result;
    }

private:
    static size_t padFor(const char* p, size_t alignment) {
        size_t rem = reinterpret_cast<uintptr_t>(p) % alignment;
        return rem ? alignment - rem : 0;
    }

    // Objects placement-new'd into this arena, in construction order.
    std::vector<std::pair<void*, void (*)(void*)>> destructors_;

    size_t block_size_;
    size_t offset_;
    size_t current_block_size_;
    char* current_block_;
    std::vector<char*> blocks_;
    std::mutex mu_; // protects all allocations
};
