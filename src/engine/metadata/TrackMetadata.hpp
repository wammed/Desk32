#pragma once

#include <string>
#include <vector>
#include <map>
#include <cstdint>

namespace desk32::engine::metadata {

enum class TagWriteStatus {
    Success = 0,
    UnsupportedFormat,
    OpenFailed,
    SaveFailed,
    VerificationFailed,
    ReadOnlyOrLocked
};

struct TrackMetadata {
    static constexpr const char* PROVENANCE_MARKER_V1 = "DESK32_V1";
    static constexpr const char* KEY_MARKER = "DESK32MARKER";
    static constexpr const char* KEY_ORIG_PATH = "DESK32ORIGPATH";
    static constexpr const char* KEY_COPY_TIME = "DESK32COPYTIME";
    static constexpr const char* KEY_ORIG_SHA256 = "DESK32ORIGSHA";

    // Standard Tag Fields (Specification §2.5)
    std::string title;
    std::string artist;
    std::string album;
    std::string albumArtist;
    int32_t trackNumber = 0;
    int32_t totalTracks = 0;
    int32_t discNumber = 0;
    int32_t totalDiscs = 0;
    std::string dateOrYear;
    std::string genre;
    std::string composer;
    std::string conductor;
    std::string performer;
    std::string arranger;
    std::string label;
    std::string publisher;
    std::string catalogNumber;
    std::string isrc;
    std::string comment;

    // Preserved additional text/numeric fields (multi-value / format-specific, excluding artwork)
    std::map<std::string, std::vector<std::string>> extraTags;

    // Workspace Copy Provenance (Specification §2.5, Read-only for user tag edits)
    bool hasProvenance = false;
    bool provenanceValid = false;
    bool provenanceCorrupt = false;
    bool provenanceEmbedSupported = false;
    std::string provenanceOriginalPath;
    std::string provenanceCopyTimestamp;
    std::string provenanceOriginalSha256;
    std::string provenanceMarker;

    // Technical Audio Parameters
    double sampleRate = 0.0;
    uint32_t channels = 0;
    uint32_t bitDepth = 0;
    double durationSeconds = 0.0;
    uint64_t totalFrames = 0;
    std::string codecName;

    // Strict Anti-Artwork Guarantee:
    // Specification 2.5 & 1.2: No extraction, no decoding, no display, no caching, and no thumbnail generation of artwork.
    [[nodiscard]] static constexpr bool hasArtworkSupport() noexcept {
        return false;
    }

    /// Return flattened canonical tag key->value pairs for save comparison / diffing
    [[nodiscard]] std::map<std::string, std::string> canonicalTagMap() const;
};

class MetadataReader {
public:
    /// Read audio metadata read-only via TagLib while strictly discarding/ignoring artwork chunks (SYS-08).
    /// Note: dummyTitle/dummyArtist are honored ONLY for synthetic non-existent test paths starting with "/path/to/".
    static TrackMetadata parseTags(const std::string& path,
                                   const std::string& dummyTitle = "",
                                   const std::string& dummyArtist = "");

    /// Check whether TagLib supports reading/embedding properties for this file container
    static bool supportsPropertyEmbedding(const std::string& path);

    /// Embed Desk32 provenance metadata tags into an imported workspace file (Spec §2.5)
    static TagWriteStatus writeProvenanceTags(const std::string& path,
                                              const std::string& originalPath,
                                              const std::string& copyTimestampIso,
                                              const std::string& originalSha256,
                                              const std::string& marker = TrackMetadata::PROVENANCE_MARKER_V1);

    /// Edit user-modifiable metadata tags in a workspace file while strictly preserving provenance tags (Spec §2.5)
    static TagWriteStatus writeUserTags(const std::string& path,
                                        const TrackMetadata& updatedMeta,
                                        std::string& outError);
};

} // namespace desk32::engine::metadata
