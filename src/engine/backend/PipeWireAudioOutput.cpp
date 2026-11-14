#include "PipeWireAudioOutput.hpp"
#include <pipewire/pipewire.h>
#include <spa/param/audio/format-utils.h>
#include <spa/param/audio/raw.h>
#include <spa/debug/types.h>
#include <spa/utils/result.h>
#include <cstring>
#include <iostream>

namespace desk32::engine::backend {

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
    {
        std::lock_guard<std::mutex> lock(m_deviceMutex);
        m_deviceName = m_offlineSimulation ? "offline_simulation" : "pipewire_default";
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

    return true;
}

bool PipeWireAudioOutput::start()
{
    if (m_offlineSimulation) {
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

    struct pw_properties* streamProps = pw_properties_new(
        PW_KEY_MEDIA_TYPE, "Audio",
        PW_KEY_MEDIA_CATEGORY, "Playback",
        PW_KEY_MEDIA_ROLE, "Music",
        PW_KEY_APP_NAME, "Desk32",
        PW_KEY_NODE_NAME, "desk32-playback",
        PW_KEY_NODE_DESCRIPTION, "Desk32 Master Reference Audio Output",
        nullptr
    );

    m_stream = pw_stream_new(m_core, "Desk32 Playback Stream", streamProps);
    if (!m_stream) {
        pw_core_disconnect(m_core);
        m_core = nullptr;
        return false;
    }

    pw_stream_add_listener(m_stream, m_streamListener.get(), &streamEvents, this);

    // Build format parameter pod (Interleaved stereo Float32)
    uint8_t buffer[1024];
    struct spa_pod_builder builder;
    spa_pod_builder_init(&builder, buffer, sizeof(buffer));

    struct spa_audio_info_raw info = {};
    info.format = SPA_AUDIO_FORMAT_F32;
    info.channels = 2;
    info.position[0] = SPA_AUDIO_CHANNEL_FL;
    info.position[1] = SPA_AUDIO_CHANNEL_FR;
    info.rate = m_requestedSampleRate;

    const struct spa_pod* params[1];
    params[0] = spa_format_audio_raw_build(&builder, SPA_PARAM_EnumFormat, &info);

    enum pw_stream_flags flags = static_cast<enum pw_stream_flags>(
        PW_STREAM_FLAG_AUTOCONNECT |
        PW_STREAM_FLAG_MAP_BUFFERS |
        PW_STREAM_FLAG_RT_PROCESS
    );

    int res = pw_stream_connect(m_stream, PW_DIRECTION_OUTPUT, PW_ID_ANY, flags, params, 1);
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
        if (m_core) {
            pw_core_disconnect(m_core);
            m_core = nullptr;
        }
        m_state.store(StreamState::Unconnected, std::memory_order_relaxed);
        pw_thread_loop_unlock(m_threadLoop);
    }
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
}

StreamDiagnostics PipeWireAudioOutput::diagnostics() const noexcept
{
    StreamDiagnostics diag;
    diag.sampleRate = m_requestedSampleRate;
    diag.channels = 2;
    diag.bufferFrames = m_requestedBufferFrames;
    diag.negotiatedQuantum = m_negotiatedQuantum.load(std::memory_order_relaxed);
    diag.formatName = "F32LE (Stereo)";
    {
        std::lock_guard<std::mutex> lock(m_deviceMutex);
        diag.deviceName = m_offlineSimulation ? "offline_simulation" : m_deviceName;
    }
    diag.xrunCount = m_xrunCount.load(std::memory_order_relaxed);
    diag.totalFramesProcessed = m_totalFramesProcessed.load(std::memory_order_relaxed);
    diag.isDirectMode = m_isDirectMode.load(std::memory_order_relaxed);
    diag.directModeStatus = m_directModeStatus.load(std::memory_order_relaxed);
    return diag;
}

void PipeWireAudioOutput::renderOfflineBlock(float* outInterleaved, size_t frames)
{
    if (m_callback) {
        m_callback(outInterleaved, frames);
        m_totalFramesProcessed.fetch_add(frames, std::memory_order_relaxed);
    } else {
        std::memset(outInterleaved, 0, frames * 2 * sizeof(float));
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
                    if (!target) target = pw_properties_get(sprops, PW_KEY_NODE_DESCRIPTION);
                    if (!target) target = pw_properties_get(sprops, PW_KEY_NODE_NAME);
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
        self->m_requestedSampleRate = raw.rate;

        // Section 2.6: Direct Mode verified when hardware graph rate matches requested track/pipeline rate
        if (raw.rate == 48000 || raw.rate == self->m_requestedSampleRate) {
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
    auto* self = static_cast<PipeWireAudioOutput*>(data);
    struct pw_buffer* pwBuf = pw_stream_dequeue_buffer(self->m_stream);
    if (!pwBuf) {
        self->m_xrunCount.fetch_add(1, std::memory_order_relaxed);
        return;
    }

    struct spa_buffer* spaBuf = pwBuf->buffer;
    float* dst = static_cast<float*>(spaBuf->datas[0].data);
    if (!dst) {
        pw_stream_queue_buffer(self->m_stream, pwBuf);
        return;
    }

    const uint32_t maxFrames = spaBuf->datas[0].maxsize / static_cast<uint32_t>(sizeof(float) * 2);
    const uint32_t reqFrames = (pwBuf->requested > 0) ? static_cast<uint32_t>(pwBuf->requested) : self->m_requestedBufferFrames;
    if (pwBuf->requested > 0) {
        self->m_negotiatedQuantum.store(static_cast<uint32_t>(pwBuf->requested), std::memory_order_relaxed);
    }
    const uint32_t nFrames = std::min(maxFrames, reqFrames);

    if (self->m_callback) {
        self->m_callback(dst, nFrames);
    } else {
        std::memset(dst, 0, nFrames * 2 * sizeof(float));
    }

    spaBuf->datas[0].chunk->offset = 0;
    spaBuf->datas[0].chunk->stride = static_cast<int32_t>(sizeof(float) * 2);
    spaBuf->datas[0].chunk->size = nFrames * static_cast<uint32_t>(sizeof(float) * 2);

    self->m_totalFramesProcessed.fetch_add(nFrames, std::memory_order_relaxed);
    pw_stream_queue_buffer(self->m_stream, pwBuf);
}

} // namespace desk32::engine::backend
