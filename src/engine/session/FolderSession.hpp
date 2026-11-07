#pragma once

#include <string>
#include <vector>
#include <algorithm>
#include <QCollator>
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
    /// Strictly deterministic natural sort comparison:
    /// 1. Numerical chunk extraction and numerical magnitude comparison
    /// 2. If numerical magnitudes equal, leading zero resolution via total chunk length
    /// 3. Non-numeric characters compared case-insensitively (ASCII/Unicode toLower)
    /// 4. If all chunks equal, exact binary byte comparison (Qt::CaseSensitive) as deterministic tie-breaker
    /// Completely immune to OS locale, ICU dependency, or Qt QCollator implementation discrepancies.
    static bool naturalSortLess(const QString& a, const QString& b) {
        qsizetype i = 0, j = 0;
        const qsizetype lenA = a.length();
        const qsizetype lenB = b.length();

        while (i < lenA && j < lenB) {
            const QChar ca = a[i];
            const QChar cb = b[j];

            if (ca.isDigit() && cb.isDigit()) {
                // Numeric chunk comparison
                qsizetype startA = i;
                while (i < lenA && a[i] == '0') i++;
                qsizetype numStartA = i;
                while (i < lenA && a[i].isDigit()) i++;
                qsizetype numLenA = i - numStartA;

                qsizetype startB = j;
                while (j < lenB && b[j] == '0') j++;
                qsizetype numStartB = j;
                while (j < lenB && b[j].isDigit()) j++;
                qsizetype numLenB = j - numStartB;

                // Compare significant digits length
                if (numLenA != numLenB) {
                    return numLenA < numLenB;
                }

                // If same number of significant digits, compare digit-by-digit
                for (qsizetype k = 0; k < numLenA; ++k) {
                    if (a[numStartA + k] != b[numStartB + k]) {
                        return a[numStartA + k] < b[numStartB + k];
                    }
                }

                // If numeric value is identical (e.g. "02" vs "2"), resolve by total length
                qsizetype totalLenA = i - startA;
                qsizetype totalLenB = j - startB;
                if (totalLenA != totalLenB) {
                    return totalLenA < totalLenB;
                }
            } else {
                // Non-numeric chunk comparison (case-insensitive)
                const QChar lowA = ca.toLower();
                const QChar lowB = cb.toLower();
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

        // Final deterministic tie-breaker: strict binary case-sensitive comparison
        return a.compare(b, Qt::CaseSensitive) < 0;
    }

    bool operator()(const QString& a, const QString& b) const {
        return naturalSortLess(a, b);
    }

    bool operator()(const SessionTrack& a, const SessionTrack& b) const {
        return naturalSortLess(QString::fromStdString(a.fileName), QString::fromStdString(b.fileName));
    }
};

class FolderSession {
public:
    FolderSession() = default;

    [[nodiscard]] bool isActive() const noexcept { return m_isActive; }
    [[nodiscard]] const std::string& folderPath() const noexcept { return m_folderPath; }
    [[nodiscard]] const std::vector<SessionTrack>& tracks() const noexcept { return m_tracks; }
    [[nodiscard]] size_t currentIndex() const noexcept { return m_currentIndex; }
    [[nodiscard]] size_t totalTracks() const noexcept { return m_tracks.size(); }

    /// Build deterministic snapshot from folder contents
    bool startSession(const std::string& folderPath, const std::vector<std::string>& fileNames) {
        m_folderPath = folderPath;
        m_tracks.clear();
        m_tracks.reserve(fileNames.size());

        NaturalSortComparator comparator;
        std::vector<QString> sortedNames;
        sortedNames.reserve(fileNames.size());
        for (const auto& fn : fileNames) {
            sortedNames.push_back(QString::fromStdString(fn));
        }

        // Apply strict deterministic natural sort
        std::sort(sortedNames.begin(), sortedNames.end(), comparator);

        for (size_t i = 0; i < sortedNames.size(); ++i) {
            SessionTrack tr;
            tr.fileName = sortedNames[i].toStdString();
            tr.path = folderPath + "/" + tr.fileName;
            tr.index = i;
            m_tracks.push_back(std::move(tr));
        }

        // Specification 2.7.2: Continuous playback strictly starts from index 0
        m_currentIndex = 0;
        m_isActive = !m_tracks.empty();
        return m_isActive;
    }

    [[nodiscard]] const SessionTrack* currentTrack() const noexcept {
        if (m_currentIndex < m_tracks.size()) {
            return &m_tracks[m_currentIndex];
        }
        return nullptr;
    }

    /// Single-track read-ahead: returns next track candidate if available
    [[nodiscard]] const SessionTrack* readAheadTrack() const noexcept {
        if (m_currentIndex + 1 < m_tracks.size()) {
            return &m_tracks[m_currentIndex + 1];
        }
        return nullptr;
    }

    bool advanceNext() {
        if (m_currentIndex + 1 < m_tracks.size()) {
            m_currentIndex++;
            return true;
        }
        m_isActive = false; // Reached end of folder session
        return false;
    }

    void stopSession() {
        m_isActive = false;
        m_tracks.clear();
        m_currentIndex = 0;
    }

private:
    bool m_isActive = false;
    std::string m_folderPath;
    std::vector<SessionTrack> m_tracks;
    size_t m_currentIndex = 0;
};

} // namespace desk32::engine::session

