#include "PipeWireAudioOutput.hpp"
#include <pipewire/pipewire.h>
#include <spa/param/audio/format-utils.h>
#include <spa/param/audio/raw.h>
#include <spa/debug/types.h>
#include <spa/utils/result.h>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>

namespace desk32::engine::backend {

namespace {

enum spa_audio_format toSpaAudioFormat(OutputPcmFormat fmt) noexcept
{
    switch (fmt) {
    case OutputPcmFormat::F32LE: return SPA_AUDIO_FORMAT_F32;
    case OutputPcmFormat::S24_32LE: return SPA_AUDIO_FORMAT_S24_32;
    case OutputPcmFormat::S32LE: return SPA_AUDIO_FORMAT_S32;
    case OutputPcmFormat::S16LE: return SPA_AUDIO_FORMAT_S16;
    }
    return SPA_AUDIO_FORMAT_F32;
}

OutputPcmFormat fromSpaAudioFormat(uint32_t spaFmt) noexcept
{
    switch (spaFmt) {
    case SPA_AUDIO_FORMAT_S16:
    case SPA_AUDIO_FORMAT_S16_BE:
        return OutputPcmFormat::S16LE;
    case SPA_AUDIO_FORMAT_S24_32:
    case SPA_AUDIO_FORMAT_S24_32_BE:
        return OutputPcmFormat::S24_32LE;
    case SPA_AUDIO_FORMAT_S32:
    case SPA_AUDIO_FORMAT_S32_BE:
        return OutputPcmFormat::S32LE;
    case SPA_AUDIO_FORMAT_F32:
    case SPA_AUDIO_FORMAT_F32_BE:
    default:
        return OutputPcmFormat::F32LE;
    }
}

void convertFloatBufferToRawPcm(const float* srcFloat, size_t frames, OutputPcmFormat fmt, void* dst) noexcept
{
    const size_t totalSamples = frames * 2;
    switch (fmt) {
    case OutputPcmFormat::F32LE:
        std::memcpy(dst, srcFloat, totalSamples * sizeof(float));
        break;
    case OutputPcmFormat::S16LE: {
        auto* out = static_cast<int16_t*>(dst);
        for (size_t i = 0; i < totalSamples; ++i) {
            const double clamped = std::clamp(static_cast<double>(srcFloat[i]), -1.0, 1.0);
            out[i] = static_cast<int16_t>(std::lrint(clamped * 32767.0));
        }
        break;
    }
    case OutputPcmFormat::S24_32LE: {
        auto* out = static_cast<int32_t*>(dst);
        for (size_t i = 0; i < totalSamples; ++i) {
            const double clamped = std::clamp(static_cast<double>(srcFloat[i]), -1.0, 1.0);
            out[i] = static_cast<int32_t>(std::lrint(clamped * 8388607.0));
        }
        break;
    }
    case OutputPcmFormat::S32LE: {
        auto* out = static_cast<int32_t*>(dst);
        for (size_t i = 0; i < totalSamples; ++i) {
            const double clamped = std::clamp(static_cast<double>(srcFloat[i]), -1.0, 1.0);
            out[i] = static_cast<int32_t>(std::lrint(clamped * 2147483647.0));
        }
        break;
    }
    }
}

struct EnumContext {
    std::vector<AudioOutputDeviceInfo> devices;
    int syncSeq = 0;
    bool syncDone = false;
};

void onEnumCoreDone(void* data, uint32_t /*id*/, int seq)
{
    auto* ctx = static_cast<EnumContext*>(data);
    if (ctx && seq == ctx->syncSeq) {
        ctx->syncDone = true;
    }
}

void onEnumRegistryGlobal(void* data, uint32_t id, uint32_t /*permissions*/,
                          const char* type, uint32_t /*version*/,
                          const struct spa_dict* props)
{
    auto* ctx = static_cast<EnumContext*>(data);
    if (!ctx || !type || !props) return;
    if (std::strcmp(type, PW_TYPE_INTERFACE_Node) != 0) return;

    const char* mediaClass = spa_dict_lookup(props, PW_KEY_MEDIA_CLASS);
    if (!mediaClass || std::strcmp(mediaClass, "Audio/Sink") != 0) return;

    const char* nodeName = spa_dict_lookup(props, PW_KEY_NODE_NAME);
    const char* nodeDesc = spa_dict_lookup(props, PW_KEY_NODE_DESCRIPTION);
    const char* nodeNick = spa_dict_lookup(props, PW_KEY_NODE_NICK);

    AudioOutputDeviceInfo dev;
    dev.nodeId = id;
    dev.name = nodeName ? nodeName : ("sink-" + std::to_string(id));
    dev.description = nodeDesc ? nodeDesc : (nodeNick ? nodeNick : dev.name);
    dev.mediaClass = mediaClass;
    dev.channels = 2;
    if (const char* chStr = spa_dict_lookup(props, PW_KEY_AUDIO_CHANNELS)) {
        const uint32_t parsedCh = static_cast<uint32_t>(std::strtoul(chStr, nullptr, 10));
        if (parsedCh > 0) {
            dev.channels = parsedCh;
        }
    }
    dev.nativeFormat = OutputPcmFormat::F32LE;

    if (const char* rateStr = spa_dict_lookup(props, PW_KEY_NODE_RATE)) {
        // Expect formats like "1/48000" or "48000"
        if (const char* slash = std::strchr(rateStr, '/')) {
            dev.sampleRate = static_cast<uint32_t>(std::strtoul(slash + 1, nullptr, 10));
        } else {
            dev.sampleRate = static_cast<uint32_t>(std::strtoul(rateStr, nullptr, 10));
        }
    }
    ctx->devices.push_back(std::move(dev));
}

} // namespace

static void handleStreamStateChanged(void* data, enum pw_stream_state oldState, enum pw_stream_state newState, const char* error)
{
    PipeWireAudioOutput::onStreamStateChanged(data, static_cast<int>(oldState), static_cast<int>(newState), error);
}

static void handleStreamParamChanged(void* data, uint32_t id, const struct spa_pod* param)
{
    PipeWireAudioOutput::onStreamParamChanged(data, id, param);
}

static void handleStreamProcess(void* data)
{
    PipeWireAudioOutput::onStreamProcess(data);
}

static const struct pw_stream_events streamEvents = {
    .version = PW_VERSION_STREAM_EVENTS,
    .destroy = nullptr,
    .state_changed = handleStreamStateChanged,
    .control_info = nullptr,
    .io_changed = nullptr,
    .param_changed = handleStreamParamChanged,
    .add_buffer = nullptr,
    .remove_buffer = nullptr,
    .process = handleStreamProcess,
    .drained = nullptr,
    .command = nullptr,
    .trigger_done = nullptr
};

PipeWireAudioOutput::PipeWireAudioOutput()
    : m_streamListener(std::make_unique<struct spa_hook>())
{
    pw_init(nullptr, nullptr);
}

PipeWireAudioOutput::~PipeWireAudioOutput()
{
    shutdown();
}

bool PipeWireAudioOutput::initialize(uint32_t sampleRate, uint32_t bufferFrames)
{
    m_requestedSampleRate = sampleRate;
    m_requestedBufferFrames = bufferFrames;
    m_fallbackFloatBuffer.assign(16384 * 2, 0.0f);
    {
        std::lock_guard<std::mutex> lock(m_deviceMutex);
        if (m_targetDeviceId.empty()) {
            m_deviceName = m_offlineSimulation ? "offline_simulation" : "pipewire_default";
        }
    }

    m_threadLoop = pw_thread_loop_new("desk32-pw-output", nullptr);
    if (!m_threadLoop) {
        return false;
    }

    struct pw_properties* props = pw_properties_new(
        PW_KEY_CONFIG_NAME, "client-rt.conf",
        PW_KEY_MEDIA_TYPE, "Audio",
        PW_KEY_MEDIA_CATEGORY, "Playback",
        PW_KEY_MEDIA_ROLE, "Music",
        nullptr
    );

    m_context = pw_context_new(pw_thread_loop_get_loop(m_threadLoop), props, 0);
    if (!m_context) {
        // Fallback for minimal/headless containers without client-rt.conf
        props = pw_properties_new(nullptr, nullptr);
        m_context = pw_context_new(pw_thread_loop_get_loop(m_threadLoop), props, 0);
        if (!m_context) {
            pw_thread_loop_destroy(m_threadLoop);
            m_threadLoop = nullptr;
            return false;
        }
    }

    m_initialized.store(true, std::memory_order_relaxed);
    return true;
}

bool PipeWireAudioOutput::start()
{
    if (m_offlineSimulation) {
        m_negotiatedFormat.store(m_preferredFormat.load(std::memory_order_relaxed), std::memory_order_relaxed);
        m_negotiatedQuantum.store(m_requestedBufferFrames, std::memory_order_relaxed);
        m_state.store(StreamState::Streaming, std::memory_order_relaxed);
        return true;
    }

    if (!m_context || !m_threadLoop) {
        return false;
    }

    // Connect to PipeWire context first (thread loop is not started yet)
    m_core = pw_context_connect(m_context, nullptr, 0);
    if (!m_core) {
        return false;
    }

    char latencyBuf[64];
    std::snprintf(latencyBuf, sizeof(latencyBuf), "%u/%u",
                  m_requestedBufferFrames > 0 ? m_requestedBufferFrames : 256u,
                  m_requestedSampleRate > 0 ? m_requestedSampleRate : 48000u);

    std::string targetObj;
    {
        std::lock_guard<std::mutex> lock(m_deviceMutex);
        targetObj = m_targetDeviceId;
    }

    struct pw_properties* streamProps = pw_properties_new(
        PW_KEY_MEDIA_TYPE, "Audio",
        PW_KEY_MEDIA_CATEGORY, "Playback",
        PW_KEY_MEDIA_ROLE, "Music",
        PW_KEY_APP_NAME, "Desk32",
        PW_KEY_NODE_NAME, "desk32-playback",
        PW_KEY_NODE_DESCRIPTION, "Desk32 Master Reference Audio Output",
        PW_KEY_NODE_LATENCY, latencyBuf,
        nullptr
    );
    if (!targetObj.empty() && targetObj != "default") {
        pw_properties_set(streamProps, PW_KEY_TARGET_OBJECT, targetObj.c_str());
    }

    m_stream = pw_stream_new(m_core, "Desk32 Playback Stream", streamProps);
    if (!m_stream) {
        pw_core_disconnect(m_core);
        m_core = nullptr;
        return false;
    }

    pw_stream_add_listener(m_stream, m_streamListener.get(), &streamEvents, this);

    // Build format parameter pod(s): preferred format first, F32LE fallback if needed
    uint8_t buffer[2048];
    struct spa_pod_builder builder;
    spa_pod_builder_init(&builder, buffer, sizeof(buffer));

    const OutputPcmFormat prefFmt = m_preferredFormat.load(std::memory_order_relaxed);
    struct spa_audio_info_raw infoPref = {};
    infoPref.format = toSpaAudioFormat(prefFmt);
    infoPref.channels = 2;
    infoPref.position[0] = SPA_AUDIO_CHANNEL_FL;
    infoPref.position[1] = SPA_AUDIO_CHANNEL_FR;
    infoPref.rate = m_requestedSampleRate;

    const struct spa_pod* params[2];
    uint32_t paramCount = 1;
    params[0] = spa_format_audio_raw_build(&builder, SPA_PARAM_EnumFormat, &infoPref);

    if (prefFmt != OutputPcmFormat::F32LE) {
        struct spa_audio_info_raw infoFallback = infoPref;
        infoFallback.format = SPA_AUDIO_FORMAT_F32;
        params[1] = spa_format_audio_raw_build(&builder, SPA_PARAM_EnumFormat, &infoFallback);
        paramCount = 2;
    }

    enum pw_stream_flags flags = static_cast<enum pw_stream_flags>(
        PW_STREAM_FLAG_AUTOCONNECT |
        PW_STREAM_FLAG_MAP_BUFFERS |
        PW_STREAM_FLAG_RT_PROCESS
    );

    int res = pw_stream_connect(m_stream, PW_DIRECTION_OUTPUT, PW_ID_ANY, flags, params, paramCount);
    if (res < 0) {
        pw_stream_destroy(m_stream);
        m_stream = nullptr;
        pw_core_disconnect(m_core);
        m_core = nullptr;
        return false;
    }

    if (pw_thread_loop_start(m_threadLoop) < 0) {
        pw_stream_disconnect(m_stream);
        pw_stream_destroy(m_stream);
        m_stream = nullptr;
        pw_core_disconnect(m_core);
        m_core = nullptr;
        return false;
    }

    m_state.store(StreamState::Connecting, std::memory_order_relaxed);
    return true;
}

void PipeWireAudioOutput::pause()
{
    if (m_offlineSimulation) {
        m_state.store(StreamState::Paused, std::memory_order_relaxed);
        return;
    }

    if (m_threadLoop && m_stream) {
        pw_thread_loop_lock(m_threadLoop);
        pw_stream_set_active(m_stream, false);
        m_state.store(StreamState::Paused, std::memory_order_relaxed);
        pw_thread_loop_unlock(m_threadLoop);
    }
}

void PipeWireAudioOutput::stop()
{
    if (m_offlineSimulation) {
        m_state.store(StreamState::Paused, std::memory_order_relaxed);
        return;
    }

    if (m_threadLoop) {
        pw_thread_loop_stop(m_threadLoop);

        pw_thread_loop_lock(m_threadLoop);
        if (m_stream) {
            pw_stream_disconnect(m_stream);
            pw_stream_destroy(m_stream);
            m_stream = nullptr;
        }
        m_streamListener = std::make_unique<struct spa_hook>();
        if (m_core) {
            pw_core_disconnect(m_core);
            m_core = nullptr;
        }
        m_state.store(StreamState::Unconnected, std::memory_order_relaxed);
        pw_thread_loop_unlock(m_threadLoop);
    }
}

bool PipeWireAudioOutput::renegotiateSampleRate(uint32_t targetRate, double& outSilenceMs)
{
    const auto t0 = std::chrono::steady_clock::now();
    if (targetRate == 0) {
        outSilenceMs = 0.0;
        return false;
    }

    if (m_offlineSimulation || !m_threadLoop || !m_context) {
        m_requestedSampleRate = targetRate;
        m_negotiatedSampleRate.store(0, std::memory_order_relaxed);
        const auto t1 = std::chrono::steady_clock::now();
        outSilenceMs = std::chrono::duration<double, std::milli>(t1 - t0).count();
        m_lastRenegotiationSilenceMs.store(outSilenceMs, std::memory_order_relaxed);
        return true;
    }

    const bool wasStreamingOrConnecting =
        (m_state.load(std::memory_order_relaxed) == StreamState::Streaming ||
         m_state.load(std::memory_order_relaxed) == StreamState::Connecting ||
         m_stream != nullptr);

    if (wasStreamingOrConnecting) {
        stop();
    }

    m_requestedSampleRate = targetRate;
    m_negotiatedSampleRate.store(0, std::memory_order_relaxed);
    m_isDirectMode.store(false, std::memory_order_relaxed);
    m_directModeStatus.store(DirectModeStatus::Unknown, std::memory_order_relaxed);

    bool ok = true;
    if (wasStreamingOrConnecting) {
        ok = start();
    }

    const auto t1 = std::chrono::steady_clock::now();
    outSilenceMs = std::chrono::duration<double, std::milli>(t1 - t0).count();
    m_lastRenegotiationSilenceMs.store(outSilenceMs, std::memory_order_relaxed);
    return ok;
}

std::vector<AudioOutputDeviceInfo> PipeWireAudioOutput::enumerateOutputDevices() const
{
    std::string activeTarget;
    std::vector<AudioOutputDeviceInfo> simCopy;
    {
        std::lock_guard<std::mutex> lock(m_deviceMutex);
        activeTarget = m_targetDeviceId;
        simCopy = m_simulatedDevices;
    }

    if (m_offlineSimulation) {
        if (!simCopy.empty()) {
            for (auto& d : simCopy) {
                d.isDefault = activeTarget.empty()
                                  ? d.isDefault
                                  : (d.name == activeTarget || std::to_string(d.nodeId) == activeTarget);
            }
            return simCopy;
        }
        AudioOutputDeviceInfo def;
        def.nodeId = 1;
        def.name = "offline_simulation";
        def.description = "Offline Simulation Sink";
        def.mediaClass = "Audio/Sink";
        def.isDefault = true;
        def.sampleRate = m_requestedSampleRate;
        def.channels = 2;
        def.nativeFormat = m_negotiatedFormat.load(std::memory_order_relaxed);
        return {def};
    }

    // Live PipeWire registry enumeration using a bounded short-lived loop
    struct pw_main_loop* loop = pw_main_loop_new(nullptr);
    if (!loop) {
        return simCopy;
    }

    struct pw_properties* ctxProps = pw_properties_new(
        PW_KEY_CONFIG_NAME, "client-rt.conf",
        nullptr
    );
    struct pw_context* ctx = pw_context_new(pw_main_loop_get_loop(loop), ctxProps, 0);
    if (!ctx) {
        ctxProps = pw_properties_new(nullptr, nullptr);
        ctx = pw_context_new(pw_main_loop_get_loop(loop), ctxProps, 0);
    }
    if (!ctx) {
        pw_main_loop_destroy(loop);
        return simCopy;
    }

    struct pw_core* core = pw_context_connect(ctx, nullptr, 0);
    if (!core) {
        pw_context_destroy(ctx);
        pw_main_loop_destroy(loop);
        return simCopy;
    }

    EnumContext enumCtx;
    struct pw_core_events coreEvents{};
    coreEvents.version = PW_VERSION_CORE_EVENTS;
    coreEvents.done = onEnumCoreDone;
    struct spa_hook coreListener{};
    pw_core_add_listener(core, &coreListener, &coreEvents, &enumCtx);

    struct pw_registry* registry = pw_core_get_registry(core, PW_VERSION_REGISTRY, 0);
    if (!registry) {
        pw_core_disconnect(core);
        pw_context_destroy(ctx);
        pw_main_loop_destroy(loop);
        return simCopy;
    }

    struct pw_registry_events regEvents{};
    regEvents.version = PW_VERSION_REGISTRY_EVENTS;
    regEvents.global = onEnumRegistryGlobal;
    struct spa_hook regListener{};
    pw_registry_add_listener(registry, &regListener, &regEvents, &enumCtx);

    enumCtx.syncSeq = pw_core_sync(core, PW_ID_CORE, 0);
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(150);
    struct pw_loop* pwLoop = pw_main_loop_get_loop(loop);
    while (!enumCtx.syncDone && std::chrono::steady_clock::now() < deadline) {
        pw_loop_iterate(pwLoop, 20);
    }

    spa_hook_remove(&regListener);
    pw_proxy_destroy(reinterpret_cast<struct pw_proxy*>(registry));
    spa_hook_remove(&coreListener);
    pw_core_disconnect(core);
    pw_context_destroy(ctx);
    pw_main_loop_destroy(loop);

    for (auto& d : enumCtx.devices) {
        if (!activeTarget.empty()) {
            d.isDefault = (d.name == activeTarget || std::to_string(d.nodeId) == activeTarget);
        }
    }
    return enumCtx.devices;
}

void PipeWireAudioOutput::setSimulatedOutputDevices(std::vector<AudioOutputDeviceInfo> devices)
{
    std::lock_guard<std::mutex> lock(m_deviceMutex);
    m_simulatedDevices = std::move(devices);
}

bool PipeWireAudioOutput::setTargetDevice(const std::string& deviceIdOrName, std::string* outError)
{
    const std::string normalized = (deviceIdOrName == "default") ? std::string{} : deviceIdOrName;
    {
        std::lock_guard<std::mutex> lock(m_deviceMutex);
        m_targetDeviceId = normalized;
        if (normalized.empty()) {
            m_deviceName = m_offlineSimulation ? "offline_simulation" : "pipewire_default";
        } else {
            m_deviceName = normalized;
        }
    }

    if (m_offlineSimulation || !m_threadLoop || !m_context) {
        return true;
    }

    const StreamState curState = m_state.load(std::memory_order_relaxed);
    const bool wasActive = (curState == StreamState::Streaming ||
                            curState == StreamState::Connecting ||
                            m_stream != nullptr);
    if (!wasActive) {
        return true;
    }

    stop();
    if (!start()) {
        if (outError) {
            *outError = "Failed to restart PipeWire playback stream on target device: " +
                        (normalized.empty() ? std::string("default") : normalized);
        }
        return false;
    }
    return true;
}

std::string PipeWireAudioOutput::targetDeviceId() const
{
    std::lock_guard<std::mutex> lock(m_deviceMutex);
    return m_targetDeviceId;
}

bool PipeWireAudioOutput::setBufferFrames(uint32_t bufferFrames)
{
    if (bufferFrames == 0 || bufferFrames > 16384) {
        return false;
    }
    m_requestedBufferFrames = bufferFrames;
    if (m_offlineSimulation) {
        m_negotiatedQuantum.store(bufferFrames, std::memory_order_relaxed);
        return true;
    }
    if (m_threadLoop && m_stream) {
        char latencyBuf[64];
        std::snprintf(latencyBuf, sizeof(latencyBuf), "%u/%u",
                      m_requestedBufferFrames,
                      m_requestedSampleRate > 0 ? m_requestedSampleRate : 48000u);
        pw_thread_loop_lock(m_threadLoop);
        struct pw_properties* props = pw_properties_new(
            PW_KEY_NODE_LATENCY, latencyBuf,
            nullptr
        );
        pw_stream_update_properties(m_stream, &props->dict);
        pw_properties_free(props);
        pw_thread_loop_unlock(m_threadLoop);
    }
    return true;
}

bool PipeWireAudioOutput::setPreferredFormat(OutputPcmFormat format)
{
    m_preferredFormat.store(format, std::memory_order_relaxed);
    if (m_offlineSimulation) {
        m_negotiatedFormat.store(format, std::memory_order_relaxed);
        return true;
    }

    const StreamState curState = m_state.load(std::memory_order_relaxed);
    const bool wasActive = (curState == StreamState::Streaming ||
                            curState == StreamState::Connecting ||
                            m_stream != nullptr);
    if (!wasActive) {
        m_negotiatedFormat.store(format, std::memory_order_relaxed);
        return true;
    }

    stop();
    return start();
}

void PipeWireAudioOutput::shutdown()
{
    stop();
    if (m_context) {
        pw_context_destroy(m_context);
        m_context = nullptr;
    }
    if (m_threadLoop) {
        pw_thread_loop_destroy(m_threadLoop);
        m_threadLoop = nullptr;
    }
    m_initialized.store(false, std::memory_order_relaxed);
}

StreamDiagnostics PipeWireAudioOutput::diagnostics() const noexcept
{
    StreamDiagnostics diag;
    const uint32_t negotiated = m_negotiatedSampleRate.load(std::memory_order_relaxed);
    diag.requestedSampleRate = m_requestedSampleRate;
    diag.sampleRate = negotiated > 0 ? negotiated : m_requestedSampleRate;
    diag.channels = 2;
    diag.bufferFrames = m_requestedBufferFrames;
    diag.negotiatedQuantum = m_negotiatedQuantum.load(std::memory_order_relaxed);
    diag.preferredFormat = m_preferredFormat.load(std::memory_order_relaxed);
    diag.negotiatedFormat = m_negotiatedFormat.load(std::memory_order_relaxed);
    diag.formatName = outputPcmFormatName(diag.negotiatedFormat);
    diag.outputBitDepth = outputPcmFormatBitDepth(diag.negotiatedFormat);
    diag.outputFormatLossless = outputPcmFormatIsLosslessForSourceBits(
        diag.negotiatedFormat, m_sourceBitDepth.load(std::memory_order_relaxed));
    const bool fmtUsesDither =
        (diag.negotiatedFormat == OutputPcmFormat::S24_32LE ||
         diag.negotiatedFormat == OutputPcmFormat::S16LE);
    diag.ditherActive = fmtUsesDither && m_ditherActive.load(std::memory_order_relaxed);
    {
        std::lock_guard<std::mutex> lock(m_deviceMutex);
        diag.deviceName = m_deviceName;
        diag.targetDeviceId = m_targetDeviceId;
    }
    diag.xrunCount = m_xrunCount.load(std::memory_order_relaxed);
    diag.totalFramesProcessed = m_totalFramesProcessed.load(std::memory_order_relaxed);
    diag.isDirectMode = m_isDirectMode.load(std::memory_order_relaxed);
    diag.directModeStatus = m_directModeStatus.load(std::memory_order_relaxed);
    diag.lastCallbackDurationNs = m_lastCallbackDurationNs.load(std::memory_order_relaxed);
    diag.maxCallbackDurationNs = m_maxCallbackDurationNs.load(std::memory_order_relaxed);
    diag.lastCallbackIntervalNs = m_lastCallbackIntervalNs.load(std::memory_order_relaxed);
    diag.expectedPeriodNs = m_expectedPeriodNs.load(std::memory_order_relaxed);
    diag.lastSchedJitterNs = m_lastSchedJitterNs.load(std::memory_order_relaxed);
    diag.maxAbsSchedJitterNs = m_maxAbsSchedJitterNs.load(std::memory_order_relaxed);
    diag.callbackCount = m_callbackCount.load(std::memory_order_relaxed);
    return diag;
}

void PipeWireAudioOutput::renderOfflineBlock(float* outInterleaved, size_t frames)
{
    if (m_rawCallback) {
        m_rawCallback(outInterleaved, frames, OutputPcmFormat::F32LE);
        m_totalFramesProcessed.fetch_add(frames, std::memory_order_relaxed);
    } else if (m_callback) {
        m_callback(outInterleaved, frames);
        m_totalFramesProcessed.fetch_add(frames, std::memory_order_relaxed);
    } else {
        std::memset(outInterleaved, 0, frames * 2 * sizeof(float));
    }
}

void PipeWireAudioOutput::renderOfflineRawBlock(void* outInterleaved, size_t frames, OutputPcmFormat* outUsedFormat)
{
    const OutputPcmFormat fmt = m_negotiatedFormat.load(std::memory_order_relaxed);
    if (outUsedFormat) {
        *outUsedFormat = fmt;
    }
    if (m_rawCallback) {
        m_rawCallback(outInterleaved, frames, fmt);
        m_totalFramesProcessed.fetch_add(frames, std::memory_order_relaxed);
    } else if (m_callback) {
        if (fmt == OutputPcmFormat::F32LE) {
            m_callback(static_cast<float*>(outInterleaved), frames);
        } else {
            const size_t nFrames = std::min(frames, m_fallbackFloatBuffer.size() / 2);
            m_callback(m_fallbackFloatBuffer.data(), nFrames);
            convertFloatBufferToRawPcm(m_fallbackFloatBuffer.data(), nFrames, fmt, outInterleaved);
        }
        m_totalFramesProcessed.fetch_add(frames, std::memory_order_relaxed);
    } else {
        std::memset(outInterleaved, 0, frames * 2 * outputPcmFormatBytesPerSample(fmt));
    }
}

void PipeWireAudioOutput::onStreamStateChanged(void* data, int /*oldState*/, int newState, const char* error)
{
    auto* self = static_cast<PipeWireAudioOutput*>(data);
    switch (static_cast<enum pw_stream_state>(newState)) {
        case PW_STREAM_STATE_ERROR:
            self->m_state.store(StreamState::Error, std::memory_order_relaxed);
            self->m_isDirectMode.store(false, std::memory_order_relaxed);
            self->m_directModeStatus.store(DirectModeStatus::Unknown, std::memory_order_relaxed);
            if (error) {
                std::cerr << "PipeWire stream error: " << error << std::endl;
            }
            break;
        case PW_STREAM_STATE_UNCONNECTED:
            self->m_state.store(StreamState::Unconnected, std::memory_order_relaxed);
            self->m_isDirectMode.store(false, std::memory_order_relaxed);
            self->m_directModeStatus.store(DirectModeStatus::Unknown, std::memory_order_relaxed);
            break;
        case PW_STREAM_STATE_CONNECTING:
            self->m_state.store(StreamState::Connecting, std::memory_order_relaxed);
            break;
        case PW_STREAM_STATE_PAUSED:
            self->m_state.store(StreamState::Paused, std::memory_order_relaxed);
            break;
        case PW_STREAM_STATE_STREAMING:
            self->m_state.store(StreamState::Streaming, std::memory_order_relaxed);
            if (self->m_stream) {
                if (const struct pw_properties* sprops = pw_stream_get_properties(self->m_stream)) {
                    const char* target = pw_properties_get(sprops, PW_KEY_TARGET_OBJECT);
                    if (!target) target = pw_properties_get(sprops, "node.target");
                    if (target && target[0] != '\0') {
                        std::lock_guard<std::mutex> lock(self->m_deviceMutex);
                        self->m_deviceName = target;
                    }
                }
            }
            break;
    }
}

void PipeWireAudioOutput::onStreamParamChanged(void* data, uint32_t id, const struct spa_pod* param)
{
    auto* self = static_cast<PipeWireAudioOutput*>(data);
    if (!param || id != SPA_PARAM_Format) return;

    struct spa_audio_info info = {};
    if (spa_format_parse(param, &info.media_type, &info.media_subtype) < 0) {
        return;
    }

    if (info.media_type == SPA_MEDIA_TYPE_audio && info.media_subtype == SPA_MEDIA_SUBTYPE_raw) {
        struct spa_audio_info_raw raw = {};
        spa_format_audio_raw_parse(param, &raw);
        self->m_negotiatedSampleRate.store(raw.rate, std::memory_order_relaxed);
        self->m_negotiatedFormat.store(fromSpaAudioFormat(raw.format), std::memory_order_relaxed);

        // Section 2.6: Direct Mode verified when negotiated hardware/graph rate matches requested rate
        if (raw.rate > 0 && raw.rate == self->m_requestedSampleRate) {
            self->m_isDirectMode.store(true, std::memory_order_relaxed);
            self->m_directModeStatus.store(DirectModeStatus::Active, std::memory_order_relaxed);
        } else {
            self->m_isDirectMode.store(false, std::memory_order_relaxed);
            self->m_directModeStatus.store(DirectModeStatus::Disabled, std::memory_order_relaxed);
        }
    }
}

void PipeWireAudioOutput::onStreamProcess(void* data)
{
    const auto cbStart = std::chrono::steady_clock::now();
    const int64_t cbStartNs = std::chrono::duration_cast<std::chrono::nanoseconds>(cbStart.time_since_epoch()).count();
    auto* self = static_cast<PipeWireAudioOutput*>(data);

    struct pw_buffer* pwBuf = pw_stream_dequeue_buffer(self->m_stream);
    if (!pwBuf) {
        self->m_xrunCount.fetch_add(1, std::memory_order_relaxed);
        return;
    }

    struct spa_buffer* spaBuf = pwBuf->buffer;
    void* dst = spaBuf->datas[0].data;
    if (!dst) {
        pw_stream_queue_buffer(self->m_stream, pwBuf);
        return;
    }

    const OutputPcmFormat fmt = self->m_negotiatedFormat.load(std::memory_order_relaxed);
    const uint32_t frameBytes = outputPcmFormatBytesPerSample(fmt) * 2u;
    const uint32_t maxFrames = spaBuf->datas[0].maxsize / frameBytes;
    const uint32_t reqFrames = (pwBuf->requested > 0) ? static_cast<uint32_t>(pwBuf->requested) : self->m_requestedBufferFrames;
    if (pwBuf->requested > 0) {
        self->m_negotiatedQuantum.store(static_cast<uint32_t>(pwBuf->requested), std::memory_order_relaxed);
    }
    const uint32_t nFrames = std::min(maxFrames, reqFrames);

    const uint32_t rate = (self->m_requestedSampleRate > 0) ? self->m_requestedSampleRate : 48000;
    const uint64_t expectedNs = (static_cast<uint64_t>(nFrames) * 1000000000ULL) / static_cast<uint64_t>(rate);
    self->m_expectedPeriodNs.store(expectedNs, std::memory_order_relaxed);
    if (self->m_prevCallbackStartNs > 0 && cbStartNs >= self->m_prevCallbackStartNs) {
        const uint64_t intervalNs = static_cast<uint64_t>(cbStartNs - self->m_prevCallbackStartNs);
        self->m_lastCallbackIntervalNs.store(intervalNs, std::memory_order_relaxed);
        const int64_t jitterNs = static_cast<int64_t>(intervalNs) - static_cast<int64_t>(expectedNs);
        self->m_lastSchedJitterNs.store(jitterNs, std::memory_order_relaxed);
        const uint64_t absJitter = static_cast<uint64_t>(jitterNs < 0 ? -jitterNs : jitterNs);
        const uint64_t prevMaxJitter = self->m_maxAbsSchedJitterNs.load(std::memory_order_relaxed);
        if (absJitter > prevMaxJitter) {
            self->m_maxAbsSchedJitterNs.store(absJitter, std::memory_order_relaxed);
        }
    }
    self->m_prevCallbackStartNs = cbStartNs;

    if (self->m_rawCallback) {
        self->m_rawCallback(dst, nFrames, fmt);
    } else if (self->m_callback) {
        if (fmt == OutputPcmFormat::F32LE) {
            self->m_callback(static_cast<float*>(dst), nFrames);
        } else {
            const size_t safeFrames = std::min(static_cast<size_t>(nFrames), self->m_fallbackFloatBuffer.size() / 2);
            self->m_callback(self->m_fallbackFloatBuffer.data(), safeFrames);
            convertFloatBufferToRawPcm(self->m_fallbackFloatBuffer.data(), safeFrames, fmt, dst);
        }
    } else {
        std::memset(dst, 0, static_cast<size_t>(nFrames) * frameBytes);
    }

    spaBuf->datas[0].chunk->offset = 0;
    spaBuf->datas[0].chunk->stride = static_cast<int32_t>(frameBytes);
    spaBuf->datas[0].chunk->size = nFrames * frameBytes;

    self->m_totalFramesProcessed.fetch_add(nFrames, std::memory_order_relaxed);
    self->m_callbackCount.fetch_add(1, std::memory_order_relaxed);
    pw_stream_queue_buffer(self->m_stream, pwBuf);

    const auto cbEnd = std::chrono::steady_clock::now();
    const uint64_t durNs = static_cast<uint64_t>(std::max<int64_t>(
        0, std::chrono::duration_cast<std::chrono::nanoseconds>(cbEnd - cbStart).count()));
    self->m_lastCallbackDurationNs.store(durNs, std::memory_order_relaxed);
    const uint64_t prevMaxDur = self->m_maxCallbackDurationNs.load(std::memory_order_relaxed);
    if (durNs > prevMaxDur) {
        self->m_maxCallbackDurationNs.store(durNs, std::memory_order_relaxed);
    }
}

} // namespace desk32::engine::backend
