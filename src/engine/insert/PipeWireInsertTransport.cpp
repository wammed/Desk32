#include "PipeWireInsertTransport.hpp"

#include <pipewire/pipewire.h>
#include <pipewire/link.h>
#include <spa/param/audio/format-utils.h>
#include <spa/param/audio/raw.h>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>

namespace desk32::engine::insert {

namespace {

std::string toLowerAscii(std::string s)
{
    for (char& c : s) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    return s;
}

int channelRankHint(const std::string& channel, const std::string& portName)
{
    const std::string ch = toLowerAscii(channel);
    if (ch == "fl" || ch == "mono") return 0;
    if (ch == "fr") return 1;

    const std::string low = toLowerAscii(portName);
    if (low.ends_with("_1") || low.ends_with("_l") || low.ends_with("left") ||
        low.find("fl") != std::string::npos) {
        return 0;
    }
    if (low.ends_with("_2") || low.ends_with("_r") || low.ends_with("right") ||
        low.find("fr") != std::string::npos) {
        return 1;
    }
    return 2;
}

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

void onLinkRemovedProxy(void* data)
{
    PipeWireInsertTransport::onLinkProxyRemoved(data);
}

void onLinkErrorProxy(void* data, int seq, int res, const char* message)
{
    PipeWireInsertTransport::onLinkProxyError(data, seq, res, message);
}

struct HostDiscoveryContext {
    struct NodeEntry {
        uint32_t id = 0;
        std::string name;
        std::string appName;
        std::string mediaClass;
        double rate = 0.0;
        size_t quantum = 0;
        uint32_t reportedLatency = 0;
        bool hasReportedLatency = false;
    };
    struct PortEntry {
        uint32_t id = 0;
        uint32_t nodeId = 0;
        uint32_t portOrder = 0;
        std::string name;
        std::string alias;
        std::string channel;
        std::string direction;
    };

