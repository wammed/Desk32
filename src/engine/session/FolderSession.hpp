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
    NaturalSortComparator() {
        m_collator.setNumericMode(true);
        m_collator.setCaseSensitivity(Qt::CaseInsensitive);
    }

    /// Strictly deterministic natural sort comparison:
    /// 1. Natural collator comparison (numeric-aware, case-insensitive)
    /// 2. If equal, length comparison (handles leading zeros like "01" vs "1")
    /// 3. If still equal, exact binary byte comparison as deterministic tie-breaker
    bool operator()(const QString& a, const QString& b) const {
        int cmp = m_collator.compare(a, b);
        if (cmp != 0) {
            return cmp < 0;
        }
        // Tie-breaker 1: length (e.g. "01" vs "1")
        if (a.length() != b.length()) {
            return a.length() > b.length(); // "01" before "1" or vice-versa consistently
        }
        // Tie-breaker 2: strict binary byte comparison
        return a.compare(b, Qt::CaseSensitive) < 0;
    }

    bool operator()(const SessionTrack& a, const SessionTrack& b) const {
        return (*this)(QString::fromStdString(a.fileName), QString::fromStdString(b.fileName));
    }

private:
    QCollator m_collator;
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

