#pragma once

#include <cstdint>

namespace desk32::core::log {

/// Centralized Error Code Registry (Section 10.5 / Log Tab Observability §7)
/// 0 is reserved for success / non-error events.
namespace ErrorCode {

// 1000-1999: Decoder / Track / Seek / File I/O
constexpr int32_t TrackOpenFailed = 1001;
constexpr int32_t TrackLockedOrCorrupt = 1002;
constexpr int32_t TrackOpenTimeout = 1003;
constexpr int32_t SeekNotOpen = 1010;
constexpr int32_t SeekOutOfRange = 1011;
constexpr int32_t SeekDecoderFailed = 1012;
constexpr int32_t SeekTimeout = 1013;
constexpr int32_t DecoderUnderrunDetected = 1020;
constexpr int32_t AutoAdvanceDuplicateEof = 1030;
constexpr int32_t AutoAdvanceStaleGeneration = 1031;
constexpr int32_t AutoAdvanceStaleRequest = 1032;
constexpr int32_t AutoAdvanceTransitionInProgress = 1033;
constexpr int32_t AutoAdvanceSessionInactive = 1034;
constexpr int32_t AutoAdvanceNotPlaying = 1035;
constexpr int32_t AutoAdvanceTrackMismatch = 1036;

// 2000-2999: AudioBackend / PipeWire / ALSA / JACK
constexpr int32_t PipeWireInitFailed = 2001;
constexpr int32_t PipeWireStartFailed = 2002;
constexpr int32_t AudioXrunDetected = 2010;

// 3000-3999: External Insert / Plugin Host / DSP Queue
constexpr int32_t DspQueueFull = 3001;
constexpr int32_t ExternalHostFailed = 3002;
constexpr int32_t ExternalPluginMissing = 3003;
constexpr int32_t DspBypassResyncFailed = 3005;

// 4000-4999: Render / Export
constexpr int32_t RenderDiskSpaceLow = 4001;
constexpr int32_t RenderWriteFailed = 4002;
constexpr int32_t RenderRollbackWarning = 4003;
constexpr int32_t RenderAuditionFailed = 4004;
constexpr int32_t RenderCancelled = 4005;

// 5000-5999: DSP / Preset
constexpr int32_t PresetTruncatedWarn = 5001;
constexpr int32_t PresetParseFailed = 5002;
constexpr int32_t PresetNotFound = 5003;
constexpr int32_t PresetSaveFailed = 5004;

// 6000-6999: Graphics / RHI / SceneGraph
constexpr int32_t VulkanFallbackToOpenGL = 6001;
constexpr int32_t SceneGraphInitError = 6002;

// 9000-9999: Logging / Diagnostics Subsystem
constexpr int32_t LogQueueOverflow = 9001;

} // namespace ErrorCode

} // namespace desk32::core::log
