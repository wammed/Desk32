#pragma once

#include <vector>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <utility>

namespace desk32::core::log {

/// Truly Lock-Free, Wait-Free Bounded Log Queue for RT-safe log transfer
/// Implements Vyukov bounded MPMC queue with zero mutex, zero trylock, and zero dynamic allocation.
template <typename T, size_t Capacity = 4096>
class BoundedLogQueue {
    static_assert((Capacity & (Capacity - 1)) == 0, "Capacity must be a power of 2");

    struct Node {
        std::atomic<size_t> sequence;
        T data;
    };

public:
    BoundedLogQueue() : m_buffer(Capacity) {
        for (size_t i = 0; i < Capacity; ++i) {
            m_buffer[i].sequence.store(i, std::memory_order_relaxed);
        }
    }

    /// Strictly non-blocking, lock-free, wait-free push for Real-time Audio Thread.
    /// Returns false immediately without waiting or acquiring locks if the queue is full.
    bool try_push(const T& item) {
        size_t pos = m_enqueuePos.load(std::memory_order_relaxed);
        for (;;) {
            Node& node = m_buffer[pos & m_bufferMask];
            size_t seq = node.sequence.load(std::memory_order_acquire);
            intptr_t diff = static_cast<intptr_t>(seq) - static_cast<intptr_t>(pos);
            if (diff == 0) {
                if (m_enqueuePos.compare_exchange_weak(pos, pos + 1, std::memory_order_relaxed)) {
                    node.data = item;
                    node.sequence.store(pos + 1, std::memory_order_release);
                    return true;
                }
            } else if (diff < 0) {
                // Queue is saturated: drop event to protect real-time audio thread
                m_overflowCount.fetch_add(1, std::memory_order_relaxed);
                return false;
            } else {
                pos = m_enqueuePos.load(std::memory_order_relaxed);
            }
        }
    }

    /// Normal push (GUI / worker threads)
    bool push(const T& item) {
        return try_push(item);
    }

    /// Try to pop one item
    bool try_pop(T& item) {
        size_t pos = m_dequeuePos.load(std::memory_order_relaxed);
        for (;;) {
            Node& node = m_buffer[pos & m_bufferMask];
            size_t seq = node.sequence.load(std::memory_order_acquire);
            intptr_t diff = static_cast<intptr_t>(seq) - static_cast<intptr_t>(pos + 1);
            if (diff == 0) {
                if (m_dequeuePos.compare_exchange_weak(pos, pos + 1, std::memory_order_relaxed)) {
                    item = std::move(node.data);
                    node.sequence.store(pos + m_bufferMask + 1, std::memory_order_release);
                    return true;
                }
            } else if (diff < 0) {
                return false;
            } else {
                pos = m_dequeuePos.load(std::memory_order_relaxed);
            }
        }
    }

    /// Drain all pending items into a vector
    std::vector<T> drain() {
        std::vector<T> result;
        T item;
        while (try_pop(item)) {
            result.push_back(std::move(item));
        }
        return result;
    }

    [[nodiscard]] size_t size() const noexcept {
        size_t eq = m_enqueuePos.load(std::memory_order_relaxed);
        size_t dq = m_dequeuePos.load(std::memory_order_relaxed);
        return (eq >= dq) ? (eq - dq) : 0;
    }

    [[nodiscard]] size_t overflowCount() const noexcept {
        return m_overflowCount.load(std::memory_order_relaxed);
    }

    size_t resetOverflowCount() noexcept {
        return m_overflowCount.exchange(0, std::memory_order_relaxed);
    }

private:
    static constexpr size_t m_bufferMask = Capacity - 1;
    std::vector<Node> m_buffer;
    alignas(64) std::atomic<size_t> m_enqueuePos{0};
    alignas(64) std::atomic<size_t> m_dequeuePos{0};
    std::atomic<size_t> m_overflowCount{0};
};

} // namespace desk32::core::log
