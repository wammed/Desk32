#pragma once

#include "ReferenceDeck.hpp"
#include <array>
#include <chrono>

namespace desk32::engine::deck {

enum class SwitchingMode {
    Normal,
    Comparative
};

struct SwitchResult {
    char sourceDeck;
    char targetDeck;
    SwitchingMode mode;
    double muteTimeMs;
    bool success;
};

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
    [[nodiscard]] char activeDeckId() const noexcept { return m_activeDeckId; }

    ReferenceDeck* deck(char id) {
        size_t idx = static_cast<size_t>(id - 'A');
        if (idx < 4) {
            return m_decks[idx].get();
        }
        return nullptr;
    }

    const ReferenceDeck* deck(char id) const {
        size_t idx = static_cast<size_t>(id - 'A');
        if (idx < 4) {
            return m_decks[idx].get();
        }
        return nullptr;
    }

    ReferenceDeck* activeDeck() {
        return deck(m_activeDeckId);
    }

    /// Perform deck switching and measure mute duration until valid audio frames are generated.
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
            // Comparative mode: calculate equivalent time position
            double currentTimeSec = 0.0;
            if (src->trackInfo().sampleRate > 0) {
                currentTimeSec = static_cast<double>(src->playhead()) / src->trackInfo().sampleRate;
            }
            src->pause();

            // Align target playhead with offset
            int64_t targetFrame = static_cast<int64_t>(currentTimeSec * tgt->trackInfo().sampleRate) + tgt->offsetFrames();
            if (targetFrame < 0) targetFrame = 0;
            tgt->seek(static_cast<uint64_t>(targetFrame));
            tgt->play();
        } else {
            // Normal mode: maintains current target play/pause state and position
            // (If target was stopped or paused, it remains so until user acts)
        }

        m_activeDeckId = targetId;

        // Render one block to verify audio output pipeline response
        std::vector<double> block(256 * 2);
        tgt->renderBlock(block.data(), 256);

        auto finish = std::chrono::steady_clock::now();
        double elapsedMs = std::chrono::duration<double, std::milli>(finish - start).count();

        return {sourceId, targetId, mode, elapsedMs, true};
    }

    /// Direct Mode renegotiation simulation: re-negotiates graph rate directly with backend
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

        std::vector<double> block(256 * 2);
        tgt->renderBlock(block.data(), 256);

        auto finish = std::chrono::steady_clock::now();
        double elapsedMs = std::chrono::duration<double, std::milli>(finish - start).count();

        return {sourceId, targetId, SwitchingMode::Normal, elapsedMs, true};
    }

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
    std::array<std::unique_ptr<ReferenceDeck>, 4> m_decks;
};

} // namespace desk32::engine::deck