    std::map<uint32_t, NodeEntry> nodes;
    std::vector<PortEntry> ports;
    int syncSeq = 0;
    bool syncDone = false;
};

void onDiscCoreDone(void* data, uint32_t /*id*/, int seq)
{
    auto* ctx = static_cast<HostDiscoveryContext*>(data);
    if (ctx && seq == ctx->syncSeq) {
        ctx->syncDone = true;
    }
}

void onDiscRegistryGlobal(void* data, uint32_t id, uint32_t /*permissions*/,
                          const char* type, uint32_t /*version*/,
                          const struct spa_dict* props)
{
    auto* ctx = static_cast<HostDiscoveryContext*>(data);
    if (!ctx || !type || !props) return;

    if (std::strcmp(type, PW_TYPE_INTERFACE_Node) == 0) {
        HostDiscoveryContext::NodeEntry n;
        n.id = id;
        if (const char* nn = spa_dict_lookup(props, PW_KEY_NODE_NAME)) {
            n.name = nn;
        }
        if (const char* an = spa_dict_lookup(props, PW_KEY_APP_NAME)) {
            n.appName = an;
        }
        if (n.name.empty()) {
            n.name = n.appName;
        }
        if (const char* mc = spa_dict_lookup(props, PW_KEY_MEDIA_CLASS)) {
            n.mediaClass = mc;
        }
        if (const char* lat = spa_dict_lookup(props, PW_KEY_NODE_LATENCY)) {
            if (const char* slash = std::strchr(lat, '/')) {
                const size_t q = static_cast<size_t>(std::strtoull(lat, nullptr, 10));
                const double r = std::strtod(slash + 1, nullptr);
                if (q > 0) n.quantum = q;
                if (r > 0.0) n.rate = r;
            }
        }
        if (n.rate <= 0.0) {
            if (const char* nr = spa_dict_lookup(props, PW_KEY_NODE_RATE)) {
                if (const char* slash = std::strchr(nr, '/')) {
                    const double r = std::strtod(slash + 1, nullptr);
                    if (r > 0.0) n.rate = r;
                } else {
                    const double r = std::strtod(nr, nullptr);
                    if (r > 0.0) n.rate = r;
                }
            }
        }
        if (const char* ilat = spa_dict_lookup(props, "latency.internal.rate")) {
            n.reportedLatency = static_cast<uint32_t>(std::max<long>(0, std::strtol(ilat, nullptr, 10)));
            n.hasReportedLatency = true;
        }
        ctx->nodes[id] = std::move(n);
    } else if (std::strcmp(type, PW_TYPE_INTERFACE_Port) == 0) {
        if (const char* mt = spa_dict_lookup(props, PW_KEY_MEDIA_TYPE)) {
            if (mt[0] != '\0' && std::strcmp(mt, "Audio") != 0) {
                return;
            }
        }
        if (const char* fmtDsp = spa_dict_lookup(props, PW_KEY_FORMAT_DSP)) {
            std::string fLower(fmtDsp);
            std::transform(fLower.begin(), fLower.end(), fLower.begin(), ::tolower);
            if (fLower.find("midi") != std::string::npos ||
                fLower.find("control") != std::string::npos ||
                (fLower.find("audio") == std::string::npos)) {
                return;
            }
        }
        HostDiscoveryContext::PortEntry p;
        p.id = id;
        if (const char* nid = spa_dict_lookup(props, PW_KEY_NODE_ID)) {
            p.nodeId = static_cast<uint32_t>(std::strtoul(nid, nullptr, 10));
        }
        if (const char* pid = spa_dict_lookup(props, PW_KEY_PORT_ID)) {
            p.portOrder = static_cast<uint32_t>(std::strtoul(pid, nullptr, 10));
        } else {
            p.portOrder = id;
        }
        if (const char* pn = spa_dict_lookup(props, PW_KEY_PORT_NAME)) {
            p.name = pn;
        }
        if (const char* pa = spa_dict_lookup(props, PW_KEY_PORT_ALIAS)) {
            p.alias = pa;
        }
        if (const char* ch = spa_dict_lookup(props, PW_KEY_AUDIO_CHANNEL)) {
            p.channel = ch;
        }
        if (const char* pd = spa_dict_lookup(props, PW_KEY_PORT_DIRECTION)) {
            p.direction = pd;
        }
        auto isNonAudioPortName = [](const std::string& s) {
            std::string lo = s;
            std::transform(lo.begin(), lo.end(), lo.begin(), ::tolower);
            return lo.find("midi") != std::string::npos ||
                   lo.find("events-in") != std::string::npos ||
                   lo.find("events_in") != std::string::npos ||
                   lo.find("events-out") != std::string::npos ||
                   lo.find("events_out") != std::string::npos ||
                   lo.find("control") != std::string::npos;
        };
        if (isNonAudioPortName(p.name) || isNonAudioPortName(p.alias)) {
            return;
        }
        if (!p.name.empty() && !p.direction.empty()) {
            ctx->ports.push_back(std::move(p));
        }
    }
}

} // namespace

std::vector<ExternalPortInfo> PipeWireInsertTransport::discoverHosts(double defaultSampleRate,
                                                                     size_t defaultBufferSize)
{
    pw_init(nullptr, nullptr);
    std::vector<ExternalPortInfo> result;

    struct pw_main_loop* loop = pw_main_loop_new(nullptr);
    if (!loop) {
        return result;
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
        return result;
    }

    struct pw_core* core = pw_context_connect(ctx, nullptr, 0);
    if (!core) {
        pw_context_destroy(ctx);
        pw_main_loop_destroy(loop);
        return result;
    }

    HostDiscoveryContext discCtx;
    struct pw_core_events coreEvents{};
    coreEvents.version = PW_VERSION_CORE_EVENTS;
    coreEvents.done = onDiscCoreDone;
    struct spa_hook coreListener{};
    pw_core_add_listener(core, &coreListener, &coreEvents, &discCtx);

    struct pw_registry* registry = pw_core_get_registry(core, PW_VERSION_REGISTRY, 0);
    if (!registry) {
        spa_hook_remove(&coreListener);
        pw_core_disconnect(core);
        pw_context_destroy(ctx);
        pw_main_loop_destroy(loop);
        return result;
    }

    struct pw_registry_events regEvents{};
    regEvents.version = PW_VERSION_REGISTRY_EVENTS;
    regEvents.global = onDiscRegistryGlobal;
    struct spa_hook regListener{};
    pw_registry_add_listener(registry, &regListener, &regEvents, &discCtx);

    discCtx.syncSeq = pw_core_sync(core, PW_ID_CORE, 0);
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(150);
    struct pw_loop* pwLoop = pw_main_loop_get_loop(loop);
    while (!discCtx.syncDone && std::chrono::steady_clock::now() < deadline) {
        pw_loop_iterate(pwLoop, 20);
    }

    spa_hook_remove(&regListener);
    pw_proxy_destroy(reinterpret_cast<struct pw_proxy*>(registry));
    spa_hook_remove(&coreListener);
    pw_core_disconnect(core);
    pw_context_destroy(ctx);
    pw_main_loop_destroy(loop);

    struct NodeBucket {
        HostDiscoveryContext::NodeEntry node;
        std::vector<HostDiscoveryContext::PortEntry> inPorts;
        std::vector<HostDiscoveryContext::PortEntry> outPorts;
    };
    std::map<uint32_t, NodeBucket> buckets;
    for (const auto& [nid, node] : discCtx.nodes) {
        buckets[nid].node = node;
    }
    for (const auto& p : discCtx.ports) {
        auto it = buckets.find(p.nodeId);
        if (it == buckets.end()) continue;
        if (p.direction == "in") {
            it->second.inPorts.push_back(p);
        } else if (p.direction == "out") {
            it->second.outPorts.push_back(p);
        }
    }

    auto sortPorts = [](std::vector<HostDiscoveryContext::PortEntry>& list) {
        std::stable_sort(list.begin(), list.end(), [](const auto& a, const auto& b) {
            const int ra = channelRankHint(a.channel, a.name);
            const int rb = channelRankHint(b.channel, b.name);
            if (ra != rb) return ra < rb;
            if (a.portOrder != b.portOrder) return a.portOrder < b.portOrder;
            return a.id < b.id;
        });
    };

    for (auto& [nid, b] : buckets) {
        (void)nid;
        const std::string& nName = b.node.name;
        if (nName.empty() || nName.rfind("desk32-", 0) == 0 || b.node.appName == "Desk32") {
            continue;
        }
        if (b.inPorts.size() >= 2 && b.outPorts.size() >= 2) {
            sortPorts(b.inPorts);
            sortPorts(b.outPorts);
            auto formatPort = [&](const HostDiscoveryContext::PortEntry& pe) -> std::string {
                if (!pe.alias.empty() && pe.alias.find(':') != std::string::npos) {
                    return pe.alias;
                }
                if (pe.name.find(':') != std::string::npos) {
                    return pe.name;
                }
                return nName + ":" + pe.name;
            };
            ExternalPortInfo ep;
            ep.clientName = nName;
            ep.sendPortL = formatPort(b.inPorts[0]);
            ep.sendPortR = formatPort(b.inPorts[1]);
            ep.returnPortL = formatPort(b.outPorts[0]);
            ep.returnPortR = formatPort(b.outPorts[1]);
            ep.sampleRate = b.node.rate > 0.0 ? b.node.rate : defaultSampleRate;
            ep.bufferSize = b.node.quantum > 0 ? b.node.quantum : defaultBufferSize;
            ep.reportedLatencyFrames = b.node.reportedLatency;
            ep.latencyReported = b.node.hasReportedLatency;
            result.push_back(std::move(ep));
        }
    }

    return result;
}

PipeWireInsertTransport::PipeWireInsertTransport()
    : m_coreListener(std::make_unique<struct spa_hook>())
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
    resetCommonTransportState(target, engineSampleRate, engineBufferSize);

