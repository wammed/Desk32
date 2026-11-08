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

/// Multi-Reader Lock-Free Epoch-Based Reclamation for RT-Safe AudioChain swapping (Section 9.3, P0-13, P0-20)
/// Guarantees:
/// - Real-Time Safety: Readers (Audio Thread, Control Thread, UI Telemetry) are strictly lock-free,
///   zero dynamic memory allocation/deallocation, zero mutex acquisition, strictly bounded execution time (O(1)).
/// - Multi-Reader Safe: Supports up to MAX_READERS concurrent readers simultaneously across multiple threads.
///   Each reader slot is cache-line aligned (alignas(64)) to eliminate false sharing.
/// - Pointer Lifetime Guarantee: Any object observed by an active reader within ReadGuard scope is guaranteed
///   NEVER to be deleted until all readers observing that epoch have exited their read scopes.
/// - Slot Exhaustion Safety (P0-20): If reader slots are exhausted, enterRead() strictly returns an invalid ReadGuard
///   with nullptr. Under NO circumstance is an unshielded / unprotected pointer ever returned.
/// - Reader Slot Dimensioning (Section 3.3, P0-20):
///   - MAX_READERS = 64 slots.
///   - Concurrency Analysis for Desk32:
///       * Audio Thread: 1 active reader during playback render block.
///       * Offline Render Thread: 1 active reader during export render block.
///       * Control Thread: 1 active reader during sample rate reconfigure / Graph swap.
///       * UI / Telemetry Thread: 1~2 active readers during background polling.
///   - Total maximum simultaneous reader threads in Desk32 is <= 6.
///   - Even allowing nested guards (up to 2 levels), maximum simultaneously needed slots <= 12.
///   - MAX_READERS = 64 provides over 5x-10x headroom without dynamic allocation.
///   - Slot Allocation: O(1) lock-free compare-and-swap loop over 64 slots (< 50 ns).
///   - Slot Release: RAII ReadGuard destructor atomically marks slot inactive.
///   - Slot Exhaustion: Returns invalid guard; atomic counter m_slotExhaustionCount increments.
///   - Destruction Safety: Destruction strictly occurs outside Audio Thread (Control / Worker Thread).
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

        [[nodiscard]] bool isValid() const noexcept {
            return chain != nullptr && slotIndex < MAX_READERS;
        }
        explicit operator bool() const noexcept { return isValid(); }
        [[nodiscard]] T* get() const noexcept { return isValid() ? chain : nullptr; }
        [[nodiscard]] T* operator->() const noexcept { return isValid() ? chain : nullptr; }
        [[nodiscard]] T& operator*() const noexcept { return *chain; }
    };

    /// Lock-Free multi-reader critical section entry (RT-Safe for Audio Thread & Non-RT threads).
    /// Bounded O(1) slot acquisition, zero heap allocation, sequential consistency memory barrier.
    /// P0-20, P0-28: If closing or all slots are saturated, NEVER returns an unshielded pointer.
    [[nodiscard]] ReadGuard enterRead() const noexcept
    {
        if (m_closing.load(std::memory_order_seq_cst)) {
            return ReadGuard{ this, nullptr, MAX_READERS };
        }

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
            // P0-20: Reader slots exhausted. NEVER expose unshielded active pointer!
            m_slotExhaustionCount.fetch_add(1, std::memory_order_relaxed);
            return ReadGuard{ this, nullptr, MAX_READERS };
        }

        // P0-28: Prohibit race between reader registration and shutdown drainAll()
        if (m_closing.load(std::memory_order_seq_cst)) {
            m_slots[slotIndex].active.store(false, std::memory_order_seq_cst);
            return ReadGuard{ this, nullptr, MAX_READERS };
        }

        // Announce current global epoch BEFORE loading the active pointer
        const uint64_t currentEpoch = m_globalEpoch.load(std::memory_order_seq_cst);
        m_slots[slotIndex].epoch.store(currentEpoch, std::memory_order_seq_cst);

        // Load active chain pointer with sequential consistency
        T* ptr = m_activeChain.load(std::memory_order_seq_cst);
        if (!ptr || m_closing.load(std::memory_order_seq_cst)) {
            // Object is already drained/null or reclaimer is closing, release slot immediately
            m_slots[slotIndex].active.store(false, std::memory_order_seq_cst);
            return ReadGuard{ this, nullptr, MAX_READERS };
        }

        return ReadGuard{ this, ptr, slotIndex };
    }

    /// Number of times enterRead() failed to acquire a reader slot (diagnostic counter)
    [[nodiscard]] uint64_t slotExhaustionCount() const noexcept {
        return m_slotExhaustionCount.load(std::memory_order_relaxed);
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

    /// Called by Control / Non-RT Thread to swap active chain (P0-20).
    /// Serialized with drainAll() via m_writerMutex to guarantee atomic lifecycle management.
    /// Retires the old chain tagged with the epoch during which it was retired.
    /// Returns true if swapped successfully, false if rejected due to shutdown.
    bool swapChain(std::unique_ptr<T> newChain)
    {
        if (!newChain) {
            return false;
        }

        std::lock_guard<std::mutex> writerLock(m_writerMutex);
        if (m_closing.load(std::memory_order_seq_cst)) {
            // P0-20: If closing or already drained, strictly reject new chain registration.
            // newChain is safely deleted via unique_ptr without leaking or corrupting state.
            return false;
        }

        T* rawNew = newChain.release();
        T* oldPtr = m_activeChain.exchange(rawNew, std::memory_order_seq_cst);

        if (oldPtr) {
            // Increment global epoch and tag retired chain
            uint64_t retireEpoch = m_globalEpoch.fetch_add(1, std::memory_order_seq_cst);
            std::lock_guard<std::mutex> lock(m_retiredMutex);
            m_retiredChains.push_back({ std::unique_ptr<T>(oldPtr), retireEpoch });
        }

        reclaimRetired();
        return true;
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

    [[nodiscard]] bool isClosing() const noexcept {
        return m_closing.load(std::memory_order_seq_cst);
    }

    [[nodiscard]] bool isWaitingForReaders() const noexcept {
        return m_waitingForReaders.load(std::memory_order_acquire);
    }

    /// Wait for active readers and drain all chains (destructor cleanup, P0-20, P0-28).
    /// Serialized with swapChain() via m_writerMutex to eliminate writer-shutdown race conditions.
    void drainAll()
    {
        std::lock_guard<std::mutex> writerLock(m_writerMutex);

        // 1. Prohibit new reader registrations
        m_closing.store(true, std::memory_order_seq_cst);

        // 2. Clear active chain pointer immediately so speculative readers see null
        T* current = m_activeChain.exchange(nullptr, std::memory_order_seq_cst);

        // 3. Wait for all existing active readers to exit
        m_waitingForReaders.store(true, std::memory_order_release);
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
        m_waitingForReaders.store(false, std::memory_order_release);

        // 4. Safely free retired chains and current active chain outside Audio Thread
        std::lock_guard<std::mutex> lock(m_retiredMutex);
        m_retiredChains.clear();
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

    alignas(64) std::atomic<bool> m_closing{false};
    alignas(64) std::atomic<bool> m_waitingForReaders{false};
    std::atomic<T*> m_activeChain{nullptr};
    std::atomic<uint64_t> m_globalEpoch{1};
    mutable std::atomic<uint64_t> m_slotExhaustionCount{0};

    // Cache-line aligned reader slots for up to MAX_READERS concurrent readers
    alignas(64) mutable std::array<ReaderSlot, MAX_READERS> m_slots{};

    // Non-RT memory management structures
    mutable std::mutex m_writerMutex;
    mutable std::mutex m_retiredMutex;
    std::vector<RetiredEntry> m_retiredChains;
};
