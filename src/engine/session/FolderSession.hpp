#pragma once

#include <string>
#include <string_view>
#include <vector>
#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstring>
#include <QString>
#include <QFileInfo>
#include <QDir>

namespace desk32::engine::session {

struct SessionTrack {
    std::string path;
    std::string fileName;
    size_t index = 0;
};

class NaturalSortComparator {
public:
    /// Strictly deterministic natural sort comparison (Phase 0 Specification):
    /// 1. Numerical chunk extraction and numerical magnitude comparison
    /// 2. If numerical magnitudes equal, leading zero resolution via total chunk length (shorter leading zeros first)
    /// 3. Non-numeric characters compared case-insensitively (ASCII std::tolower)
    /// 4. Final tie-breaker: strict binary UTF-8 byte comparison (std::lexicographical_compare on raw unsigned char)
    /// Completely immune to OS locale, ICU dependency, or Qt QString UTF-16 code point discrepancies.
    static bool naturalSortLess(std::string_view a, std::string_view b) {
        size_t i = 0, j = 0;
        const size_t lenA = a.length();
        const size_t lenB = b.length();

        auto isDigit = [](char c) -> bool {
            return c >= '0' && c <= '9';
        };

        while (i < lenA && j < lenB) {
            const unsigned char ca = static_cast<unsigned char>(a[i]);
            const unsigned char cb = static_cast<unsigned char>(b[j]);

            if (isDigit(a[i]) && isDigit(b[j])) {
                // Numeric chunk comparison
                size_t startA = i;
                while (i < lenA && a[i] == '0') i++;
                size_t numStartA = i;
                while (i < lenA && isDigit(a[i])) i++;
                size_t numLenA = i - numStartA;

                size_t startB = j;
                while (j < lenB && b[j] == '0') j++;
                size_t numStartB = j;
                while (j < lenB && isDigit(b[j])) j++;
                size_t numLenB = j - numStartB;

                // Compare significant digits length
                if (numLenA != numLenB) {
                    return numLenA < numLenB;
                }

                // If same number of significant digits, compare digit-by-digit
                for (size_t k = 0; k < numLenA; ++k) {
                    if (a[numStartA + k] != b[numStartB + k]) {
                        return a[numStartA + k] < b[numStartB + k];
                    }
                }

                // If numeric value is identical (e.g. "02" vs "2"), resolve by total length (shorter first)
                size_t totalLenA = i - startA;
                size_t totalLenB = j - startB;
                if (totalLenA != totalLenB) {
                    return totalLenA < totalLenB;
                }
            } else {
                // Non-numeric chunk comparison (case-insensitive for ASCII)
                unsigned char lowA = (ca >= 'A' && ca <= 'Z') ? static_cast<unsigned char>(ca + ('a' - 'A')) : ca;
                unsigned char lowB = (cb >= 'A' && cb <= 'Z') ? static_cast<unsigned char>(cb + ('a' - 'A')) : cb;
                if (lowA != lowB) {
                    return lowA < lowB;
                }
                i++;
                j++;
            }
        }

        // Shorter prefix goes first
        if (i < lenA || j < lenB) {
            return lenA < lenB;
        }

        // Final deterministic tie-breaker: strict binary UTF-8 byte comparison (unsigned char)
        return std::lexicographical_compare(
            reinterpret_cast<const unsigned char*>(a.data()),
            reinterpret_cast<const unsigned char*>(a.data()) + a.size(),
            reinterpret_cast<const unsigned char*>(b.data()),
            reinterpret_cast<const unsigned char*>(b.data()) + b.size()
        );
    }

    static bool naturalSortLess(const QString& a, const QString& b) {
        auto uA = a.toUtf8();
        auto uB = b.toUtf8();
        return naturalSortLess(std::string_view(uA.constData(), static_cast<size_t>(uA.size())),
                               std::string_view(uB.constData(), static_cast<size_t>(uB.size())));
    }

    bool operator()(const std::string& a, const std::string& b) const {
        return naturalSortLess(a, b);
    }

    bool operator()(const QString& a, const QString& b) const {
        return naturalSortLess(a, b);
    }

    bool operator()(const SessionTrack& a, const SessionTrack& b) const {
        return naturalSortLess(a.fileName, b.fileName);
    }
};

class FolderSession {
public:
    FolderSession();
    ~FolderSession();

