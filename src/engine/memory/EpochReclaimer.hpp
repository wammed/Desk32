#pragma once

#include <atomic>
#include <vector>
#include <memory>
#include <thread>

// Lock-Free Epoch-Based Reclamation for RT-Safe AudioChain swapping (Section 9.3)
template <typename T>
class EpochReclaimer
{
public:
    EpochReclaimer()
        : m_activeChain(nullptr), m_globalEpoch(0)
    {
    }

    ~EpochReclaimer()
    {
        drainAll();
    }

    // RT-Safe: Called by Audio Thread to enter reading epoch and get current chain
    struct ReadGuard {
        EpochReclaimer* reclaimer;
        T* chain;

        ~ReadGuard() {
            if (reclaimer) {
                reclaimer->leaveRead();
            }
        }
    };

    ReadGuard enterRead()
    {
        m_activeReaders.fetch_add(1, std::memory_order_acquire);
        T* ptr = m_activeChain.load(std::memory_order_consume);
        return ReadGuard{ this, ptr };
    }

    void leaveRead()
    {
        m_activeReaders.fetch_sub(1, std::memory_order_release);
    }

    // Called by Control Thread (NON-RT) to swap chain and queue old chain for delayed deletion
    void swapChain(std::unique_ptr<T> newChain)
    {
        T* rawNew = newChain.release();
        T* oldPtr = m_activeChain.exchange(rawNew, std::memory_order_acq_rel);

        if (oldPtr) {
            uint64_t epoch = m_globalEpoch.fetch_add(1, std::memory_order_relaxed);
            m_retiredChains.push_back({ std::unique_ptr<T>(oldPtr), epoch });
        }

        reclaimRetired();
    }

    // Called periodically by Control / Worker thread (NON-RT)
    void reclaimRetired()
    {
        if (m_activeReaders.load(std::memory_order_acquire) == 0) {
            // Safe to destroy retired chains outside Audio Thread
            m_retiredChains.clear();
        }
    }

    void drainAll()
    {
        while (m_activeReaders.load(std::memory_order_acquire) > 0) {
            std::this_thread::yield();
        }
        m_retiredChains.clear();
        T* current = m_activeChain.exchange(nullptr);
        delete current;
    }

private:
    struct RetiredEntry {
        std::unique_ptr<T> chain;
        uint64_t epoch;
    };

    std::atomic<T*> m_activeChain;
    std::atomic<uint64_t> m_globalEpoch;
    std::atomic<int> m_activeReaders{0};
    std::vector<RetiredEntry> m_retiredChains;
};

