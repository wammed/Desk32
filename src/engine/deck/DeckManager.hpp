#pragma once

#include "ReferenceDeck.hpp"
#include <array>
#include <chrono>
#include <memory>
#include <thread>
#include <atomic>
#include <algorithm>

namespace desk32::engine::deck {

/// Section 2.3 Deck Switching 2-Mode System
enum class SwitchingMode {
    Normal,       ///< Section 2.3 Mode 1: Maintains each deck's transport state and position independently.
    Comparative,  ///< Section 2.3 Mode 2: Aligns target playhead to source playhead (+ offset), plays target, pauses source.

    // Semantic aliases:
    ParallelSolo = Normal,
    Exclusive = Comparative
};

struct SwitchResult {
    char sourceDeck;
    char targetDeck;
    SwitchingMode mode;
    double muteTimeMs;
    bool success;
    bool hadSeek = false;
    int64_t seekBeforeFrame = 0;
    double seekDurationMs = 0.0;
    decoder::SeekResult seekResult{};
};

/// 4-Deck Reference System Manager (Decks A, B, C, D)
/// Implements Section 2.1 - 2.3 & Exit Criteria.
class DeckManager {
public:
    explicit DeckManager(double graphRate = 48000.0);
    ~DeckManager();

    [[nodiscard]] double graphRate() const noexcept { return m_graphRate.load(std::memory_order_relaxed); }
    void setGraphRate(double rate) {
        m_graphRate.store(rate, std::memory_order_relaxed);
        for (auto& d : m_decks) {
            if (d) {
                d->configureGraphRate(rate);
            }
        }
    }

    [[nodiscard]] char activeDeckId() const noexcept { return m_activeDeckId.load(std::memory_order_relaxed); }
    [[nodiscard]] char previousDeckId() const noexcept { return m_previousDeckId.load(std::memory_order_relaxed); }

    ReferenceDeck* deck(char id) {
        size_t idx = static_cast<size_t>(id - 'A');
        return (idx < 4) ? m_decks[idx].get() : nullptr;
    }

    const ReferenceDeck* deck(char id) const {
        size_t idx = static_cast<size_t>(id - 'A');
        return (idx < 4) ? m_decks[idx].get() : nullptr;
    }

    ReferenceDeck* activeDeck() {
        return deck(m_activeDeckId.load(std::memory_order_relaxed));
    }

    const ReferenceDeck* activeDeck() const {
        return deck(m_activeDeckId.load(std::memory_order_relaxed));
    }

    /// Perform deck switching with mode selection (Section 2.3)
    SwitchResult switchDeck(char targetId, SwitchingMode mode) {
        char sourceId = m_activeDeckId.load(std::memory_order_relaxed);
        if (sourceId == targetId) {
            return {sourceId, targetId, mode, 0.0, true};
        }

        auto start = std::chrono::steady_clock::now();

        ReferenceDeck* src = deck(sourceId);
        ReferenceDeck* tgt = deck(targetId);

        if (!src || !tgt) {
            return {sourceId, targetId, mode, 0.0, false};
        }

        bool hadSeek = false;
        int64_t seekBeforeFrame = 0;
        double seekDurationMs = 0.0;
        decoder::SeekResult seekResult{};

        if (mode == SwitchingMode::Comparative) {
            // Comparative mode (Section 2.3 & 2.7.5):
            // Terminate any active Folder Session on comparative-switched decks while keeping loaded track context
            src->terminateFolderSession();
            tgt->terminateFolderSession();

            // Calculate equivalent time position from source deck
            double currentTimeSec = 0.0;
            if (src->trackInfo().sampleRate > 0) {
                currentTimeSec = static_cast<double>(src->playhead()) / src->trackInfo().sampleRate;
            }
            src->pause();

            // Align target playhead with offset
            int64_t targetFrame = static_cast<int64_t>(currentTimeSec * tgt->trackInfo().sampleRate) + tgt->offsetFrames();
            if (targetFrame < 0) targetFrame = 0;
            seekBeforeFrame = tgt->playhead();
            const auto seekStart = std::chrono::steady_clock::now();
            seekResult = tgt->seekWithResult(targetFrame);
            seekDurationMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - seekStart).count();
            hadSeek = true;
            tgt->play();
        } else {
            // Normal mode (Section 2.3 & §2.7.5):
            // Non-Folder-Session decks maintain each deck's independent transport state and position.
            // Folder Session decks pause and retain session state/position when deselected, and resume when reselected (§2.7.5).
            if (src->folderSession().isActive() && src->isPlaying()) {
                src->pause();
                src->setPausedByNormalSwitch(true);
            }
            if (tgt->folderSession().isActive() && tgt->wasPausedByNormalSwitch()) {
                tgt->setPausedByNormalSwitch(false);
                tgt->play();
            }
        }