    [[nodiscard]] bool isActive() const noexcept { return m_isActive.load(std::memory_order_acquire); }
    [[nodiscard]] uint64_t sessionId() const noexcept { return m_sessionId.load(std::memory_order_acquire); }
    [[nodiscard]] const std::string& folderPath() const noexcept { return m_folderPath; }
    [[nodiscard]] const std::vector<SessionTrack>& tracks() const noexcept { return m_tracks; }
    [[nodiscard]] size_t currentIndex() const noexcept { return m_currentIndex.load(std::memory_order_acquire); }
    [[nodiscard]] size_t totalTracks() const noexcept { return m_tracks.size(); }

    /// Deterministic FNV-1a 64-bit hash for RT-safe track identity comparison
    [[nodiscard]] static uint64_t hashTrackPath(std::string_view path) noexcept {
        uint64_t h = 14695981039346656037ULL;
        for (char c : path) {
            h ^= static_cast<uint64_t>(static_cast<unsigned char>(c));
            h *= 1099511628211ULL;
        }
        return h;
    }

    /// Build deterministic snapshot from folder contents
    bool startSession(const std::string& folderPath, const std::vector<std::string>& fileNames) {
        m_folderPath = folderPath;
        m_tracks.clear();
        m_tracks.reserve(fileNames.size());

        std::vector<std::string> sortedNames = fileNames;

        // Apply strict deterministic natural sort with UTF-8 byte tie-breaker
        std::sort(sortedNames.begin(), sortedNames.end(), NaturalSortComparator{});

        for (size_t i = 0; i < sortedNames.size(); ++i) {
            SessionTrack tr;
            tr.fileName = sortedNames[i];
            tr.path = folderPath + "/" + tr.fileName;
            tr.index = i;
            m_tracks.push_back(std::move(tr));
        }

        // Specification 2.7.2: Continuous playback strictly starts from index 0
        m_currentIndex.store(0, std::memory_order_release);
        const bool active = !m_tracks.empty();
        if (active) {
            m_sessionId.store(s_nextSessionId.fetch_add(1, std::memory_order_relaxed), std::memory_order_release);
        } else {
            m_sessionId.store(0, std::memory_order_release);
        }
        m_isActive.store(active, std::memory_order_release);
        return active;
    }

    [[nodiscard]] const SessionTrack* currentTrack() const noexcept {
        const size_t idx = m_currentIndex.load(std::memory_order_acquire);
        if (idx < m_tracks.size()) {
            return &m_tracks[idx];
        }
        return nullptr;
    }

    /// Single-track read-ahead: returns next track candidate if available
    [[nodiscard]] const SessionTrack* readAheadTrack() const noexcept {
        const size_t idx = m_currentIndex.load(std::memory_order_acquire);
        if (idx + 1 < m_tracks.size()) {
            return &m_tracks[idx + 1];
        }
        return nullptr;
    }

    bool advanceNext() {
        const size_t idx = m_currentIndex.load(std::memory_order_acquire);
        if (idx + 1 < m_tracks.size()) {
            m_currentIndex.store(idx + 1, std::memory_order_release);
            return true;
        }
        m_isActive.store(false, std::memory_order_release); // Reached end of folder session
        return false;
    }

    bool retreatPrevious() {
        const size_t idx = m_currentIndex.load(std::memory_order_acquire);
        if (idx > 0) {
            m_currentIndex.store(idx - 1, std::memory_order_release);
            return true;
        }
        return false;
    }

    bool seekTrack(size_t index) {
        if (index < m_tracks.size()) {
            m_currentIndex.store(index, std::memory_order_release);
            m_isActive.store(true, std::memory_order_release);
            return true;
        }
        return false;
    }

    void stopSession() {
        m_isActive.store(false, std::memory_order_release);
        m_sessionId.store(0, std::memory_order_release);
        m_tracks.clear();
        m_currentIndex.store(0, std::memory_order_release);
    }

private:
    inline static std::atomic<uint64_t> s_nextSessionId{1};
    std::atomic<bool> m_isActive{false};
    std::atomic<uint64_t> m_sessionId{0};
    std::string m_folderPath;
    std::vector<SessionTrack> m_tracks;
    std::atomic<size_t> m_currentIndex{0};
};

} // namespace desk32::engine::session