    {
        std::lock_guard<std::mutex> lock(m_stateMutex);
        m_nodesById.clear();
        m_portsById.clear();
        m_watchedIds.clear();
        m_syncDone = false;
    }

    if (m_simulationMode.load(std::memory_order_acquire)) {
        return completeSimulationConnect();
    }

    // Pre-prime 1 block of silence into send ring to absorb cycle-ordering jitter between
    // desk32-playback and desk32-insert-send thread loops without startup underruns.
    const size_t primeFrames = std::min<size_t>(m_expectedBufferSize, m_pwSendTmpL.size());
    if (primeFrames > 0) {
        std::fill_n(m_pwSendTmpL.data(), primeFrames, 0.0);
        std::fill_n(m_pwSendTmpR.data(), primeFrames, 0.0);
        m_sendRing.writePlanar(m_pwSendTmpL.data(), m_pwSendTmpR.data(), primeFrames);
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

    char latencyBuf[64];
    std::snprintf(latencyBuf, sizeof(latencyBuf), "%zu/%u",
                  m_expectedBufferSize,
                  static_cast<uint32_t>(m_expectedSampleRate));

    // Create Send playback stream (no autoconnect — explicit 4-port Link routing only,
    // always-process so PipeWire schedules the insert loop even without a downstream HW sink).
    struct pw_properties* sendProps = pw_properties_new(
        PW_KEY_MEDIA_TYPE, "Audio",
        PW_KEY_MEDIA_CATEGORY, "Playback",
        PW_KEY_MEDIA_ROLE, "DSP",
        PW_KEY_APP_NAME, "Desk32",
        PW_KEY_NODE_NAME, "desk32-insert-send",
        PW_KEY_NODE_DESCRIPTION, "Desk32 External Insert Send",
        PW_KEY_NODE_AUTOCONNECT, "false",
        PW_KEY_NODE_ALWAYS_PROCESS, "true",
        PW_KEY_NODE_LATENCY, latencyBuf,
        nullptr
    );
    m_sendStream = pw_stream_new(m_core, "Desk32 Insert Send", sendProps);

    struct pw_properties* retProps = pw_properties_new(
        PW_KEY_MEDIA_TYPE, "Audio",
        PW_KEY_MEDIA_CATEGORY, "Capture",
        PW_KEY_MEDIA_ROLE, "DSP",
        PW_KEY_APP_NAME, "Desk32",
        PW_KEY_NODE_NAME, "desk32-insert-return",
        PW_KEY_NODE_DESCRIPTION, "Desk32 External Insert Return",
        PW_KEY_NODE_AUTOCONNECT, "false",
        PW_KEY_NODE_ALWAYS_PROCESS, "true",
        PW_KEY_NODE_LATENCY, latencyBuf,
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

    // Perform bounded registry roundtrip to resolve target 4 ports & self ports and establish 4 PW Links
    pw_thread_loop_lock(m_threadLoop);
    m_syncSeq = pw_core_sync(m_core, PW_ID_CORE, 0);
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(500);
    while (std::chrono::steady_clock::now() < deadline) {
        tryResolveAndCreateLinksLocked();
        if (m_syncDone && m_activeLinksCreated.load(std::memory_order_acquire) >= 4) {
            break;
        }
        struct timespec abstime{};
        pw_thread_loop_get_time(m_threadLoop, &abstime, 50 * SPA_NSEC_PER_MSEC);
        pw_thread_loop_timed_wait_full(m_threadLoop, &abstime);
    }
    tryResolveAndCreateLinksLocked();

    // Confirm created links with one bounded core sync roundtrip
    if (m_activeLinksCreated.load(std::memory_order_acquire) >= 4) {
        {
            std::lock_guard<std::mutex> lock(m_stateMutex);
            m_syncDone = false;
        }
        m_syncSeq = pw_core_sync(m_core, PW_ID_CORE, 0);
        const auto syncDeadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(100);
        while (!m_syncDone && std::chrono::steady_clock::now() < syncDeadline) {
            struct timespec abstime{};
            pw_thread_loop_get_time(m_threadLoop, &abstime, 25 * SPA_NSEC_PER_MSEC);
            pw_thread_loop_timed_wait_full(m_threadLoop, &abstime);
        }
    }

    const uint32_t linksCreated = m_activeLinksCreated.load(std::memory_order_acquire);
    const uint32_t portsResolved = m_resolvedTargetPorts.load(std::memory_order_acquire);
    const bool linkBroken = m_graphDisconnected.load(std::memory_order_acquire);
    std::string existingErr;
    {
        std::lock_guard<std::mutex> lock(m_stateMutex);
        existingErr = m_errorDetail;
    }
    pw_thread_loop_unlock(m_threadLoop);

    if (linksCreated < 4 || linkBroken) {
        std::string msg = existingErr.empty()
                              ? ("Target External Insert ports not found or link creation incomplete (resolvedPorts=" +
                                 std::to_string(portsResolved) + ", links=" + std::to_string(linksCreated) + ")")
                              : existingErr;
        failAndCleanup(msg);
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
    auto safeRemoveHook = [](struct spa_hook* hook) {
        if (hook && hook->link.next && hook->link.prev) {
            spa_hook_remove(hook);
        }
    };

    for (auto& binding : m_linkBindings) {
        safeRemoveHook(binding.listener.get());
        if (binding.proxy) {
            pw_proxy_destroy(binding.proxy);
        }
    }
    m_linkBindings.clear();

    safeRemoveHook(m_sendListener.get());
    safeRemoveHook(m_returnListener.get());
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

    safeRemoveHook(m_registryListener.get());
    if (m_registry) {
        pw_proxy_destroy(reinterpret_cast<struct pw_proxy*>(m_registry));
        m_registry = nullptr;
    }
    m_registryListener = std::make_unique<struct spa_hook>();

    safeRemoveHook(m_coreListener.get());
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

void PipeWireInsertTransport::tryResolveAndCreateLinksLocked()
{
    bool createdAllNow = false;
    {
        std::lock_guard<std::mutex> lock(m_stateMutex);
        if (!m_core || !m_linkBindings.empty()) {
            return;
        }

        auto findTargetPort = [&](const std::string& spec, const char* expectedDir) -> const DiscoveredPort* {
            if (spec.empty()) return nullptr;
            const std::string specLower = toLowerAscii(spec);
            const std::string clientLower = toLowerAscii(m_targetConfig.clientName);

            for (const auto& [pid, port] : m_portsById) {
                (void)pid;
                if (!expectedDir || port.direction != expectedDir) continue;
                auto nodeIt = m_nodesById.find(port.nodeId);
                const std::string nodeName = (nodeIt != m_nodesById.end()) ? nodeIt->second.name : std::string{};
                const std::string appName = (nodeIt != m_nodesById.end()) ? nodeIt->second.appName : std::string{};
                const std::string fullColon = nodeName.empty() ? port.name : (nodeName + ":" + port.name);
                const std::string appColon = appName.empty() ? port.name : (appName + ":" + port.name);

                if (port.alias == spec || fullColon == spec || appColon == spec ||
                    ((nodeName == m_targetConfig.clientName || appName == m_targetConfig.clientName) && port.name == spec)) {
                    return &port;
                }
                // Case-insensitive fallback if exact case didn't match (e.g. "Carla" vs "carla")
                if (toLowerAscii(port.alias) == specLower || toLowerAscii(fullColon) == specLower ||
                    toLowerAscii(appColon) == specLower ||
                    ((toLowerAscii(nodeName) == clientLower || toLowerAscii(appName) == clientLower) &&
                     toLowerAscii(port.name) == specLower)) {
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
            (void)pid;
            if (sendNid > 0 && port.nodeId == sendNid && port.direction == "out") {
                selfSendOuts.push_back(&port);
            }
            if (retNid > 0 && port.nodeId == retNid && port.direction == "in") {
                selfRetIns.push_back(&port);
            }
        }

        auto sortSelfPorts = [](std::vector<const DiscoveredPort*>& list) {
            std::stable_sort(list.begin(), list.end(), [](const DiscoveredPort* a, const DiscoveredPort* b) {
                const int ra = channelRankHint(a->channel, a->name);
                const int rb = channelRankHint(b->channel, b->name);
                if (ra != rb) return ra < rb;
                if (a->portOrder != b->portOrder) return a->portOrder < b->portOrder;
                return a->id < b->id;
            });
        };
        sortSelfPorts(selfSendOuts);
        sortSelfPorts(selfRetIns);

        if (!targetInL || !targetInR || !targetOutL || !targetOutR ||
            selfSendOuts.size() < 2 || selfRetIns.size() < 2) {
            return;
        }

        static struct pw_proxy_events linkProxyEvents{};
        linkProxyEvents.version = PW_VERSION_PROXY_EVENTS;
        linkProxyEvents.removed = onLinkRemovedProxy;
        linkProxyEvents.error = onLinkErrorProxy;

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
            LinkBinding binding;
            binding.proxy = proxy;
            binding.listener = std::make_unique<struct spa_hook>();
            pw_proxy_add_listener(proxy, binding.listener.get(), &linkProxyEvents, this);
            m_linkBindings.push_back(std::move(binding));
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
            createdAllNow = true;
        }
    }

    if (createdAllNow && m_threadLoop) {
        pw_thread_loop_signal(m_threadLoop, false);
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
            DiscoveredNode node;
            if (const char* name = spa_dict_lookup(props, PW_KEY_NODE_NAME)) {
                node.name = name;
            }
            if (const char* app = spa_dict_lookup(props, PW_KEY_APP_NAME)) {
                node.appName = app;
            }
            if (const char* nick = spa_dict_lookup(props, PW_KEY_NODE_NICK)) {
                node.nick = nick;
            }
            if (node.name.empty()) {
                node.name = !node.appName.empty() ? node.appName : node.nick;
            }
            if (!node.name.empty() || !node.appName.empty()) {
                self->m_nodesById[id] = std::move(node);
            }
        } else if (std::strcmp(type, PW_TYPE_INTERFACE_Port) == 0) {
            DiscoveredPort p;
            p.id = id;
            if (const char* nid = spa_dict_lookup(props, PW_KEY_NODE_ID)) {
                p.nodeId = static_cast<uint32_t>(std::strtoul(nid, nullptr, 10));
            }
            if (const char* pid = spa_dict_lookup(props, PW_KEY_PORT_ID)) {
                p.portOrder = static_cast<uint32_t>(std::strtoul(pid, nullptr, 10));
            } else {
                p.portOrder = id;
            }
            if (const char* pname = spa_dict_lookup(props, PW_KEY_PORT_NAME)) {
                p.name = pname;
            }
            if (const char* palias = spa_dict_lookup(props, PW_KEY_PORT_ALIAS)) {
                p.alias = palias;
            }
            if (const char* pchan = spa_dict_lookup(props, PW_KEY_AUDIO_CHANNEL)) {
                p.channel = pchan;
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

    if (watchedRemoved) {
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
    if (st == PW_STREAM_STATE_ERROR ||
        (st == PW_STREAM_STATE_UNCONNECTED && self->m_connected.load(std::memory_order_acquire))) {
        self->m_graphDisconnected.store(true, std::memory_order_release);
        self->m_connected.store(false, std::memory_order_release);
        if (error) {
            std::lock_guard<std::mutex> lock(self->m_stateMutex);
            self->m_errorDetail = std::string("PipeWire insert send stream error: ") + error;
        }
    } else if (st == PW_STREAM_STATE_PAUSED || st == PW_STREAM_STATE_STREAMING) {
        self->tryResolveAndCreateLinksLocked();
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
    if (st == PW_STREAM_STATE_ERROR ||
        (st == PW_STREAM_STATE_UNCONNECTED && self->m_connected.load(std::memory_order_acquire))) {
        self->m_graphDisconnected.store(true, std::memory_order_release);
        self->m_connected.store(false, std::memory_order_release);
        if (error) {
            std::lock_guard<std::mutex> lock(self->m_stateMutex);
            self->m_errorDetail = std::string("PipeWire insert return stream error: ") + error;
        }
    } else if (st == PW_STREAM_STATE_PAUSED || st == PW_STREAM_STATE_STREAMING) {
        self->tryResolveAndCreateLinksLocked();
    }
}

void PipeWireInsertTransport::onLinkProxyRemoved(void* data)
{
    auto* self = static_cast<PipeWireInsertTransport*>(data);
    if (!self) return;
    {
        std::lock_guard<std::mutex> lock(self->m_stateMutex);
        self->m_errorDetail = "PipeWire External Insert link removed from graph";
    }
    self->m_graphDisconnected.store(true, std::memory_order_release);
    self->m_connected.store(false, std::memory_order_release);
    if (self->m_threadLoop) {
        pw_thread_loop_signal(self->m_threadLoop, false);
    }
}

void PipeWireInsertTransport::onLinkProxyError(void* data, int /*seq*/, int /*res*/, const char* message)
{
    auto* self = static_cast<PipeWireInsertTransport*>(data);
    if (!self) return;
    {
        std::lock_guard<std::mutex> lock(self->m_stateMutex);
        self->m_errorDetail = message ? (std::string("PipeWire External Insert link error: ") + message)
                                      : std::string("PipeWire External Insert link error");
    }
    self->m_graphDisconnected.store(true, std::memory_order_release);
    self->m_connected.store(false, std::memory_order_release);
    if (self->m_threadLoop) {
        pw_thread_loop_signal(self->m_threadLoop, false);
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
            {
                std::lock_guard<std::mutex> lock(self->m_stateMutex);
                self->m_errorDetail = "PipeWire send stream rate mismatch (" + std::to_string(raw.rate) + " Hz)";
            }
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
            {
                std::lock_guard<std::mutex> lock(self->m_stateMutex);
                self->m_errorDetail = "PipeWire return stream rate mismatch (" + std::to_string(raw.rate) + " Hz)";
            }
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
