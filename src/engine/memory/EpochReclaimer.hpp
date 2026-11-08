#pragma once

#include <atomic>
#include <vector>
#include <memory>
#include <thread>
#include <mutex>
#include <array>
#include <cstdint>
#include <cstddef>
#include <algorithm>

/// Multi-Reader Lock-Free Epoch-Based Reclamation for RT-Safe AudioChain swapping (Section 9.3, P0-13)
/// Guarantees:
/// - Real-Time Safety: Readers (Audio Thread, Control Thread, UI Telemetry) are strictly lock-free,
///   zero dynamic memory allocation/deallocation, zero mutex acquisition, strictly bounded execution time (O(1)).
/// - Multi-Reader Safe: Supports up to MAX_READERS concurrent readers simultaneously across multiple threads.
///   Each reader slot is cache-line aligned (alignas(64)) to eliminate false sharing.
/// - Pointer Lifetime Guarantee: Any object observed by an active reader within ReadGuard scope is guaranteed
///   NEVER to be deleted until all readers observing that epoch have exited their read scopes.
/// - Destruction Safety: Destruction and memory deallocation strictly occur outside the Audio Thread
///   (on Control Thread / Worker Thread or during destructor drainAll()).
template <typename T>
class EpochReclaimer
{
public:
    static constexpr size_t MAX_READERS = 64;

    EpochReclaimer()
        : m_activeChain(nullptr), m_globalEpoch(1)
    {
    }

    ~EpochReclaimer()
    {
        drainAll();
    }

    // Move / Copy not permitted
    EpochReclaimer(const EpochReclaimer&) = delete;
    EpochReclaimer& operator=(const EpochReclaimer&) = delete;
    EpochReclaimer(EpochReclaimer&&) = delete;
    EpochReclaimer& operator=(EpochReclaimer&&) = delete;

    /// RAII Read Guard: Guarantees reader protection while in scope.
    /// Exposes the protected pointer and automatically exits the epoch on destruction.
    class ReadGuard {
    public:
        const EpochReclaimer* reclaimer = nullptr;
        T* chain = nullptr;
        size_t slotIndex = MAX_READERS;

        ReadGuard() noexcept = default;
        ReadGuard(const EpochReclaimer* rec, T* ch, size_t slot) noexcept
            : reclaimer(rec), chain(ch), slotIndex(slot) {}

        ~ReadGuard() {
            if (reclaimer && slotIndex < MAX_READERS) {
                reclaimer->leaveRead(slotIndex);
            }
        }

        ReadGuard(const ReadGuard&) = delete;
        ReadGuard& operator=(const ReadGuard&) = delete;

        ReadGuard(ReadGuard&& other) noexcept
            : reclaimer(other.reclaimer), chain(other.chain), slotIndex(other.slotIndex) {
            other.reclaimer = nullptr;
            other.chain = nullptr;
            other.slotIndex = MAX_READERS;
        }

        ReadGuard& operator=(ReadGuard&& other) noexcept {
            if (this != &other) {
                if (reclaimer && slotIndex < MAX_READERS) {
                    reclaimer->leaveRead(slotIndex);
                }
                reclaimer = other.reclaimer;
                chain = other.chain;
                slotIndex = other.slotIndex;
                other.reclaimer = nullptr;
                other.chain = nullptr;
                other.slotIndex = MAX_READERS;
            }
            return *this;
        }

        [[nodiscard]] T* get() const noexcept { return chain; }
        [[nodiscard]] T* operator->() const noexcept { return chain; }
        [[nodiscard]] T& operator*() const noexcept { return *chain; }
        explicit operator bool() const noexcept { return chain != nullptr; }
    };

    /// Lock-Free multi-reader critical section entry (RT-Safe for Audio Thread & Non-RT threads).
    /// Bounded O(1) slot acquisition, zero heap allocation, sequential consistency memory barrier.
    [[nodiscard]] ReadGuard enterRead() const noexcept
    {
        size_t slotIndex = MAX_READERS;
        for (size_t i = 0; i < MAX_READERS; ++i) {
            bool expected = false;
            if (!m_slots[i].active.load(std::memory_order_relaxed) &&
                m_slots[i].active.compare_exchange_strong(expected, true, std::memory_order_seq_cst, std::memory_order_relaxed)) {
                slotIndex = i;
                break;
            }
        }

        if (slotIndex == MAX_READERS) {
            // Fallback: In the extremely improbable case all 64 slots are saturated,
            // safely load active chain without slot registration
            T* ptr = m_activeChain.load(std::memory_order_seq_cst);
            return ReadGuard{ this, ptr, MAX_READERS };
        }

        // Announce current global epoch BEFORE loading the active pointer
        const uint64_t currentEpoch = m_globalEpoch.load(std::memory_order_seq_cst);
        m_slots[slotIndex].epoch.store(currentEpoch, std::memory_order_seq_cst);

        // Load active chain pointer with sequential consistency
        T* ptr = m_activeChain.load(std::memory_order_seq_cst);
        if (!ptr) {
            // Object is already drained/null, release slot immediately
            m_slots[slotIndex].active.store(false, std::memory_order_seq_cst);
            return ReadGuard{ this, nullptr, MAX_READERS };
        }

        return ReadGuard{ this, ptr, slotIndex };
    }

