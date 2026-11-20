#include "PipeWireInsertTransport.hpp"

#include <pipewire/pipewire.h>
#include <pipewire/link.h>
#include <spa/param/audio/format-utils.h>
#include <spa/param/audio/raw.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <ctime>

namespace desk32::engine::insert {

namespace {

void onCoreDoneProxy(void* data, uint32_t id, int seq)
{
    PipeWireInsertTransport::onCoreDone(data, id, seq);
}

void onRegGlobalProxy(void* data, uint32_t id, uint32_t permissions,
                      const char* type, uint32_t version,
                      const struct ::spa_dict* props)
{
    PipeWireInsertTransport::onRegistryGlobal(data, id, permissions, type, version, props);
}

void onRegGlobalRemoveProxy(void* data, uint32_t id)
{
    PipeWireInsertTransport::onRegistryGlobalRemove(data, id);
}

void onSendStateProxy(void* data, enum pw_stream_state oldState, enum pw_stream_state newState, const char* error)
{
    PipeWireInsertTransport::onSendStateChanged(data, static_cast<int>(oldState), static_cast<int>(newState), error);
}

void onReturnStateProxy(void* data, enum pw_stream_state oldState, enum pw_stream_state newState, const char* error)
{
    PipeWireInsertTransport::onReturnStateChanged(data, static_cast<int>(oldState), static_cast<int>(newState), error);
}

void onSendParamProxy(void* data, uint32_t id, const struct spa_pod* param)
{
    PipeWireInsertTransport::onSendParamChanged(data, id, param);
}

void onReturnParamProxy(void* data, uint32_t id, const struct spa_pod* param)
{
    PipeWireInsertTransport::onReturnParamChanged(data, id, param);
}

void onSendProcessProxy(void* data)
{
    PipeWireInsertTransport::onSendProcess(data);
}

void onReturnProcessProxy(void* data)
{
    PipeWireInsertTransport::onReturnProcess(data);
}

} // namespace

PipeWireInsertTransport::PipeWireInsertTransport()
    : m_simDelayL(RING_CAPACITY_FRAMES, 0.0)
    , m_simDelayR(RING_CAPACITY_FRAMES, 0.0)
    , m_coreListener(std::make_unique<struct spa_hook>())
    , m_registryListener(std::make_unique<struct spa_hook>())
    , m_sendListener(std::make_unique<struct spa_hook>())
    , m_returnListener(std::make_unique<struct spa_hook>())
{
    pw_init(nullptr, nullptr);
}

PipeWireInsertTransport::~PipeWireInsertTransport()
{
    disconnect();
}

bool PipeWireInsertTransport::configureAndConnect(const ExternalPortInfo& target,
                                                  double engineSampleRate,
                                                  size_t engineBufferSize)
{
    disconnect();

    {
        std::lock_guard<std::mutex> lock(m_stateMutex);
        m_targetConfig = target;
        m_expectedSampleRate = engineSampleRate > 0.0 ? engineSampleRate : 48000.0;
        m_expectedBufferSize = engineBufferSize > 0 ? engineBufferSize : 256;
        m_errorDetail.clear();
        m_nodesById.clear();
        m_portsById.clear();
        m_watchedIds.clear();
        m_syncDone = false;
    }

    m_sendRing.fullReset();
    m_returnRing.fullReset();
    std::fill(m_simDelayL.begin(), m_simDelayL.end(), 0.0);
    std::fill(m_simDelayR.begin(), m_simDelayR.end(), 0.0);
    m_simWritePos = 0;

    m_rateMismatch.store(false, std::memory_order_release);
    m_bufferMismatch.store(false, std::memory_order_release);
    m_graphDisconnected.store(false, std::memory_order_release);
    m_resolvedTargetPorts.store(0, std::memory_order_release);
    m_activeLinksCreated.store(0, std::memory_order_release);
    m_sendBlocksWritten.store(0, std::memory_order_release);
    m_returnBlocksRead.store(0, std::memory_order_release);
    m_xrunOrUnderruns.store(0, std::memory_order_release);
    m_negotiatedSendRate.store(0, std::memory_order_release);
    m_negotiatedReturnRate.store(0, std::memory_order_release);
    m_negotiatedQuantum.store(0, std::memory_order_release);
    m_sendNodeId.store(0, std::memory_order_release);
    m_returnNodeId.store(0, std::memory_order_release);

    if (m_simulationMode.load(std::memory_order_acquire)) {
        m_resolvedTargetPorts.store(4, std::memory_order_release);
        m_activeLinksCreated.store(4, std::memory_order_release);
        m_negotiatedSendRate.store(static_cast<uint32_t>(m_expectedSampleRate), std::memory_order_release);
        m_negotiatedReturnRate.store(static_cast<uint32_t>(m_expectedSampleRate), std::memory_order_release);
        m_negotiatedQuantum.store(static_cast<uint32_t>(m_expectedBufferSize), std::memory_order_release);
        m_connected.store(true, std::memory_order_release);
        return true;
    }

    auto failAndCleanup = [this](const std::string& msg) {
        disconnect();
        std::lock_guard<std::mutex> lock(m_stateMutex);
        m_errorDetail = msg;
    };

    m_threadLoop = pw_thread_loop_new("desk32-pw-insert", nullptr);
    if (!m_threadLoop) {
        failAndCleanup("Failed to allocate PipeWire insert thread loop");
        return false;
    }

    struct pw_properties* ctxProps = pw_properties_new(
        PW_KEY_CONFIG_NAME, "client-rt.conf",
        PW_KEY_MEDIA_TYPE, "Audio",
        nullptr
    );
    m_context = pw_context_new(pw_thread_loop_get_loop(m_threadLoop), ctxProps, 0);
    if (!m_context) {
        ctxProps = pw_properties_new(nullptr, nullptr);
        m_context = pw_context_new(pw_thread_loop_get_loop(m_threadLoop), ctxProps, 0);
    }
    if (!m_context) {
        failAndCleanup("Failed to initialize PipeWire insert context");
        return false;
    }

    m_core = pw_context_connect(m_context, nullptr, 0);
    if (!m_core) {
        failAndCleanup("PipeWire daemon unavailable for External Insert");
        return false;
    }

    static struct pw_core_events coreEvents{};
    coreEvents.version = PW_VERSION_CORE_EVENTS;
    coreEvents.done = onCoreDoneProxy;
    pw_core_add_listener(m_core, m_coreListener.get(), &coreEvents, this);

    m_registry = pw_core_get_registry(m_core, PW_VERSION_REGISTRY, 0);
    if (!m_registry) {
        failAndCleanup("Failed to bind PipeWire registry for External Insert");
        return false;
    }

    static struct pw_registry_events regEvents{};
    regEvents.version = PW_VERSION_REGISTRY_EVENTS;
    regEvents.global = onRegGlobalProxy;
    regEvents.global_remove = onRegGlobalRemoveProxy;
    pw_registry_add_listener(m_registry, m_registryListener.get(), &regEvents, this);

    // Create Send playback stream (no autoconnect — explicit 4-port Link routing only)
    struct pw_properties* sendProps = pw_properties_new(
        PW_KEY_MEDIA_TYPE, "Audio",
        PW_KEY_MEDIA_CATEGORY, "Duplex",
        PW_KEY_MEDIA_ROLE, "DSP",
        PW_KEY_APP_NAME, "Desk32",
        PW_KEY_NODE_NAME, "desk32-insert-send",
        PW_KEY_NODE_DESCRIPTION, "Desk32 External Insert Send",
        nullptr
    );
    m_sendStream = pw_stream_new(m_core, "Desk32 Insert Send", sendProps);

    struct pw_properties* retProps = pw_properties_new(
        PW_KEY_MEDIA_TYPE, "Audio",
        PW_KEY_MEDIA_CATEGORY, "Duplex",
        PW_KEY_MEDIA_ROLE, "DSP",
        PW_KEY_APP_NAME, "Desk32",
        PW_KEY_NODE_NAME, "desk32-insert-return",
        PW_KEY_NODE_DESCRIPTION, "Desk32 External Insert Return",
        nullptr
    );
    m_returnStream = pw_stream_new(m_core, "Desk32 Insert Return", retProps);

    if (!m_sendStream || !m_returnStream) {
        failAndCleanup("Failed to create PipeWire Send/Return insert streams");
        return false;
    }

    static struct pw_stream_events sendEvents{};
    sendEvents.version = PW_VERSION_STREAM_EVENTS;
    sendEvents.state_changed = onSendStateProxy;
    sendEvents.param_changed = onSendParamProxy;
    sendEvents.process = onSendProcessProxy;
    pw_stream_add_listener(m_sendStream, m_sendListener.get(), &sendEvents, this);

    static struct pw_stream_events retEvents{};
    retEvents.version = PW_VERSION_STREAM_EVENTS;
    retEvents.state_changed = onReturnStateProxy;
    retEvents.param_changed = onReturnParamProxy;
    retEvents.process = onReturnProcessProxy;
    pw_stream_add_listener(m_returnStream, m_returnListener.get(), &retEvents, this);

    uint8_t fmtBuf[1024];
    struct spa_pod_builder builder;
    spa_pod_builder_init(&builder, fmtBuf, sizeof(fmtBuf));
    struct spa_audio_info_raw rawInfo = {};
    rawInfo.format = SPA_AUDIO_FORMAT_F32;
    rawInfo.channels = 2;
    rawInfo.position[0] = SPA_AUDIO_CHANNEL_FL;
    rawInfo.position[1] = SPA_AUDIO_CHANNEL_FR;
    rawInfo.rate = static_cast<uint32_t>(m_expectedSampleRate);
    const struct spa_pod* params[1];
    params[0] = spa_format_audio_raw_build(&builder, SPA_PARAM_EnumFormat, &rawInfo);

    const enum pw_stream_flags flags = static_cast<enum pw_stream_flags>(
        PW_STREAM_FLAG_MAP_BUFFERS | PW_STREAM_FLAG_RT_PROCESS
    );

    if (pw_stream_connect(m_sendStream, PW_DIRECTION_OUTPUT, PW_ID_ANY, flags, params, 1) < 0 ||
        pw_stream_connect(m_returnStream, PW_DIRECTION_INPUT, PW_ID_ANY, flags, params, 1) < 0) {
        failAndCleanup("Failed to connect PipeWire Send/Return streams");
        return false;
    }

    if (pw_thread_loop_start(m_threadLoop) < 0) {
        failAndCleanup("Failed to start PipeWire insert thread loop");
        return false;
    }
    m_threadLoopStarted = true;

    // Perform bounded 150ms registry roundtrip to resolve target 4 ports & self ports and establish 4 PW Links
    pw_thread_loop_lock(m_threadLoop);
    m_syncSeq = pw_core_sync(m_core, PW_ID_CORE, 0);
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(150);
    while (!m_syncDone && std::chrono::steady_clock::now() < deadline) {
        struct timespec abstime{};
        pw_thread_loop_get_time(m_threadLoop, &abstime, 50 * SPA_NSEC_PER_MSEC);
        pw_thread_loop_timed_wait_full(m_threadLoop, &abstime);
    }
    tryResolveAndCreateLinksLocked();
    if (m_activeLinksCreated.load(std::memory_order_acquire) < 4) {
        struct timespec abstime{};
        pw_thread_loop_get_time(m_threadLoop, &abstime, 100 * SPA_NSEC_PER_MSEC);
        pw_thread_loop_timed_wait_full(m_threadLoop, &abstime);
        tryResolveAndCreateLinksLocked();
    }
    const uint32_t linksCreated = m_activeLinksCreated.load(std::memory_order_acquire);
    const uint32_t portsResolved = m_resolvedTargetPorts.load(std::memory_order_acquire);
    pw_thread_loop_unlock(m_threadLoop);

    if (linksCreated < 4) {
        failAndCleanup("Target External Insert ports not found or link creation incomplete (resolvedPorts=" +
                       std::to_string(portsResolved) + ", links=" + std::to_string(linksCreated) + ")");
        return false;
    }

    m_connected.store(true, std::memory_order_release);
    return true;
}

void PipeWireInsertTransport::disconnect()
{
    m_connected.store(false, std::memory_order_release);
    if (m_threadLoop) {
        if (m_threadLoopStarted) {
            pw_thread_loop_stop(m_threadLoop);
            m_threadLoopStarted = false;
        }
        pw_thread_loop_lock(m_threadLoop);
        {
            std::lock_guard<std::mutex> lock(m_stateMutex);
            teardownPipeWireLocked();
        }
        pw_thread_loop_unlock(m_threadLoop);
        pw_thread_loop_destroy(m_threadLoop);
        m_threadLoop = nullptr;
    } else {
        std::lock_guard<std::mutex> lock(m_stateMutex);
        teardownPipeWireLocked();
    }
}

void PipeWireInsertTransport::teardownPipeWireLocked()
{
    for (struct pw_proxy* linkProxy : m_linkProxies) {
        if (linkProxy) {
            pw_proxy_destroy(linkProxy);
        }
    }
    m_linkProxies.clear();

    if (m_sendStream) {
        pw_stream_disconnect(m_sendStream);
        pw_stream_destroy(m_sendStream);
        m_sendStream = nullptr;
    }
    if (m_returnStream) {
        pw_stream_disconnect(m_returnStream);
        pw_stream_destroy(m_returnStream);
        m_returnStream = nullptr;
    }
    m_sendListener = std::make_unique<struct spa_hook>();
    m_returnListener = std::make_unique<struct spa_hook>();

    if (m_registry) {
        pw_proxy_destroy(reinterpret_cast<struct pw_proxy*>(m_registry));
        m_registry = nullptr;
    }
    m_registryListener = std::make_unique<struct spa_hook>();

    if (m_core) {
        pw_core_disconnect(m_core);
        m_core = nullptr;
    }
    m_coreListener = std::make_unique<struct spa_hook>();

    if (m_context) {
        pw_context_destroy(m_context);
        m_context = nullptr;
    }
}

bool PipeWireInsertTransport::exchangeBlock(double* inOutLeft, double* inOutRight, size_t frames) noexcept
{
    if (!inOutLeft || !inOutRight || frames == 0 || !m_connected.load(std::memory_order_acquire)) {
        return false;
    }

    if (m_simulationMode.load(std::memory_order_acquire)) {
        const size_t delay = static_cast<size_t>(m_simLatencyFrames.load(std::memory_order_acquire));
        const size_t cap = m_simDelayL.size();
        const size_t mask = cap - 1;
        for (size_t i = 0; i < frames; ++i) {
            const size_t wIdx = (m_simWritePos + delay) & mask;
            m_simDelayL[wIdx] = inOutLeft[i];
            m_simDelayR[wIdx] = inOutRight[i];

            const size_t rIdx = m_simWritePos & mask;
            inOutLeft[i] = m_simDelayL[rIdx];
            inOutRight[i] = m_simDelayR[rIdx];
            m_simDelayL[rIdx] = 0.0;
            m_simDelayR[rIdx] = 0.0;
            ++m_simWritePos;
        }
        m_sendBlocksWritten.fetch_add(1, std::memory_order_relaxed);
        m_returnBlocksRead.fetch_add(1, std::memory_order_relaxed);
        return true;
    }

    const size_t safeFrames = std::min(frames, m_rtReadL.size());
    const size_t written = m_sendRing.writePlanar(inOutLeft, inOutRight, safeFrames);
    if (written < safeFrames) {
        m_xrunOrUnderruns.fetch_add(1, std::memory_order_relaxed);
    }
    m_sendBlocksWritten.fetch_add(1, std::memory_order_relaxed);

    const size_t read = m_returnRing.readPlanar(m_rtReadL.data(), m_rtReadR.data(), safeFrames);
    if (read < safeFrames) {
        if (m_returnBlocksRead.load(std::memory_order_relaxed) > 2) {
            m_xrunOrUnderruns.fetch_add(1, std::memory_order_relaxed);
        }
        std::fill(m_rtReadL.begin() + static_cast<std::ptrdiff_t>(read),
                  m_rtReadL.begin() + static_cast<std::ptrdiff_t>(safeFrames), 0.0);
        std::fill(m_rtReadR.begin() + static_cast<std::ptrdiff_t>(read),
                  m_rtReadR.begin() + static_cast<std::ptrdiff_t>(safeFrames), 0.0);
    }
    std::copy_n(m_rtReadL.data(), safeFrames, inOutLeft);
    std::copy_n(m_rtReadR.data(), safeFrames, inOutRight);
    m_returnBlocksRead.fetch_add(1, std::memory_order_relaxed);
    return read > 0;
}

InsertTransportDiagnostics PipeWireInsertTransport::diagnostics() const
{
    InsertTransportDiagnostics d;
    d.connected = m_connected.load(std::memory_order_acquire);
    d.simulationMode = m_simulationMode.load(std::memory_order_acquire);
    d.sendNodeId = m_sendNodeId.load(std::memory_order_acquire);
    d.returnNodeId = m_returnNodeId.load(std::memory_order_acquire);
    d.resolvedTargetPorts = m_resolvedTargetPorts.load(std::memory_order_acquire);
    d.activeLinksCreated = m_activeLinksCreated.load(std::memory_order_acquire);
    d.sendBlocksWritten = m_sendBlocksWritten.load(std::memory_order_relaxed);
    d.returnBlocksRead = m_returnBlocksRead.load(std::memory_order_relaxed);
    d.xrunOrUnderruns = m_xrunOrUnderruns.load(std::memory_order_relaxed);
    d.negotiatedSendRate = m_negotiatedSendRate.load(std::memory_order_acquire);
    d.negotiatedReturnRate = m_negotiatedReturnRate.load(std::memory_order_acquire);
    d.negotiatedQuantum = m_negotiatedQuantum.load(std::memory_order_acquire);
    d.rateMismatchDetected = m_rateMismatch.load(std::memory_order_acquire);
    d.bufferMismatchDetected = m_bufferMismatch.load(std::memory_order_acquire);
    d.graphDisconnected = m_graphDisconnected.load(std::memory_order_acquire);
    std::lock_guard<std::mutex> lock(m_stateMutex);
    d.errorDetail = m_errorDetail;
    return d;
}

void PipeWireInsertTransport::tryResolveAndCreateLinksLocked()
{
    std::lock_guard<std::mutex> lock(m_stateMutex);
    if (!m_core || !m_linkProxies.empty()) {
        return;
    }

    auto findTargetPort = [&](const std::string& spec, const char* expectedDir) -> const DiscoveredPort* {
        for (const auto& [pid, port] : m_portsById) {
            if (!expectedDir || port.direction != expectedDir) continue;
            auto nodeIt = m_nodesById.find(port.nodeId);
            const std::string nodeName = (nodeIt != m_nodesById.end()) ? nodeIt->second : std::string{};
            const std::string fullColon = nodeName.empty() ? port.name : (nodeName + ":" + port.name);
            if (port.alias == spec || fullColon == spec ||
                (nodeName == m_targetConfig.clientName && port.name == spec)) {
                return &port;
            }
        }
        return nullptr;
    };

    const DiscoveredPort* targetInL = findTargetPort(m_targetConfig.sendPortL, "in");
    const DiscoveredPort* targetInR = findTargetPort(m_targetConfig.sendPortR, "in");
    const DiscoveredPort* targetOutL = findTargetPort(m_targetConfig.returnPortL, "out");
    const DiscoveredPort* targetOutR = findTargetPort(m_targetConfig.returnPortR, "out");

    uint32_t foundTargets = 0;
    if (targetInL) ++foundTargets;
    if (targetInR) ++foundTargets;
    if (targetOutL) ++foundTargets;
    if (targetOutR) ++foundTargets;
    m_resolvedTargetPorts.store(foundTargets, std::memory_order_release);

    const uint32_t sendNid = m_sendNodeId.load(std::memory_order_acquire);
    const uint32_t retNid = m_returnNodeId.load(std::memory_order_acquire);

    std::vector<const DiscoveredPort*> selfSendOuts;
    std::vector<const DiscoveredPort*> selfRetIns;
    for (const auto& [pid, port] : m_portsById) {
        if (sendNid > 0 && port.nodeId == sendNid && port.direction == "out") {
            selfSendOuts.push_back(&port);
        }
        if (retNid > 0 && port.nodeId == retNid && port.direction == "in") {
            selfRetIns.push_back(&port);
        }
    }

    if (!targetInL || !targetInR || !targetOutL || !targetOutR ||
        selfSendOuts.size() < 2 || selfRetIns.size() < 2) {
        return;
    }

    auto createLink = [&](const DiscoveredPort* srcPort, const DiscoveredPort* dstPort) -> bool {
        const std::string outNodeStr = std::to_string(srcPort->nodeId);
        const std::string outPortStr = std::to_string(srcPort->id);
        const std::string inNodeStr = std::to_string(dstPort->nodeId);
        const std::string inPortStr = std::to_string(dstPort->id);
        struct pw_properties* lp = pw_properties_new(
            PW_KEY_LINK_OUTPUT_NODE, outNodeStr.c_str(),
            PW_KEY_LINK_OUTPUT_PORT, outPortStr.c_str(),
            PW_KEY_LINK_INPUT_NODE, inNodeStr.c_str(),
            PW_KEY_LINK_INPUT_PORT, inPortStr.c_str(),
            PW_KEY_OBJECT_LINGER, "false",
            nullptr
        );
        struct pw_proxy* proxy = static_cast<struct pw_proxy*>(
            pw_core_create_object(m_core, "link-factory", PW_TYPE_INTERFACE_Link,
                                  PW_VERSION_LINK, &lp->dict, 0));
        pw_properties_free(lp);
        if (!proxy) {
            return false;
        }
        m_linkProxies.push_back(proxy);
        return true;
    };

    uint32_t created = 0;
    if (createLink(selfSendOuts[0], targetInL)) ++created;
    if (createLink(selfSendOuts[1], targetInR)) ++created;
    if (createLink(targetOutL, selfRetIns[0])) ++created;
    if (createLink(targetOutR, selfRetIns[1])) ++created;
    m_activeLinksCreated.store(created, std::memory_order_release);

    if (created == 4) {
        m_watchedIds.insert(targetInL->nodeId);
        m_watchedIds.insert(targetOutL->nodeId);
        m_watchedIds.insert(targetInL->id);
        m_watchedIds.insert(targetInR->id);
        m_watchedIds.insert(targetOutL->id);
        m_watchedIds.insert(targetOutR->id);
    }
}

void PipeWireInsertTransport::onCoreDone(void* data, uint32_t /*id*/, int seq)
{
    auto* self = static_cast<PipeWireInsertTransport*>(data);
    if (!self) return;
    if (seq == self->m_syncSeq) {
        {
            std::lock_guard<std::mutex> lock(self->m_stateMutex);
            self->m_syncDone = true;
        }
        if (self->m_threadLoop) {
            pw_thread_loop_signal(self->m_threadLoop, false);
        }
    }
}

void PipeWireInsertTransport::onRegistryGlobal(void* data, uint32_t id, uint32_t /*permissions*/,
                                               const char* type, uint32_t /*version*/,
                                               const struct ::spa_dict* props)
{
    auto* self = static_cast<PipeWireInsertTransport*>(data);
    if (!self || !type || !props) return;

    {
        std::lock_guard<std::mutex> lock(self->m_stateMutex);
        if (std::strcmp(type, PW_TYPE_INTERFACE_Node) == 0) {
            if (const char* name = spa_dict_lookup(props, PW_KEY_NODE_NAME)) {
                self->m_nodesById[id] = name;
            }
        } else if (std::strcmp(type, PW_TYPE_INTERFACE_Port) == 0) {
            DiscoveredPort p;
            p.id = id;
            if (const char* nid = spa_dict_lookup(props, PW_KEY_NODE_ID)) {
                p.nodeId = static_cast<uint32_t>(std::strtoul(nid, nullptr, 10));
            }
            if (const char* pname = spa_dict_lookup(props, PW_KEY_PORT_NAME)) {
                p.name = pname;
            }
            if (const char* palias = spa_dict_lookup(props, PW_KEY_PORT_ALIAS)) {
                p.alias = palias;
            }
            if (const char* pdir = spa_dict_lookup(props, PW_KEY_PORT_DIRECTION)) {
                p.direction = pdir;
            }
            self->m_portsById[id] = std::move(p);
        }
    }
    self->tryResolveAndCreateLinksLocked();
}

void PipeWireInsertTransport::onRegistryGlobalRemove(void* data, uint32_t id)
{
    auto* self = static_cast<PipeWireInsertTransport*>(data);
    if (!self) return;

    bool watchedRemoved = false;
    {
        std::lock_guard<std::mutex> lock(self->m_stateMutex);
        self->m_portsById.erase(id);
        self->m_nodesById.erase(id);
        if (self->m_watchedIds.count(id) > 0) {
            watchedRemoved = true;
            self->m_errorDetail = "Target PipeWire node/port removed from graph (id=" + std::to_string(id) + ")";
        }
    }

    if (watchedRemoved && self->m_connected.load(std::memory_order_acquire)) {
        self->m_graphDisconnected.store(true, std::memory_order_release);
        self->m_connected.store(false, std::memory_order_release);
    }
}

void PipeWireInsertTransport::onSendStateChanged(void* data, int /*oldState*/, int newState, const char* error)
{
    auto* self = static_cast<PipeWireInsertTransport*>(data);
    if (!self) return;
    if (self->m_sendStream) {
        const uint32_t nid = pw_stream_get_node_id(self->m_sendStream);
        if (nid != PW_ID_ANY) {
            self->m_sendNodeId.store(nid, std::memory_order_release);
        }
    }
    const auto st = static_cast<enum pw_stream_state>(newState);
    if (st == PW_STREAM_STATE_ERROR) {
        self->m_graphDisconnected.store(true, std::memory_order_release);
        self->m_connected.store(false, std::memory_order_release);
        if (error) {
            std::lock_guard<std::mutex> lock(self->m_stateMutex);
            self->m_errorDetail = std::string("PipeWire insert send stream error: ") + error;
        }
    }
}

void PipeWireInsertTransport::onReturnStateChanged(void* data, int /*oldState*/, int newState, const char* error)
{
    auto* self = static_cast<PipeWireInsertTransport*>(data);
    if (!self) return;
    if (self->m_returnStream) {
        const uint32_t nid = pw_stream_get_node_id(self->m_returnStream);
        if (nid != PW_ID_ANY) {
            self->m_returnNodeId.store(nid, std::memory_order_release);
        }
    }
    const auto st = static_cast<enum pw_stream_state>(newState);
    if (st == PW_STREAM_STATE_ERROR) {
        self->m_graphDisconnected.store(true, std::memory_order_release);
        self->m_connected.store(false, std::memory_order_release);
        if (error) {
            std::lock_guard<std::mutex> lock(self->m_stateMutex);
            self->m_errorDetail = std::string("PipeWire insert return stream error: ") + error;
        }
    }
}

void PipeWireInsertTransport::onSendParamChanged(void* data, uint32_t id, const struct spa_pod* param)
{
    auto* self = static_cast<PipeWireInsertTransport*>(data);
    if (!self || !param || id != SPA_PARAM_Format) return;

    struct spa_audio_info info = {};
    if (spa_format_parse(param, &info.media_type, &info.media_subtype) < 0) return;
    if (info.media_type == SPA_MEDIA_TYPE_audio && info.media_subtype == SPA_MEDIA_SUBTYPE_raw) {
        struct spa_audio_info_raw raw = {};
        spa_format_audio_raw_parse(param, &raw);
        self->m_negotiatedSendRate.store(raw.rate, std::memory_order_release);
        if (raw.rate > 0 && std::abs(static_cast<double>(raw.rate) - self->m_expectedSampleRate) > 0.5) {
            self->m_rateMismatch.store(true, std::memory_order_release);
        }
    }
}

void PipeWireInsertTransport::onReturnParamChanged(void* data, uint32_t id, const struct spa_pod* param)
{
    auto* self = static_cast<PipeWireInsertTransport*>(data);
    if (!self || !param || id != SPA_PARAM_Format) return;

    struct spa_audio_info info = {};
    if (spa_format_parse(param, &info.media_type, &info.media_subtype) < 0) return;
    if (info.media_type == SPA_MEDIA_TYPE_audio && info.media_subtype == SPA_MEDIA_SUBTYPE_raw) {
        struct spa_audio_info_raw raw = {};
        spa_format_audio_raw_parse(param, &raw);
        self->m_negotiatedReturnRate.store(raw.rate, std::memory_order_release);
        if (raw.rate > 0 && std::abs(static_cast<double>(raw.rate) - self->m_expectedSampleRate) > 0.5) {
            self->m_rateMismatch.store(true, std::memory_order_release);
        }
    }
}

void PipeWireInsertTransport::onSendProcess(void* data)
{
    auto* self = static_cast<PipeWireInsertTransport*>(data);
    if (!self || !self->m_sendStream) return;

    struct pw_buffer* pwBuf = pw_stream_dequeue_buffer(self->m_sendStream);
    if (!pwBuf) return;

    struct spa_buffer* spaBuf = pwBuf->buffer;
    float* dst = static_cast<float*>(spaBuf->datas[0].data);
    if (!dst) {
        pw_stream_queue_buffer(self->m_sendStream, pwBuf);
        return;
    }

    const uint32_t maxFrames = spaBuf->datas[0].maxsize / static_cast<uint32_t>(sizeof(float) * 2);
    const uint32_t reqFrames = (pwBuf->requested > 0)
                                   ? static_cast<uint32_t>(pwBuf->requested)
                                   : static_cast<uint32_t>(self->m_expectedBufferSize);
    if (pwBuf->requested > 0) {
        self->m_negotiatedQuantum.store(static_cast<uint32_t>(pwBuf->requested), std::memory_order_release);
        if (static_cast<size_t>(pwBuf->requested) != self->m_expectedBufferSize) {
            self->m_bufferMismatch.store(true, std::memory_order_release);
        }
    }

    const uint32_t nFrames = std::min<uint32_t>(maxFrames, std::min<uint32_t>(reqFrames, static_cast<uint32_t>(self->m_pwSendTmpL.size())));
    const size_t read = self->m_sendRing.readPlanar(self->m_pwSendTmpL.data(), self->m_pwSendTmpR.data(), nFrames);
    for (size_t i = 0; i < read; ++i) {
        dst[i * 2] = static_cast<float>(self->m_pwSendTmpL[i]);
        dst[i * 2 + 1] = static_cast<float>(self->m_pwSendTmpR[i]);
    }
    if (read < nFrames) {
        std::memset(dst + read * 2, 0, (static_cast<size_t>(nFrames) - read) * 2 * sizeof(float));
    }

    spaBuf->datas[0].chunk->offset = 0;
    spaBuf->datas[0].chunk->stride = static_cast<int32_t>(sizeof(float) * 2);
    spaBuf->datas[0].chunk->size = nFrames * static_cast<uint32_t>(sizeof(float) * 2);
    pw_stream_queue_buffer(self->m_sendStream, pwBuf);
}

void PipeWireInsertTransport::onReturnProcess(void* data)
{
    auto* self = static_cast<PipeWireInsertTransport*>(data);
    if (!self || !self->m_returnStream) return;

    struct pw_buffer* pwBuf = pw_stream_dequeue_buffer(self->m_returnStream);
    if (!pwBuf) return;

    struct spa_buffer* spaBuf = pwBuf->buffer;
    const float* src = static_cast<const float*>(spaBuf->datas[0].data);
    if (src && spaBuf->datas[0].chunk) {
        const uint32_t availFrames = spaBuf->datas[0].chunk->size / static_cast<uint32_t>(sizeof(float) * 2);
        const uint32_t nFrames = std::min<uint32_t>(availFrames, static_cast<uint32_t>(self->m_pwRetTmpL.size()));
        for (size_t i = 0; i < nFrames; ++i) {
            self->m_pwRetTmpL[i] = static_cast<double>(src[i * 2]);
            self->m_pwRetTmpR[i] = static_cast<double>(src[i * 2 + 1]);
        }
        self->m_returnRing.writePlanar(self->m_pwRetTmpL.data(), self->m_pwRetTmpR.data(), nFrames);
    }

    pw_stream_queue_buffer(self->m_returnStream, pwBuf);
}

} // namespace desk32::engine::insert
