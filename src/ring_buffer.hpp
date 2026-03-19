#pragma once

#include <atomic>
#include <memory>
#include <cstdint>
#include <cstring>

// Lock-free Single-Producer Single-Consumer (SPSC) ring buffer
// Optimized for low latency with cache-line alignment
template<typename T, size_t Size>
class SPSCRingBuffer {
public:
    static_assert((Size & (Size - 1)) == 0, "Size must be a power of 2");

    SPSCRingBuffer() : head_(0), tail_(0) {
        // Initialize buffer
        buffer_ = std::make_unique<T[]>(Size);
    }

    // Non-copyable
    SPSCRingBuffer(const SPSCRingBuffer&) = delete;
    SPSCRingBuffer& operator=(const SPSCRingBuffer&) = delete;

    // Try to push an element (returns false if full)
    bool try_push(const T& item) {
        const size_t current_tail = tail_.load(std::memory_order_relaxed);
        const size_t next_tail = (current_tail + 1) & (Size - 1);

        // Check if buffer is full
        if (next_tail == head_.load(std::memory_order_acquire)) {
            return false;
        }

        buffer_[current_tail] = item;
        tail_.store(next_tail, std::memory_order_release);
        return true;
    }

    // Try to push (move version)
    bool try_push(T&& item) {
        const size_t current_tail = tail_.load(std::memory_order_relaxed);
        const size_t next_tail = (current_tail + 1) & (Size - 1);

        if (next_tail == head_.load(std::memory_order_acquire)) {
            return false;
        }

        buffer_[current_tail] = std::move(item);
        tail_.store(next_tail, std::memory_order_release);
        return true;
    }

    // Try to pop an element (returns false if empty)
    bool try_pop(T& item) {
        const size_t current_head = head_.load(std::memory_order_relaxed);

        // Check if buffer is empty
        if (current_head == tail_.load(std::memory_order_acquire)) {
            return false;
        }

        item = buffer_[current_head];
        head_.store((current_head + 1) & (Size - 1), std::memory_order_release);
        return true;
    }

    // Check if buffer is empty
    bool empty() const {
        return head_.load(std::memory_order_acquire) == tail_.load(std::memory_order_acquire);
    }

    // Check if buffer is full
    bool full() const {
        const size_t next_tail = (tail_.load(std::memory_order_acquire) + 1) & (Size - 1);
        return next_tail == head_.load(std::memory_order_acquire);
    }

    // Get current size (approximate - may not be exact due to concurrent access)
    size_t size() const {
        const size_t current_tail = tail_.load(std::memory_order_acquire);
        const size_t current_head = head_.load(std::memory_order_acquire);
        return (current_tail - current_head) & (Size - 1);
    }

    // Get capacity
    constexpr size_t capacity() const {
        return Size - 1;  // One slot is always unused to distinguish full from empty
    }

private:
    // Align to cache line (64 bytes) to prevent false sharing
    alignas(64) std::atomic<size_t> head_;  // Consumer index
    alignas(64) std::atomic<size_t> tail_;  // Producer index
    alignas(64) std::unique_ptr<T[]> buffer_;
};

// Blocking SPSC ring buffer with backpressure handling
template<typename T, size_t Size>
class BlockingSPSCRingBuffer {
public:
    static_assert((Size & (Size - 1)) == 0, "Size must be a power of 2");

    BlockingSPSCRingBuffer()
        : head_(0)
        , tail_(0)
        , dropped_count_(0) {
        buffer_ = std::make_unique<T[]>(Size);
    }

    // Push
    bool push(const T& item) {
        const size_t current_tail = tail_.load(std::memory_order_relaxed);
        const size_t next_tail = (current_tail + 1) & (Size - 1);

        // Check if buffer is full
        if (next_tail == head_.load(std::memory_order_acquire)) {
            dropped_count_.fetch_add(1, std::memory_order_relaxed);
            return false;
        }

        buffer_[current_tail] = item;
        tail_.store(next_tail, std::memory_order_release);
        return true;
    }

    // Push (move version)
    bool push(T&& item) {
        const size_t current_tail = tail_.load(std::memory_order_relaxed);
        const size_t next_tail = (current_tail + 1) & (Size - 1);

        if (next_tail == head_.load(std::memory_order_acquire)) {
            dropped_count_.fetch_add(1, std::memory_order_relaxed);
            return false;
        }

        buffer_[current_tail] = std::move(item);
        tail_.store(next_tail, std::memory_order_release);
        return true;
    }

    bool try_pop(T& item) {
        const size_t current_head = head_.load(std::memory_order_relaxed);

        if (current_head == tail_.load(std::memory_order_acquire)) {
            return false;
        }

        item = buffer_[current_head];
        head_.store((current_head + 1) & (Size - 1), std::memory_order_release);
        return true;
    }

    bool empty() const {
        return head_.load(std::memory_order_acquire) == tail_.load(std::memory_order_acquire);
    }

    size_t size() const {
        const size_t current_tail = tail_.load(std::memory_order_acquire);
        const size_t current_head = head_.load(std::memory_order_acquire);
        return (current_tail - current_head) & (Size - 1);
    }

    uint64_t get_dropped_count() const {
        return dropped_count_.load(std::memory_order_relaxed);
    }

    void reset_dropped_count() {
        dropped_count_.store(0, std::memory_order_relaxed);
    }

private:
    alignas(64) std::atomic<size_t> head_;
    alignas(64) std::atomic<size_t> tail_;
    alignas(64) std::unique_ptr<T[]> buffer_;
    alignas(64) std::atomic<uint64_t> dropped_count_;
};
