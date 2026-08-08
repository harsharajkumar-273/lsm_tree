#pragma once
#include "arena.h"
#include <atomic>
#include <string>
#include <optional>
#include <vector>
#include <random>
#include <cstring>
#include <mutex>

// Immutable value cell.
//
// An update publishes a new cell and swaps a pointer rather than assigning over
// the old cell. Assigning a std::optional<std::string> releases the old heap
// buffer and allocates a new one, and readers hold no lock while touching that
// field — so a reader could be walking a buffer the writer had already freed.
// A published cell is never written again, so a reader that has loaded the
// pointer keeps a stable view of it.
//
// Superseded cells stay resident until the Arena is released. Their destructors
// do run: Node::create registers each one, the same way it registers nodes. A
// Memtable is flushed and discarded whole, so the cells go with it — which is
// what lets readers proceed without hazard pointers or epoch reclamation.
struct ValueSlot {
    std::optional<std::string> v;
    explicit ValueSlot(const std::optional<std::string>& value) : v(value) {}
};

// Lock-Free SkipList Node
struct Node {
    std::string key;
    std::atomic<const ValueSlot*> value;
    int height;

    // Points at `height` link slots carved from the same arena allocation that
    // holds the Node.
    //
    // This was declared `std::atomic<Node*> next[1]` and over-allocated. Two
    // things were wrong with that. Indexing next[i] for i >= 1 runs off the end
    // of a one-element array, and — the part that bites in practice — placement
    // new of a Node only ever constructs next[0]; the slots beyond it stayed
    // raw arena bytes, so every .store() and .load() above level 0 ran against
    // an object that was never constructed. Holding a pointer to an array whose
    // elements are each explicitly constructed removes both problems, and every
    // `next[i]` use site reads exactly as before.
    std::atomic<Node*>* next;

    static Node* create(Arena& arena, const std::string& k, const std::optional<std::string>& v, int h) {
        static_assert(alignof(Node) >= alignof(std::atomic<Node*>),
                      "link slots trail the Node and inherit its alignment");

        constexpr size_t kLinkAlign = alignof(std::atomic<Node*>);
        const size_t node_bytes = sizeof(Node);
        const size_t pad = (node_bytes % kLinkAlign) ? kLinkAlign - (node_bytes % kLinkAlign) : 0;
        const size_t total = node_bytes + pad + sizeof(std::atomic<Node*>) * static_cast<size_t>(h);

        char* mem = static_cast<char*>(arena.allocate(total, alignof(Node)));
        Node* n = new (mem) Node();

        auto* links = reinterpret_cast<std::atomic<Node*>*>(mem + node_bytes + pad);
        for (int i = 0; i < h; ++i) {
            ::new (static_cast<void*>(links + i)) std::atomic<Node*>(nullptr);
        }

        n->next   = links;
        n->key    = k;
        n->height = h;
        // Relaxed: the node is private to this thread until the release CAS in
        // insert() publishes it.
        n->value.store(makeSlot(arena, v), std::memory_order_relaxed);

        // Registered once the node is fully constructed, and registered for
        // every node rather than only the ones that get linked. A node whose
        // level-0 CAS loses the race is abandoned where it lies — the retry
        // allocates a fresh one — so tying cleanup to reachability would leave
        // exactly those behind. The arena owns the storage, so it owns the
        // teardown.
        arena.registerDestructor(n, [](void* p) { static_cast<Node*>(p)->~Node(); });

        return n;
    }

    static const ValueSlot* makeSlot(Arena& arena, const std::optional<std::string>& v) {
        void* mem = arena.allocate(sizeof(ValueSlot), alignof(ValueSlot));
        auto* slot = ::new (mem) ValueSlot(v);
        arena.registerDestructor(slot, [](void* p) { static_cast<ValueSlot*>(p)->~ValueSlot(); });
        return slot;
    }
};

// Simple Lock-Free SkipList (Insert-only for Memtable usage)
class SkipList {
public:
    static constexpr int MAX_HEIGHT = 12;

    explicit SkipList(Arena& arena) : arena_(arena), head_(Node::create(arena_, "", std::nullopt, MAX_HEIGHT)) {}

    // Outcome of an insert, so the caller can account for it correctly.
    //
    // Memtable tracks a byte total and an entry count, and previously had no
    // way to distinguish a new key from an overwrite — insert() returned void,
    // so every call was charged as if it added an entry.
    struct InsertResult {
        bool   inserted;             // true when a new key was linked in
        size_t replacedValueBytes;   // bytes held by the value this call displaced
    };

    InsertResult insert(const std::string& key, const std::optional<std::string>& value) {
        Node* preds[MAX_HEIGHT];
        Node* succs[MAX_HEIGHT];
        
        while (true) {
            if (find(key, preds, succs)) {
                // Key already exists — update the value in-place.
                // This is safe for MemTable usage: the engine is single-writer per key
                // in practice, and the memtable is swapped atomically on flush.
                // A direct store avoids inserting duplicate nodes, which would cause
                // find() to return stale data (the first/oldest match).
                const ValueSlot* previous = succs[0]->value.load(std::memory_order_acquire);
                const size_t replaced = (previous && previous->v) ? previous->v->size() : 0;

                // Swap an immutable cell instead of assigning over a live
                // std::optional<std::string>, so a concurrent reader can never
                // observe the field mid-reallocation. Still one store rather
                // than a second node, so find() keeps returning a single match.
                succs[0]->value.store(Node::makeSlot(arena_, value), std::memory_order_release);
                return { false, replaced };
            }

            int height = randomHeight();
            Node* newNode = Node::create(arena_, key, value, height);

            for (int i = 0; i < height; ++i) {
                newNode->next[i].store(succs[i], std::memory_order_relaxed);
            }

            // Lock-free insertion at level 0
            if (preds[0]->next[0].compare_exchange_strong(succs[0], newNode)) {
                // Successfully inserted at level 0. Now link higher levels.
                for (int i = 1; i < height; ++i) {
                    while (true) {
                        Node* pred = preds[i];
                        Node* succ = succs[i];
                        newNode->next[i].store(succ, std::memory_order_relaxed);
                        if (pred->next[i].compare_exchange_strong(succ, newNode)) break;
                        // If CAS fails, we need to re-find neighbors for this level
                        find(key, preds, succs);
                    }
                }
                return { true, 0 };
            }
            // If level 0 CAS fails, someone else inserted. Retry entire operation.
        }
    }