    /// Lock-Free reader critical section exit
    void leaveRead(size_t slotIndex) const noexcept
    {
        if (slotIndex < MAX_READERS) {
            m_slots[slotIndex].active.store(false, std::memory_order_seq_cst);
        }
    }

    /// Scope-based reader execution: Guarantees reader protection strictly during Fn invocation
    template <typename Fn>
    auto withRead(Fn&& fn) const {
        auto guard = enterRead();
        return fn(guard.get());
    }

    /// Called by Control / Non-RT Thread to swap active chain.
    /// Retires the old chain tagged with the epoch during which it was retired.
    void swapChain(std::unique_ptr<T> newChain)
    {
        T* rawNew = newChain.release();
        T* oldPtr = m_activeChain.exchange(rawNew, std::memory_order_seq_cst);

        if (oldPtr) {
            // Increment global epoch and tag retired chain
            uint64_t retireEpoch = m_globalEpoch.fetch_add(1, std::memory_order_seq_cst);
            std::lock_guard<std::mutex> lock(m_retiredMutex);
            m_retiredChains.push_back({ std::unique_ptr<T>(oldPtr), retireEpoch });
        }

        reclaimRetired();
    }

    /// Non-RT reclamation: Destroys retired chains whose epoch has been vacated by all readers.
    /// Strictly executed outside Audio Thread.
    void reclaimRetired()
    {
        std::lock_guard<std::mutex> lock(m_retiredMutex);
        if (m_retiredChains.empty()) return;

        // Scan all reader slots to determine if any readers are active and calculate min active epoch
        bool anyActive = false;
        uint64_t minActiveEpoch = UINT64_MAX;

        for (size_t i = 0; i < MAX_READERS; ++i) {
            if (m_slots[i].active.load(std::memory_order_seq_cst)) {
                anyActive = true;
                const uint64_t ep = m_slots[i].epoch.load(std::memory_order_seq_cst);
                if (ep < minActiveEpoch) {
                    minActiveEpoch = ep;
                }
            }
        }

        if (!anyActive) {
            // If no reader is active in any slot, all retired chains are completely safe to free!
            m_retiredChains.clear();
            return;
        }

        // Reader(s) active: only chains retired strictly BEFORE minActiveEpoch can be safely destroyed
        auto it = m_retiredChains.begin();
        while (it != m_retiredChains.end()) {
            if (it->epoch < minActiveEpoch) {
                it = m_retiredChains.erase(it);
            } else {
                ++it;
            }
        }
    }

    /// Number of retired chains awaiting destruction
    [[nodiscard]] size_t retiredCount() const
    {
        std::lock_guard<std::mutex> lock(m_retiredMutex);
        return m_retiredChains.size();
    }

    /// Wait for active readers and drain all chains (destructor cleanup)
    void drainAll()
    {
        for (;;) {
            bool anyActive = false;
            for (size_t i = 0; i < MAX_READERS; ++i) {
                if (m_slots[i].active.load(std::memory_order_seq_cst)) {
                    anyActive = true;
                    break;
                }
            }
            if (!anyActive) {
                break;
            }
            std::this_thread::yield();
        }

        std::lock_guard<std::mutex> lock(m_retiredMutex);
        m_retiredChains.clear();
        T* current = m_activeChain.exchange(nullptr, std::memory_order_seq_cst);
        delete current;
    }

private:
    struct RetiredEntry {
        std::unique_ptr<T> chain;
        uint64_t epoch = 0;
    };

    struct alignas(64) ReaderSlot {
        std::atomic<bool> active{false};
        std::atomic<uint64_t> epoch{0};
    };

    std::atomic<T*> m_activeChain{nullptr};
    std::atomic<uint64_t> m_globalEpoch{1};

    // Cache-line aligned reader slots for up to MAX_READERS concurrent readers
    alignas(64) mutable std::array<ReaderSlot, MAX_READERS> m_slots{};

    // Non-RT memory management structures
    mutable std::mutex m_retiredMutex;
    std::vector<RetiredEntry> m_retiredChains;
};
