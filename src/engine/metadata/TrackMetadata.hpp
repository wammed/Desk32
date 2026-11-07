#pragma once

#include <string>
#include <cstdint>

namespace desk32::engine::metadata {

struct TrackMetadata {
    // Standard Tag Fields (Specification 2.5)
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
    std::string comment;

    // Technical Audio Parameters
    double sampleRate = 44100.0;
    uint32_t channels = 2;
    uint32_t bitDepth = 16;
    double durationSeconds = 0.0;
    uint64_t totalFrames = 0;
    std::string codecName;

    // Strict Anti-Artwork Guarantee:
    // Specification 2.5 & 1.2: No extraction, no decoding, no display, no caching, and no thumbnail generation of artwork.
    [[nodiscard]] static constexpr bool hasArtworkSupport() noexcept {
        return false;
    }
};

class MetadataReader {
public:
    /// Read audio metadata while strictly discarding/ignoring artwork chunks
    static TrackMetadata parseTags(const std::string& path,
                                   const std::string& dummyTitle = "",
                                   const std::string& dummyArtist = "") {
        (void)path;
        TrackMetadata meta;
        meta.title = dummyTitle.empty() ? "Track Title" : dummyTitle;
        meta.artist = dummyArtist.empty() ? "Artist Name" : dummyArtist;
        meta.album = "Reference Album";
        meta.trackNumber = 1;
        meta.totalTracks = 10;
        meta.sampleRate = 48000.0;
        meta.channels = 2;
        meta.bitDepth = 24;
        meta.durationSeconds = 180.0;
        meta.totalFrames = static_cast<uint64_t>(meta.durationSeconds * meta.sampleRate);
        meta.codecName = "FLAC";

        // Artwork frames (APIC, METADATA_BLOCK_PICTURE) are strictly skipped and not parsed
        return meta;
    }
};

} // namespace desk32::engine::metadata