        // Arm short 5 ms RT-safe de-click ramp on active deck transition (§2.3 / Plan §6)
        const double rate = m_graphRate.load(std::memory_order_relaxed);
        const size_t rampFrames = static_cast<size_t>(std::clamp(rate * 0.005, 32.0, 1024.0));
        m_switchRampTotalFrames.store(rampFrames, std::memory_order_release);
        m_switchRampRemainingFrames.store(rampFrames, std::memory_order_release);

        m_previousDeckId.store(sourceId, std::memory_order_relaxed);
        m_activeDeckId.store(targetId, std::memory_order_relaxed);

        auto end = std::chrono::steady_clock::now();
        double elapsedMs = std::chrono::duration<double, std::milli>(end - start).count();

        return {sourceId, targetId, mode, elapsedMs, true, hadSeek, seekBeforeFrame, seekDurationMs, seekResult};
    }

    /// Direct Mode renegotiation: re-negotiates graph rate directly with backend (Section 2.6)
    SwitchResult directModeSwitch(char targetId) {
        char sourceId = m_activeDeckId.load(std::memory_order_relaxed);
        auto start = std::chrono::steady_clock::now();

        ReferenceDeck* tgt = deck(targetId);
        if (!tgt) {
            return {sourceId, targetId, SwitchingMode::Normal, 0.0, false};
        }

        // Backend sample rate reconfiguration & resampler bypass for Direct Mode (P0-09)
        const double targetRate = tgt->trackInfo().sampleRate;
        if (targetRate > 0.0) {
            setGraphRate(targetRate);
        }

        tgt->play();
        m_previousDeckId.store(sourceId, std::memory_order_relaxed);
        m_activeDeckId.store(targetId, std::memory_order_relaxed);

        auto finish = std::chrono::steady_clock::now();
        double elapsedMs = std::chrono::duration<double, std::milli>(finish - start).count();

        return {sourceId, targetId, SwitchingMode::Normal, elapsedMs, true};
    }

    /// Quick toggle between Deck A and Deck B
    SwitchResult quickSwitchAB(SwitchingMode mode = SwitchingMode::Comparative) {
        char target = (m_activeDeckId.load(std::memory_order_relaxed) == 'A') ? 'B' : 'A';
        return switchDeck(target, mode);
    }

    /// Render planar audio block from the currently active deck.
    /// In Normal mode, active decks in background also advance frames to maintain synchronization.
    /// Uses pre-allocated member buffers to guarantee strictly ZERO dynamic memory allocation on the Audio Thread.
    size_t renderActiveBlock(double* outLeft, double* outRight, size_t frames) {
        ReferenceDeck* active = activeDeck();
        size_t rendered = 0;

        if (active) {
            rendered = active->renderBlock(outLeft, outRight, frames);
        } else {
            std::fill_n(outLeft, frames, 0.0);
            std::fill_n(outRight, frames, 0.0);
        }

        // Apply short RT-safe de-click ramp across active deck transition (0 dynamic allocations)
        size_t remRamp = m_switchRampRemainingFrames.load(std::memory_order_acquire);
        if (remRamp > 0 && frames > 0) {
            const size_t totalRamp = std::max<size_t>(1, m_switchRampTotalFrames.load(std::memory_order_acquire));
            const size_t toRamp = std::min(frames, remRamp);
            const size_t elapsedBefore = (totalRamp >= remRamp) ? (totalRamp - remRamp) : 0;
            for (size_t i = 0; i < toRamp; ++i) {
                const double gain = static_cast<double>(elapsedBefore + i + 1) / static_cast<double>(totalRamp);
                outLeft[i] *= gain;
                outRight[i] *= gain;
            }
            m_switchRampRemainingFrames.store(remRamp - toRamp, std::memory_order_release);
        }

        // Advance any playing background decks without heap allocation
        for (auto& d : m_decks) {
            if (d.get() != active && d->state() == TransportState::Playing) {
                size_t remaining = frames;
                while (remaining > 0) {
                    size_t chunk = std::min(remaining, size_t(4096));
                    d->renderBlock(m_discardL.data(), m_discardR.data(), chunk);
                    remaining -= chunk;
                }
            }
        }

        return rendered;
    }

    /// Backward-compatible interleaved rendering for Phase 0 benchmarks
    size_t renderBlock(double* outInterleaved, size_t frames) {
        ReferenceDeck* act = activeDeck();
        if (act) {
            return act->renderBlock(outInterleaved, frames);
        }
        std::fill_n(outInterleaved, frames * 2, 0.0);
        return 0;
    }

private:
    std::atomic<double> m_graphRate{48000.0};
    std::atomic<char> m_activeDeckId{'A'};
    std::atomic<char> m_previousDeckId{'B'};
    std::atomic<size_t> m_switchRampRemainingFrames{0};
    std::atomic<size_t> m_switchRampTotalFrames{240};
    std::array<std::unique_ptr<ReferenceDeck>, 4> m_decks;

    // Pre-allocated Audio Thread scratch buffers for background deck advancement (zero RT heap allocation)
    alignas(64) std::array<double, 4096> m_discardL{};
    alignas(64) std::array<double, 4096> m_discardR{};
};

} // namespace desk32::engine::deck
