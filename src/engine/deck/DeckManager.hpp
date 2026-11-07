#pragma once

#include "ReferenceDeck.hpp"
#include <array>
#include <chrono>
#include <memory>
#include <thread>

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
};

/// 4-Deck Reference System Manager (Decks A, B, C, D)
/// Implements Section 2.1 - 2.3 & Exit Criteria.
class DeckManager {
public:
    explicit DeckManager(double graphRate = 48000.0)
        : m_graphRate(graphRate)
    {
        for (size_t i = 0; i < 4; ++i) {
            char id = static_cast<char>('A' + i);
            m_decks[i] = std::make_unique<ReferenceDeck>(id);
        }
    }

    [[nodiscard]] double graphRate() const noexcept { return m_graphRate; }
    void setGraphRate(double rate) {
        m_graphRate = rate;
    }

    [[nodiscard]] char activeDeckId() const noexcept { return m_activeDeckId; }
    [[nodiscard]] char previousDeckId() const noexcept { return m_previousDeckId; }

    ReferenceDeck* deck(char id) {
        size_t idx = static_cast<size_t>(id - 'A');
        return (idx < 4) ? m_decks[idx].get() : nullptr;
    }

    const ReferenceDeck* deck(char id) const {
        size_t idx = static_cast<size_t>(id - 'A');
        return (idx < 4) ? m_decks[idx].get() : nullptr;
    }

    ReferenceDeck* activeDeck() {
        return deck(m_activeDeckId);
    }

    const ReferenceDeck* activeDeck() const {
        return deck(m_activeDeckId);
    }

    /// Perform deck switching with mode selection (Section 2.3)
    SwitchResult switchDeck(char targetId, SwitchingMode mode) {
        char sourceId = m_activeDeckId;
        if (sourceId == targetId) {
            return {sourceId, targetId, mode, 0.0, true};
        }

        auto start = std::chrono::steady_clock::now();

        ReferenceDeck* src = deck(sourceId);
        ReferenceDeck* tgt = deck(targetId);

        if (!src || !tgt) {
            return {sourceId, targetId, mode, 0.0, false};
        }

        if (mode == SwitchingMode::Comparative) {
            // Comparative mode (Section 2.3):
            // Calculate equivalent time position from source deck
            double currentTimeSec = 0.0;
            if (src->trackInfo().sampleRate > 0) {
                currentTimeSec = static_cast<double>(src->playhead()) / src->trackInfo().sampleRate;
            }
            src->pause();

            // Align target playhead with offset
            int64_t targetFrame = static_cast<int64_t>(currentTimeSec * tgt->trackInfo().sampleRate) + tgt->offsetFrames();
            if (targetFrame < 0) targetFrame = 0;
            tgt->seek(targetFrame);
            tgt->play();
        } else {
            // Normal mode (Section 2.3):
            // Maintains independent transport state and position.
            tgt->play();
        }

        m_previousDeckId = sourceId;
        m_activeDeckId = targetId;

        auto end = std::chrono::steady_clock::now();
        double elapsedMs = std::chrono::duration<double, std::milli>(end - start).count();

        return {sourceId, targetId, mode, elapsedMs, true};
    }

    /// Direct Mode renegotiation simulation: re-negotiates graph rate directly with backend (Section 2.6)
    SwitchResult directModeSwitch(char targetId) {
        char sourceId = m_activeDeckId;
        auto start = std::chrono::steady_clock::now();

        ReferenceDeck* tgt = deck(targetId);
        if (!tgt) {
            return {sourceId, targetId, SwitchingMode::Normal, 0.0, false};
        }

        // Backend sample rate reconfiguration (e.g. 44.1k -> 192k)
        m_graphRate = tgt->trackInfo().sampleRate;

        // Reconfigure active deck for direct mode (bypass resampler)
        tgt->loadTrack(tgt->trackInfo(), m_graphRate);
        tgt->play();
        m_activeDeckId = targetId;

        // Simulate ALSA/PipeWire driver stream renegotiation handshake delay (typically 20-50ms)
        std::this_thread::sleep_for(std::chrono::milliseconds(25));

        auto finish = std::chrono::steady_clock::now();
        double elapsedMs = std::chrono::duration<double, std::milli>(finish - start).count();

        return {sourceId, targetId, SwitchingMode::Normal, elapsedMs, true};
    }

    /// Quick toggle between Deck A and Deck B
    SwitchResult quickSwitchAB(SwitchingMode mode = SwitchingMode::Comparative) {
        char target = (m_activeDeckId == 'A') ? 'B' : 'A';
        return switchDeck(target, mode);
    }

    /// Render planar audio block from the currently active deck.
    /// In Normal mode, active decks in background also advance frames to maintain synchronization.
    size_t renderActiveBlock(double* outLeft, double* outRight, size_t frames) {
        ReferenceDeck* active = activeDeck();
        size_t rendered = 0;

        if (active) {
            rendered = active->renderBlock(outLeft, outRight, frames);
        } else {
            std::fill_n(outLeft, frames, 0.0);
            std::fill_n(outRight, frames, 0.0);
        }

        // Advance any playing background decks
        for (auto& d : m_decks) {
            if (d.get() != active && d->state() == TransportState::Playing) {
                std::vector<double> discardL(frames);
                std::vector<double> discardR(frames);
                d->renderBlock(discardL.data(), discardR.data(), frames);
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
    double m_graphRate = 48000.0;
    char m_activeDeckId = 'A';
    char m_previousDeckId = 'B';
    std::array<std::unique_ptr<ReferenceDeck>, 4> m_decks;
};

} // namespace desk32::engine::deck
