#include "JackInsertTransport.hpp"

#include "engine/backend/AudioControlTypes.hpp"

#include <jack/jack.h>

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <mutex>
#include <vector>

namespace desk32::engine::insert {

namespace {

std::mutex g_jackInsertEnvMutex;

/// Scoped RAII helper setting PipeWire-JACK latency hint around jack_client_open()
/// so desk32-insert requests the same quantum as Desk32's main audio engine.
class ScopedJackInsertEnvHint {
public:
    ScopedJackInsertEnvHint(double sampleRate, size_t bufferFrames)
        : m_lock(g_jackInsertEnvMutex) {
        if (const char* oldLat = std::getenv("PIPEWIRE_LATENCY")) {
            m_hadLatency = true;
            m_oldLatency = oldLat;
        }
        char latBuf[64];
        std::snprintf(latBuf, sizeof(latBuf), "%zu/%u",
                      bufferFrames > 0 ? bufferFrames : size_t(256),
                      sampleRate > 0.0 ? static_cast<uint32_t>(std::llround(sampleRate)) : 48000u);
        ::setenv("PIPEWIRE_LATENCY", latBuf, 1);
    }

    ~ScopedJackInsertEnvHint() {
        if (m_hadLatency) {
            ::setenv("PIPEWIRE_LATENCY", m_oldLatency.c_str(), 1);
        } else {
            ::unsetenv("PIPEWIRE_LATENCY");
        }
    }

private:
    std::lock_guard<std::mutex> m_lock;
    bool m_hadLatency = false;
    std::string m_oldLatency;
};

std::string toLowerAscii(std::string s)
{
    for (char& c : s) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    return s;
}

bool isSelfJackClient(const std::string& clientPrefix)
{
    const std::string low = toLowerAscii(clientPrefix);
    return low == "desk32" || low.rfind("desk32-", 0) == 0;
}

std::string resolveJackTargetPortName(jack_client_t* client,
                                      const std::string& spec,
                                      const std::string& clientNameHint,
                                      int requiredFlags)
{
    if (!client || spec.empty()) {
        return {};
    }

    auto matches = [&](const char* full) -> bool {
        if (!full) return false;
        jack_port_t* p = jack_port_by_name(client, full);
        if (!p || (jack_port_flags(p) & requiredFlags) == 0) {
            return false;
        }
        return true;
    };

    if (matches(spec.c_str())) {
        return spec;
    }
    if (!clientNameHint.empty() && spec.find(':') == std::string::npos) {
        const std::string qualified = clientNameHint + ":" + spec;
        if (matches(qualified.c_str())) {
            return qualified;
        }
    }

    // Case-insensitive search across live ports matching direction
    const char** allPorts = jack_get_ports(
        client, nullptr, JACK_DEFAULT_AUDIO_TYPE,
        static_cast<unsigned long>(requiredFlags));
    if (!allPorts) {
        return {};
    }
    std::string found;
    const std::string specLow = toLowerAscii(spec);
    const std::string qualLow = (!clientNameHint.empty() && spec.find(':') == std::string::npos)
                                    ? toLowerAscii(clientNameHint + ":" + spec)
                                    : specLow;
    for (size_t i = 0; allPorts[i] != nullptr; ++i) {
        const std::string portStr = allPorts[i];
        const std::string portLow = toLowerAscii(portStr);
        if (portLow == specLow || portLow == qualLow) {
            found = portStr;
            break;
        }
    }
    jack_free(allPorts);
    return found;
}

} // namespace

std::vector<ExternalPortInfo> JackInsertTransport::discoverHosts(double defaultSampleRate,
                                                                 size_t defaultBufferSize)
{
    std::vector<ExternalPortInfo> result;

    jack_status_t st = static_cast<jack_status_t>(0);
    jack_client_t* client = jack_client_open("desk32-jack-insert-discover", JackNoStartServer, &st);
    if (!client) {
        return result;
    }

    const uint32_t srvRate = static_cast<uint32_t>(jack_get_sample_rate(client));
    const uint32_t srvBuf = static_cast<uint32_t>(jack_get_buffer_size(client));
    const double effRate = srvRate > 0 ? static_cast<double>(srvRate) : defaultSampleRate;
    const size_t effBuf = srvBuf > 0 ? static_cast<size_t>(srvBuf) : defaultBufferSize;

    const char** portNames = jack_get_ports(client, nullptr, JACK_DEFAULT_AUDIO_TYPE, 0);
    if (!portNames) {
        jack_client_close(client);
        return result;
    }

    struct ClientBucket {
        std::vector<std::string> inPorts;
        std::vector<std::string> outPorts;
        uint32_t reportedLatency = 0;
        bool hasReportedLatency = false;
    };
    std::map<std::string, ClientBucket> buckets;

    for (size_t i = 0; portNames[i] != nullptr; ++i) {
        const std::string full = portNames[i];
        const size_t colon = full.find(':');
        if (colon == std::string::npos || colon == 0) continue;
        const std::string clientPrefix = full.substr(0, colon);
        if (isSelfJackClient(clientPrefix)) continue;

        const std::string portSuffix = full.substr(colon + 1);
        std::string lo = portSuffix;
        std::transform(lo.begin(), lo.end(), lo.begin(), ::tolower);
        if (lo.find("midi") != std::string::npos ||
            lo.find("events-in") != std::string::npos ||
            lo.find("events_in") != std::string::npos ||
            lo.find("events-out") != std::string::npos ||
            lo.find("events_out") != std::string::npos ||
            lo.find("control") != std::string::npos) {
            continue;
        }

        jack_port_t* portObj = jack_port_by_name(client, full.c_str());
        if (!portObj) continue;
        const char* ptype = jack_port_type(portObj);
        if (!ptype || std::strcmp(ptype, JACK_DEFAULT_AUDIO_TYPE) != 0) continue;
        const int flags = jack_port_flags(portObj);
        auto& b = buckets[clientPrefix];
        if ((flags & JackPortIsInput) != 0) {
            b.inPorts.push_back(full);
        }
        if ((flags & JackPortIsOutput) != 0) {
            b.outPorts.push_back(full);
            jack_latency_range_t range{};
            jack_port_get_latency_range(portObj, JackCaptureLatency, &range);
            if (range.max > 0) {
                b.reportedLatency = std::max<uint32_t>(b.reportedLatency, range.max);
                b.hasReportedLatency = true;
            }
        }
    }
    jack_free(portNames);
    jack_client_close(client);

    auto sortPorts = [](std::vector<std::string>& list) {
        std::stable_sort(list.begin(), list.end(), [](const std::string& a, const std::string& b) {
            const int ra = backend::rankStereoChannelPortHint("", a);
            const int rb = backend::rankStereoChannelPortHint("", b);
            if (ra != rb) return ra < rb;
            return a < b;
        });
    };

    for (auto& [clientName, b] : buckets) {
        if (b.inPorts.size() >= 2 && b.outPorts.size() >= 2) {
            sortPorts(b.inPorts);
            sortPorts(b.outPorts);
            ExternalPortInfo ep;
            ep.clientName = clientName;
            ep.sendPortL = b.inPorts[0];
            ep.sendPortR = b.inPorts[1];
            ep.returnPortL = b.outPorts[0];
            ep.returnPortR = b.outPorts[1];
            ep.sampleRate = effRate;
            ep.bufferSize = effBuf;
            ep.reportedLatencyFrames = b.reportedLatency;
            ep.latencyReported = b.hasReportedLatency;
            result.push_back(std::move(ep));
        }
    }

    return result;
}

JackInsertTransport::JackInsertTransport() = default;

JackInsertTransport::~JackInsertTransport()
{
    disconnect();
}

bool JackInsertTransport::configureAndConnect(const ExternalPortInfo& target,
                                              double engineSampleRate,
                                              size_t engineBufferSize)
{
    disconnect();
    resetCommonTransportState(target, engineSampleRate, engineBufferSize);
    m_serverShutdown.store(false, std::memory_order_release);

    if (m_simulationMode.load(std::memory_order_acquire)) {
        return completeSimulationConnect();
    }

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

    jack_status_t st = static_cast<jack_status_t>(0);
    {
        ScopedJackInsertEnvHint envHint(m_expectedSampleRate, m_expectedBufferSize);
        m_client = jack_client_open("desk32-insert", JackNoStartServer, &st);
    }
    if (!m_client) {
        failAndCleanup("PipeWire-JACK server unavailable for External Insert");
        return false;
    }

    const uint32_t srvRate = static_cast<uint32_t>(jack_get_sample_rate(m_client));
    const uint32_t srvBuf = static_cast<uint32_t>(jack_get_buffer_size(m_client));
    m_negotiatedSendRate.store(srvRate, std::memory_order_release);
    m_negotiatedReturnRate.store(srvRate, std::memory_order_release);
    m_negotiatedQuantum.store(srvBuf, std::memory_order_release);

    if (srvRate > 0 && std::abs(static_cast<double>(srvRate) - m_expectedSampleRate) > 0.5) {
        m_rateMismatch.store(true, std::memory_order_release);
        failAndCleanup("JACK insert sample rate mismatch (" + std::to_string(srvRate) +
                       " Hz vs engine " + std::to_string(static_cast<uint32_t>(m_expectedSampleRate)) + " Hz)");
        return false;
    }
    if (srvBuf > 0 && static_cast<size_t>(srvBuf) > m_pwSendTmpL.size()) {
        m_bufferMismatch.store(true, std::memory_order_release);
        failAndCleanup("JACK insert buffer size exceeds ring capacity (" + std::to_string(srvBuf) +
                       " frames > max " + std::to_string(m_pwSendTmpL.size()) + ")");
        return false;
    }

    // Prime send ring with max(engineBufferSize, srvBuf) silence frames so differing
    // PipeWire-JACK quanta (e.g. engine 256 vs Carla/JACK 1024) run smoothly without underruns.
    const size_t targetPrime = std::min<size_t>(
        std::max<size_t>(m_expectedBufferSize, static_cast<size_t>(srvBuf)),
        m_pwSendTmpL.size());
    if (targetPrime > primeFrames) {
        const size_t extra = targetPrime - primeFrames;
        std::fill_n(m_pwSendTmpL.data(), extra, 0.0);
        std::fill_n(m_pwSendTmpR.data(), extra, 0.0);
        m_sendRing.writePlanar(m_pwSendTmpL.data(), m_pwSendTmpR.data(), extra);
    }

    jack_set_process_callback(m_client, &JackInsertTransport::onProcessCallback, this);
    jack_set_sample_rate_callback(m_client, &JackInsertTransport::onSampleRateCallback, this);
    jack_set_buffer_size_callback(m_client, &JackInsertTransport::onBufferSizeCallback, this);
    jack_set_port_registration_callback(m_client, &JackInsertTransport::onPortRegistrationCallback, this);
    jack_set_port_connect_callback(m_client, &JackInsertTransport::onPortConnectCallback, this);
    jack_on_shutdown(m_client, &JackInsertTransport::onShutdownCallback, this);

    m_sendPortL = jack_port_register(m_client, "send_L", JACK_DEFAULT_AUDIO_TYPE, JackPortIsOutput, 0);
    m_sendPortR = jack_port_register(m_client, "send_R", JACK_DEFAULT_AUDIO_TYPE, JackPortIsOutput, 0);
    m_returnPortL = jack_port_register(m_client, "return_L", JACK_DEFAULT_AUDIO_TYPE, JackPortIsInput, 0);
    m_returnPortR = jack_port_register(m_client, "return_R", JACK_DEFAULT_AUDIO_TYPE, JackPortIsInput, 0);
    if (!m_sendPortL || !m_sendPortR || !m_returnPortL || !m_returnPortR) {
        failAndCleanup("Failed to register 4 JACK insert Send/Return ports");
        return false;
    }

    const std::string resolvedInL = resolveJackTargetPortName(m_client, target.sendPortL, target.clientName, JackPortIsInput);
    const std::string resolvedInR = resolveJackTargetPortName(m_client, target.sendPortR, target.clientName, JackPortIsInput);
    const std::string resolvedOutL = resolveJackTargetPortName(m_client, target.returnPortL, target.clientName, JackPortIsOutput);
    const std::string resolvedOutR = resolveJackTargetPortName(m_client, target.returnPortR, target.clientName, JackPortIsOutput);

    uint32_t foundTargets = 0;
    if (!resolvedInL.empty()) ++foundTargets;
    if (!resolvedInR.empty()) ++foundTargets;
    if (!resolvedOutL.empty()) ++foundTargets;
    if (!resolvedOutR.empty()) ++foundTargets;
    m_resolvedTargetPorts.store(foundTargets, std::memory_order_release);

    if (foundTargets < 4) {
        failAndCleanup("Target JACK External Insert ports not found (resolved=" +
                       std::to_string(foundTargets) + "/4)");
        return false;
    }

    if (jack_activate(m_client) != 0) {
        failAndCleanup("jack_activate() failed for External Insert client");
        return false;
    }

    uint32_t linksCreated = 0;
    auto linkPair = [&](const char* srcPort, const char* dstPort) -> bool {
        const int rc = jack_connect(m_client, srcPort, dstPort);
        return (rc == 0 || rc == EEXIST);
    };
    if (linkPair(jack_port_name(m_sendPortL), resolvedInL.c_str())) ++linksCreated;
    if (linkPair(jack_port_name(m_sendPortR), resolvedInR.c_str())) ++linksCreated;
    if (linkPair(resolvedOutL.c_str(), jack_port_name(m_returnPortL))) ++linksCreated;
    if (linkPair(resolvedOutR.c_str(), jack_port_name(m_returnPortR))) ++linksCreated;
    m_activeLinksCreated.store(linksCreated, std::memory_order_release);

    if (linksCreated < 4) {
        failAndCleanup("Failed to establish all 4 JACK External Insert links (" +
                       std::to_string(linksCreated) + "/4)");
        return false;
    }

    m_connected.store(true, std::memory_order_release);
    return true;
}

void JackInsertTransport::disconnect()
{
    m_connected.store(false, std::memory_order_release);
    std::lock_guard<std::mutex> lock(m_stateMutex);
    if (m_client) {
        const bool serverDown = m_serverShutdown.load(std::memory_order_acquire);
        if (!serverDown) {
            jack_deactivate(m_client);
            if (m_sendPortL) jack_port_unregister(m_client, m_sendPortL);
            if (m_sendPortR) jack_port_unregister(m_client, m_sendPortR);
            if (m_returnPortL) jack_port_unregister(m_client, m_returnPortL);
            if (m_returnPortR) jack_port_unregister(m_client, m_returnPortR);
        }
        m_sendPortL = nullptr;
        m_sendPortR = nullptr;
        m_returnPortL = nullptr;
        m_returnPortR = nullptr;
        jack_client_close(m_client);
        m_client = nullptr;
    }
}

int JackInsertTransport::onProcessCallback(uint32_t nframes, void* arg)
{
    auto* self = static_cast<JackInsertTransport*>(arg);
    if (!self || !self->m_sendPortL || !self->m_sendPortR ||
        !self->m_returnPortL || !self->m_returnPortR) {
        return 0;
    }

    auto* sendL = static_cast<float*>(jack_port_get_buffer(self->m_sendPortL, static_cast<jack_nframes_t>(nframes)));
    auto* sendR = static_cast<float*>(jack_port_get_buffer(self->m_sendPortR, static_cast<jack_nframes_t>(nframes)));
    const auto* retL = static_cast<const float*>(jack_port_get_buffer(self->m_returnPortL, static_cast<jack_nframes_t>(nframes)));
    const auto* retR = static_cast<const float*>(jack_port_get_buffer(self->m_returnPortR, static_cast<jack_nframes_t>(nframes)));
    if (!sendL || !sendR || !retL || !retR) {
        return 0;
    }

    const size_t n = std::min<size_t>(static_cast<size_t>(nframes), self->m_pwSendTmpL.size());
    const size_t read = self->m_sendRing.readPlanar(self->m_pwSendTmpL.data(), self->m_pwSendTmpR.data(), n);
    for (size_t i = 0; i < read; ++i) {
        sendL[i] = static_cast<float>(self->m_pwSendTmpL[i]);
        sendR[i] = static_cast<float>(self->m_pwSendTmpR[i]);
    }
    if (read < nframes) {
        std::memset(sendL + read, 0, (static_cast<size_t>(nframes) - read) * sizeof(float));
        std::memset(sendR + read, 0, (static_cast<size_t>(nframes) - read) * sizeof(float));
    }

    const size_t nRet = std::min<size_t>(static_cast<size_t>(nframes), self->m_pwRetTmpL.size());
    for (size_t i = 0; i < nRet; ++i) {
        self->m_pwRetTmpL[i] = static_cast<double>(retL[i]);
        self->m_pwRetTmpR[i] = static_cast<double>(retR[i]);
    }
    self->m_returnRing.writePlanar(self->m_pwRetTmpL.data(), self->m_pwRetTmpR.data(), nRet);
    return 0;
}

int JackInsertTransport::onSampleRateCallback(uint32_t nframes, void* arg)
{
    auto* self = static_cast<JackInsertTransport*>(arg);
    if (!self || nframes == 0) return 0;
    self->m_negotiatedSendRate.store(nframes, std::memory_order_release);
    self->m_negotiatedReturnRate.store(nframes, std::memory_order_release);
    if (std::abs(static_cast<double>(nframes) - self->m_expectedSampleRate) > 0.5) {
        {
            std::lock_guard<std::mutex> lock(self->m_stateMutex);
            self->m_errorDetail = "JACK insert sample rate mismatch (" + std::to_string(nframes) + " Hz)";
        }
        self->m_rateMismatch.store(true, std::memory_order_release);
        self->m_connected.store(false, std::memory_order_release);
    }
    return 0;
}

int JackInsertTransport::onBufferSizeCallback(uint32_t nframes, void* arg)
{
    auto* self = static_cast<JackInsertTransport*>(arg);
    if (!self || nframes == 0) return 0;
    self->m_negotiatedQuantum.store(nframes, std::memory_order_release);
    // Lock-free send/return ring buffers decouple Desk32's block size from PipeWire-JACK's
    // dynamically negotiated quantum; only reject if nframes exceeds scratch buffer capacity.
    if (static_cast<size_t>(nframes) > self->m_pwSendTmpL.size()) {
        {
            std::lock_guard<std::mutex> lock(self->m_stateMutex);
            self->m_errorDetail = "JACK insert buffer size exceeds max (" + std::to_string(nframes) +
                                  " > " + std::to_string(self->m_pwSendTmpL.size()) + " frames)";
        }
        self->m_bufferMismatch.store(true, std::memory_order_release);
        self->m_connected.store(false, std::memory_order_release);
    }
    return 0;
}

void JackInsertTransport::onPortRegistrationCallback(uint32_t /*portId*/, int registered, void* arg)
{
    auto* self = static_cast<JackInsertTransport*>(arg);
    if (!self || registered != 0 || !self->m_connected.load(std::memory_order_acquire) || !self->m_client) {
        return;
    }
    const bool anyMissing =
        (self->m_sendPortL && jack_port_connected(self->m_sendPortL) == 0) ||
        (self->m_sendPortR && jack_port_connected(self->m_sendPortR) == 0) ||
        (self->m_returnPortL && jack_port_connected(self->m_returnPortL) == 0) ||
        (self->m_returnPortR && jack_port_connected(self->m_returnPortR) == 0);
    if (anyMissing) {
        {
            std::lock_guard<std::mutex> lock(self->m_stateMutex);
            self->m_errorDetail = "Target JACK External Insert port unregistered from graph";
        }
        self->m_graphDisconnected.store(true, std::memory_order_release);
        self->m_connected.store(false, std::memory_order_release);
    }
}

void JackInsertTransport::onPortConnectCallback(uint32_t a, uint32_t b, int connect, void* arg)
{
    auto* self = static_cast<JackInsertTransport*>(arg);
    if (!self || connect != 0 || !self->m_connected.load(std::memory_order_acquire) || !self->m_client) {
        return;
    }
    jack_port_t* pa = jack_port_by_id(self->m_client, static_cast<jack_port_id_t>(a));
    jack_port_t* pb = jack_port_by_id(self->m_client, static_cast<jack_port_id_t>(b));
    const bool touchesUs =
        (pa && jack_port_is_mine(self->m_client, pa) != 0) ||
        (pb && jack_port_is_mine(self->m_client, pb) != 0);
    if (touchesUs) {
        {
            std::lock_guard<std::mutex> lock(self->m_stateMutex);
            self->m_errorDetail = "JACK External Insert link disconnected from graph";
        }
        self->m_graphDisconnected.store(true, std::memory_order_release);
        self->m_connected.store(false, std::memory_order_release);
    }
}

void JackInsertTransport::onShutdownCallback(void* arg)
{
    auto* self = static_cast<JackInsertTransport*>(arg);
    if (!self) return;
    self->m_serverShutdown.store(true, std::memory_order_release);
    {
        std::lock_guard<std::mutex> lock(self->m_stateMutex);
        self->m_errorDetail = "PipeWire-JACK server shut down";
    }
    self->m_graphDisconnected.store(true, std::memory_order_release);
    self->m_connected.store(false, std::memory_order_release);
}

} // namespace desk32::engine::insert