    bool find(const std::string& key, Node** preds, Node** succs) const {
        Node* x = head_;
        for (int i = MAX_HEIGHT - 1; i >= 0; --i) {
            Node* next = x->next[i].load(std::memory_order_acquire);
            while (next != nullptr && next->key < key) {
                x = next;
                next = x->next[i].load(std::memory_order_acquire);
            }
            preds[i] = x;
            succs[i] = next;
        }
        return (succs[0] != nullptr && succs[0]->key == key);
    }

    Node* getHead() const { return head_; }

private:
    Arena& arena_;
    Node* head_;

    int randomHeight() {
        // Thread-safe: each thread gets its own RNG instance
        thread_local static std::mt19937 rng{std::random_device{}()};
        int h = 1;
        while (h < MAX_HEIGHT && (rng() % 4 == 0)) h++;
        return h;
    }
};

class Memtable {
public:
    static constexpr size_t DEFAULT_MAX_BYTES = 4 * 1024 * 1024; // 4 MiB

    explicit Memtable(size_t max_bytes = DEFAULT_MAX_BYTES)
        : max_bytes_(max_bytes), current_bytes_(0), list_(arena_) {}

    void put(const std::string& key, const std::string& value) {
        const auto result = list_.insert(key, value);

        if (result.inserted) {
            current_bytes_.fetch_add(key.size() + value.size() + 8, std::memory_order_relaxed);
            count_.fetch_add(1, std::memory_order_relaxed);
            return;
        }

        // An overwrite adds no key and no entry. Only the value changed, so the
        // byte total moves by the difference between the new value and the one
        // it displaced. Charging the full key+value again — as this did before —
        // inflated the total without bound under repeated updates to one key,
        // driving isFull() true and forcing flushes the data did not warrant.
        adjustValueBytes(result.replacedValueBytes, value.size());
    }

    void del(const std::string& key) {
        const auto result = list_.insert(key, std::nullopt);

        if (result.inserted) {
            // A tombstone for a key not present still occupies a node.
            current_bytes_.fetch_add(key.size() + 8, std::memory_order_relaxed);
            count_.fetch_add(1, std::memory_order_relaxed);
            return;
        }

        // Tombstoning an existing key keeps the node and releases the value.
        adjustValueBytes(result.replacedValueBytes, 0);
    }

    std::optional<std::optional<std::string>> get(const std::string& key) const {
        Node* preds[SkipList::MAX_HEIGHT];
        Node* succs[SkipList::MAX_HEIGHT];
        if (list_.find(key, preds, succs)) {
            // Acquire pairs with the release store in insert(), so the cell's
            // contents are visible once its pointer is.
            return succs[0]->value.load(std::memory_order_acquire)->v;
        }
        return std::nullopt;
    }

    bool isFull() const { return current_bytes_.load(std::memory_order_relaxed) >= max_bytes_; }

    // Applies the signed difference between two value sizes to the byte total.
    // Split into add and subtract so neither operand is computed by subtracting
    // a larger size_t from a smaller one.
    void adjustValueBytes(size_t oldBytes, size_t newBytes) {
        if (newBytes >= oldBytes) {
            current_bytes_.fetch_add(newBytes - oldBytes, std::memory_order_relaxed);
        } else {
            current_bytes_.fetch_sub(oldBytes - newBytes, std::memory_order_relaxed);
        }
    }
    size_t size() const { return count_.load(std::memory_order_relaxed); }
    size_t byteSize() const { return current_bytes_.load(std::memory_order_relaxed); }
    bool empty() const { return size() == 0; }

    // Iterator for flushing (sequential)
    class Iterator {
    public:
        using value_type = std::pair<std::string, std::optional<std::string>>;
        explicit Iterator(Node* n) : current_(n) { updateCache(); }
        bool operator!=(const Iterator& other) const { return current_ != other.current_; }
        void operator++() { 
            if (current_) current_ = current_->next[0].load(std::memory_order_acquire);
            updateCache();
        }
        const value_type& operator*() const { return cache_; }
        const value_type* operator->() const { return &cache_; }
    private:
        Node* current_;
        value_type cache_;
        void updateCache() {
            if (current_) {
                cache_ = {current_->key, current_->value.load(std::memory_order_acquire)->v};
            }
        }
    };

    Iterator begin() const { return Iterator(list_.getHead()->next[0].load(std::memory_order_acquire)); }
    Iterator end() const { return Iterator(nullptr); }

private:
    size_t max_bytes_;
    std::atomic<size_t> current_bytes_{0};
    std::atomic<size_t> count_{0};
    Arena arena_;
    SkipList list_;
};
