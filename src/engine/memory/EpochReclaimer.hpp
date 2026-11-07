#pragma once

#include <atomic>
#include <vector>
#include <memory>
#include <thread>
#include <mutex>
#include <cstdint>

/// Lock-Free Epoch-Based Reclamation for RT-Safe AudioChain swapping (Section 9.3)
/// Guarantees:
/// - Audio Thread (Reader): strictly lock-free, zero heap allocation, bounded execution time (O(1)).
/// - Control Thread (Writer): swaps active chain via atomic exchange, tags retired chain with current global epoch,
///   and safely destroys old chains only after all active readers have completed reading or advanced to a newer epoch.
template <typename T>
class EpochReclaimer
{
public:
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

    /// RT-Safe: Called by Audio Thread to enter reading epoch and get current chain
    struct ReadGuard {
        EpochReclaimer* reclaimer = nullptr;
        T* chain = nullptr;

        ReadGuard() = default;
        ReadGuard(EpochReclaimer* rec, T* ch) noexcept : reclaimer(rec), chain(ch) {}

        ~ReadGuard() {
            if (reclaimer) {
                reclaimer->leaveRead();
            }
        }

        ReadGuard(const ReadGuard&) = delete;
        ReadGuard& operator=(const ReadGuard&) = delete;

        ReadGuard(ReadGuard&& other) noexcept
            : reclaimer(other.reclaimer), chain(other.chain) {
            other.reclaimer = nullptr;
            other.chain = nullptr;
        }

        ReadGuard& operator=(ReadGuard&& other) noexcept {
            if (this != &other) {
                if (reclaimer) reclaimer->leaveRead();
                reclaimer = other.reclaimer;
                chain = other.chain;
                other.reclaimer = nullptr;
                other.chain = nullptr;
            }
            return *this;
        }
    };

    /// Lock-Free Audio Thread critical section entry
    [[nodiscard]] ReadGuard enterRead() noexcept
    {
        // 1. Announce entry with current global epoch
        const uint64_t currentEpoch = m_globalEpoch.load(std::memory_order_acquire);
        m_readerEpoch.store(currentEpoch, std::memory_order_release);
        m_readerActive.store(true, std::memory_order_seq_cst);

        // 2. Load pointer to currently active chain
        T* ptr = m_activeChain.load(std::memory_order_acquire);
        return ReadGuard{ this, ptr };
    }

    /// Lock-Free Audio Thread critical section exit
    void leaveRead() noexcept
    {
        m_readerActive.store(false, std::memory_order_seq_cst);
    }

    /// Read currently active chain pointer outside Audio Thread (Control Thread)
    [[nodiscard]] T* current() const noexcept
    {
        return m_activeChain.load(std::memory_order_acquire);
    }

    /// Called by Control / Non-RT Thread to swap active chain.
    /// Retires the old chain tagged with the epoch during which it was retired.
    void swapChain(std::unique_ptr<T> newChain)
    {
        T* rawNew = newChain.release();
        T* oldPtr = m_activeChain.exchange(rawNew, std::memory_order_acq_rel);

        if (oldPtr) {
            // Increment global epoch and tag retired chain
            uint64_t retireEpoch = m_globalEpoch.fetch_add(1, std::memory_order_acq_rel);
            std::lock_guard<std::mutex> lock(m_retiredMutex);
            m_retiredChains.push_back({ std::unique_ptr<T>(oldPtr), retireEpoch });
        }

        reclaimRetired();
    }

    /// Non-RT reclamation: Destroys retired chains whose epoch has been vacated by all readers.
    void reclaimRetired()
    {
        std::lock_guard<std::mutex> lock(m_retiredMutex);
        if (m_retiredChains.empty()) return;

        const bool readerActive = m_readerActive.load(std::memory_order_seq_cst);
        const uint64_t readerEpoch = m_readerEpoch.load(std::memory_order_seq_cst);

        if (!readerActive) {
            // If reader is not in a critical section, all retired chains are completely safe to free!
            m_retiredChains.clear();
            return;
        }

        // Reader is active: only chains retired strictly BEFORE readerEpoch can be safely destroyed
        // (because a reader entering at readerEpoch could never observe a chain retired at >= readerEpoch)
        auto it = m_retiredChains.begin();
        while (it != m_retiredChains.end()) {
            if (it->epoch < readerEpoch) {
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
        while (m_readerActive.load(std::memory_order_seq_cst)) {
            std::this_thread::yield();
        }
        std::lock_guard<std::mutex> lock(m_retiredMutex);
        m_retiredChains.clear();
        T* current = m_activeChain.exchange(nullptr, std::memory_order_acq_rel);
        delete current;
    }

private:
    struct RetiredEntry {
        std::unique_ptr<T> chain;
        uint64_t epoch = 0;
    };

    std::atomic<T*> m_activeChain{nullptr};
    std::atomic<uint64_t> m_globalEpoch{1};

    // Single RT reader tracking (Audio Thread)
    std::atomic<bool> m_readerActive{false};
    std::atomic<uint64_t> m_readerEpoch{0};

    // Non-RT memory management structures
    mutable std::mutex m_retiredMutex;
    std::vector<RetiredEntry> m_retiredChains;
};
