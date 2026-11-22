#pragma once

#include <string>
#include <vector>
#include <atomic>
#include <cmath>
#include <chrono>
#include <algorithm>
#include <mutex>
#include <functional>
#include <memory>
#include <QProcess>
#include <QJsonDocument>
#include <QJsonArray>
#include <QJsonObject>
#include <QString>

namespace desk32::engine::insert {

enum class InsertState {
    NotConfigured,
    HostNotFound,
    PortsFound,
    Connected,
    Active,
    RateMismatch,
    BufferMismatch,
    XRun,
    Disconnected,
    Recovering,
    Error
};

enum class LatencySource {
    Uncalibrated,
    ReportedStatic,
    SimulatedMock,
    HardwareMeasured
};

enum class DisconnectPolicy : uint8_t {
    AutoBypass = 0,
    MuteOutput = 1,
    StopPlayback = 2
};

/// Spec §7 Latency Alignment measurement stability / confidence level
enum class LatencyConfidence : uint8_t {
    Unmeasured = 0,
    High = 1,
    Unstable = 2,
    LowSnr = 3
};

/// Spec §7 2-step Latency Calibration candidate awaiting user confirmation
struct LatencyCalibrationCandidate {
    bool sessionActive = false;
    bool hasMeasurement = false;
    bool confirmed = false;
    int64_t recommendedFrames = -1;
    int64_t minFrames = -1;
    int64_t maxFrames = -1;
    int64_t spreadFrames = 0;
    double peakSnrDb = 0.0;
    uint32_t sampleCount = 0;
    LatencyConfidence confidence = LatencyConfidence::Unmeasured;
    std::string summary;
};

struct ExternalPortInfo {
    std::string clientName;
    std::string sendPortL;
    std::string sendPortR;
    std::string returnPortL;
    std::string returnPortR;
    double sampleRate = 48000.0;
    size_t bufferSize = 256;
    uint32_t reportedLatencyFrames = 0;
    bool latencyReported = true;
    std::string pluginName{};
    std::string pluginUri{};
    std::string hostPresetReference{};
};

struct InsertTransportDiagnostics {
    bool connected = false;
    bool simulationMode = false;
    uint32_t sendNodeId = 0;
    uint32_t returnNodeId = 0;
    uint32_t resolvedTargetPorts = 0;
    uint32_t activeLinksCreated = 0;
    uint64_t sendBlocksWritten = 0;
    uint64_t returnBlocksRead = 0;
    uint64_t xrunOrUnderruns = 0;
    uint32_t negotiatedSendRate = 0;
    uint32_t negotiatedReturnRate = 0;
    uint32_t negotiatedQuantum = 0;
    bool rateMismatchDetected = false;
    bool bufferMismatchDetected = false;
    bool graphDisconnected = false;
    std::string errorDetail;
};

/// Abstract real-time transport for External Insert (implemented by PipeWireInsertTransport).
/// Kept free of PipeWire headers so ExternalInsertBridge remains portable for non-PipeWire tests.
class IExternalInsertTransport {
public:
    virtual ~IExternalInsertTransport() = default;

    virtual bool configureAndConnect(const ExternalPortInfo& target,
                                     double engineSampleRate,
                                     size_t engineBufferSize) = 0;
    virtual void disconnect() = 0;
    [[nodiscard]] virtual bool isConnected() const noexcept = 0;

    /// Real-time thread call: strictly zero heap allocations and zero blocking locks.
    virtual bool exchangeBlock(double* inOutLeft, double* inOutRight, size_t frames) noexcept = 0;

    [[nodiscard]] virtual bool hasRateMismatch() const noexcept = 0;
    [[nodiscard]] virtual bool hasBufferMismatch() const noexcept = 0;
    [[nodiscard]] virtual bool consumeGraphDisconnectEvent() noexcept = 0;
    [[nodiscard]] virtual InsertTransportDiagnostics diagnostics() const = 0;
};

class ExternalInsertBridge {
public:
    using MockProcessorFunc = std::function<void(double*, double*, size_t)>;

    struct MockHolder {
        MockProcessorFunc func;
        MockHolder* nextRetired = nullptr;
    };

    ExternalInsertBridge() = default;

    ~ExternalInsertBridge() {
        if (m_transport) {
            m_rtTransport.store(nullptr, std::memory_order_release);
            m_transport->disconnect();
        }
        reclaimRetiredMocks();
        delete m_preparedMock.exchange(nullptr, std::memory_order_acquire);
        delete m_activeMock;
    }

    ExternalInsertBridge(const ExternalInsertBridge&) = delete;
    ExternalInsertBridge& operator=(const ExternalInsertBridge&) = delete;

    [[nodiscard]] InsertState state() const noexcept { return m_state.load(std::memory_order_acquire); }
    [[nodiscard]] bool isBypassed() const noexcept { return m_isBypassed.load(std::memory_order_acquire); }
    [[nodiscard]] uint32_t reportedLatencyFrames() const noexcept { return m_reportedLatencyFrames.load(std::memory_order_acquire); }
    [[nodiscard]] bool hasReportedLatency() const noexcept { return m_latencyReported.load(std::memory_order_acquire); }
    [[nodiscard]] std::string reportedLatencyText() const {
        if (!m_latencyReported.load(std::memory_order_acquire)) {
            return "Not Reported";
        }
        return std::to_string(m_reportedLatencyFrames.load(std::memory_order_acquire)) + " frames";
    }
    [[nodiscard]] DisconnectPolicy disconnectPolicy() const noexcept {
        return m_disconnectPolicy.load(std::memory_order_acquire);
    }
    void setDisconnectPolicy(DisconnectPolicy policy) noexcept {
        m_disconnectPolicy.store(policy, std::memory_order_release);
    }
    [[nodiscard]] bool stopPlaybackRequested() const noexcept {
        return m_stopPlaybackRequested.load(std::memory_order_acquire);
    }
    void clearStopPlaybackRequested() noexcept {
        m_stopPlaybackRequested.store(false, std::memory_order_release);
    }
    [[nodiscard]] double measuredLatencyMs() const noexcept { return m_measuredLatencyMs.load(std::memory_order_acquire); }
    [[nodiscard]] int64_t measuredLatencyFrames() const noexcept { return m_measuredLatencyFrames.load(std::memory_order_acquire); }
    [[nodiscard]] LatencySource latencySource() const noexcept { return m_latencySource.load(std::memory_order_acquire); }
    [[nodiscard]] uint32_t xrunCount() const noexcept { return m_xrunCount.load(std::memory_order_relaxed); }
    [[nodiscard]] uint64_t impulseSendFrame() const noexcept { return m_impulseSendFrame.load(std::memory_order_relaxed); }

    [[nodiscard]] std::string lastError() const {
        std::lock_guard<std::mutex> lock(m_configMutex);
        return m_lastError;
    }

    [[nodiscard]] ExternalPortInfo config() const {
        std::lock_guard<std::mutex> lock(m_configMutex);
        return m_config;
    }

    static const char* stateToString(InsertState s) noexcept {
        switch (s) {
            case InsertState::NotConfigured: return "Not Configured";
            case InsertState::HostNotFound: return "Host Not Found";
            case InsertState::PortsFound: return "Ports Found";
            case InsertState::Connected: return "Connected";
            case InsertState::Active: return "Active";
            case InsertState::RateMismatch: return "Rate Mismatch";
            case InsertState::BufferMismatch: return "Buffer Mismatch";
            case InsertState::XRun: return "XRUN / Dropout";
            case InsertState::Disconnected: return "Disconnected";
            case InsertState::Recovering: return "Recovering";
            case InsertState::Error: return "Error";
        }
        return "Unknown";
    }

    static const char* latencySourceToString(LatencySource src) noexcept {
        switch (src) {
            case LatencySource::Uncalibrated: return "Uncalibrated";
            case LatencySource::ReportedStatic: return "Host Reported (Static)";
            case LatencySource::SimulatedMock: return "Simulated Mock";
            case LatencySource::HardwareMeasured: return "Hardware Measured (Physical)";
        }
        return "Unknown";
    }

    void setBypass(bool bypass) noexcept {
        m_isBypassed.store(bypass, std::memory_order_release);
    }

    void recordXRun() noexcept {
        m_xrunCount.fetch_add(1, std::memory_order_relaxed);
        m_state.store(InsertState::XRun, std::memory_order_release);
    }

    void resetXRunCount() noexcept {
        m_xrunCount.store(0, std::memory_order_relaxed);
    }

    /// Set mock processing function for testing or simulated plugins (lock-free Prepared Instance Exchange, P0-21/P0-27)
    /// Strictly zero shared_ptr atomic spinlocks and zero Audio Thread deallocations.
    void setMockProcessor(MockProcessorFunc processor) {
        auto next = std::make_unique<MockHolder>();
        next->func = std::move(processor);
        MockHolder* unconsumed = nullptr;
        {
            std::lock_guard<std::mutex> lock(m_configMutex);
            unconsumed = m_preparedMock.exchange(next.release(), std::memory_order_acq_rel);
        }
        delete unconsumed;
        reclaimRetiredMocks();
    }

    /// Reclaim retired MockHolder instances strictly on Control Thread (P0-21/P0-27)
    void reclaimRetiredMocks() noexcept {
        MockHolder* cur = m_retiredMockHead.exchange(nullptr, std::memory_order_acquire);
        while (cur) {
            MockHolder* next = cur->nextRetired;
            delete cur;
            cur = next;
        }
    }

    void setDelayCompensationFrames(size_t frames) noexcept {
        m_delayCompensationFrames.store(static_cast<int64_t>(frames), std::memory_order_release);
    }

    [[nodiscard]] int64_t delayCompensationFrames() const noexcept {
        return m_delayCompensationFrames.load(std::memory_order_acquire);
    }

    void setTransport(std::shared_ptr<IExternalInsertTransport> transport) {
        std::lock_guard<std::mutex> lock(m_configMutex);
        if (m_transport && m_transport != transport) {
            m_rtTransport.store(nullptr, std::memory_order_release);
            m_transport->disconnect();
        }
        m_transport = std::move(transport);
        m_rtTransport.store(m_transport.get(), std::memory_order_release);
    }

    [[nodiscard]] std::shared_ptr<IExternalInsertTransport> transport() const {
        std::lock_guard<std::mutex> lock(m_configMutex);
        return m_transport;
    }

    void setRequireLiveTransport(bool requireLive) noexcept {
        m_requireLiveTransport.store(requireLive, std::memory_order_release);
    }
    [[nodiscard]] bool requireLiveTransport() const noexcept {
        return m_requireLiveTransport.load(std::memory_order_acquire);
    }

    [[nodiscard]] bool isTransportLiveConnected() const noexcept {
        auto* tr = m_rtTransport.load(std::memory_order_acquire);
        return tr && tr->isConnected();
    }

    [[nodiscard]] InsertTransportDiagnostics transportDiagnostics() const {
        std::lock_guard<std::mutex> lock(m_configMutex);
        if (!m_transport) {
            return {};
        }
        return m_transport->diagnostics();
    }

    [[nodiscard]] static const char* latencyConfidenceToString(LatencyConfidence c) noexcept {
        switch (c) {
            case LatencyConfidence::Unmeasured: return "Unmeasured";
            case LatencyConfidence::High: return "High";
            case LatencyConfidence::Unstable: return "Unstable";
            case LatencyConfidence::LowSnr: return "LowSnr";
        }
        return "Unmeasured";
    }

    /// Spec §7 Step 1: Start multi-sample latency calibration session without auto-applying to PDC.
    void beginLatencyCalibrationSession() {
        std::lock_guard<std::mutex> lock(m_calMutex);
        m_calSamples.clear();
        m_minCalSnrDb = 999.0;
        m_calCandidate = LatencyCalibrationCandidate{};
        m_calCandidate.sessionActive = true;
        m_calCandidate.summary = "Calibration session started; collecting round-trip impulse samples.";
    }

    /// Record one measured round-trip delay sample and evaluate jitter/SNR stability.
    bool recordLatencyCalibrationSample(int64_t measuredFrames, double peakSnrDb) {
        std::lock_guard<std::mutex> lock(m_calMutex);
        if (!m_calCandidate.sessionActive) {
            m_calCandidate.sessionActive = true;
        }
        if (measuredFrames < 0) {
            return false;
        }
        m_calSamples.push_back(measuredFrames);
        if (std::isfinite(peakSnrDb)) {
            m_minCalSnrDb = std::min(m_minCalSnrDb, peakSnrDb);
        } else {
            m_minCalSnrDb = 0.0;
        }
        recomputeCalibrationCandidateLocked();
        return m_calCandidate.confidence == LatencyConfidence::High;
    }

    /// Analyze a returned buffer block for an expected impulse sent at `expectedSendIdx` and record sample.
    bool measureCalibrationSampleFromBuffers(const double* returnL,
                                             const double* returnR,
                                             size_t frames,
                                             size_t expectedSendIdx,
                                             double noiseFloorRms = 1e-6)
    {
        if (!returnL || !returnR || frames == 0 || expectedSendIdx >= frames) {
            return false;
        }
        double peakAmp = 0.0;
        size_t peakIdx = expectedSendIdx;
        double residualSqSum = 0.0;
        size_t residualCount = 0;

        for (size_t i = expectedSendIdx; i < frames; ++i) {
            const double mag = std::max(std::abs(returnL[i]), std::abs(returnR[i]));
            if (mag > peakAmp) {
                peakAmp = mag;
                peakIdx = i;
            }
        }
        for (size_t i = 0; i < frames; ++i) {
            if (i == peakIdx) continue;
            residualSqSum += 0.5 * (returnL[i] * returnL[i] + returnR[i] * returnR[i]);
            ++residualCount;
        }

        const double measuredFloor = (residualCount > 0)
                                         ? std::sqrt(residualSqSum / static_cast<double>(residualCount))
                                         : noiseFloorRms;
        const double effectiveFloor = std::max(noiseFloorRms, measuredFloor);
        const double snrDb = (peakAmp > 1e-12)
                                 ? (20.0 * std::log10(peakAmp / std::max(1e-12, effectiveFloor)))
                                 : 0.0;
        const int64_t delayFrames = static_cast<int64_t>(peakIdx - expectedSendIdx);
        return recordLatencyCalibrationSample(delayFrames, snrDb);
    }

    /// Spec §7 Step 2: Explicit user confirmation applies recommended PDC only if confidence is High
    /// (unless `allowOverrideUnsafe == true` for explicit force override).
    bool confirmRecommendedLatency(bool allowOverrideUnsafe = false) {
        std::lock_guard<std::mutex> lock(m_calMutex);
        if (!m_calCandidate.hasMeasurement || m_calCandidate.recommendedFrames < 0) {
            return false;
        }
        if (m_calCandidate.confidence != LatencyConfidence::High && !allowOverrideUnsafe) {
            return false;
        }
        const size_t frames = static_cast<size_t>(m_calCandidate.recommendedFrames);
        const double sr = std::max(1.0, m_engineSampleRate.load(std::memory_order_acquire));
        m_delayCompensationFrames.store(static_cast<int64_t>(frames), std::memory_order_release);
        m_measuredLatencyFrames.store(m_calCandidate.recommendedFrames, std::memory_order_release);
        m_measuredLatencyMs.store((static_cast<double>(frames) / sr) * 1000.0, std::memory_order_release);
        m_latencySource.store(LatencySource::HardwareMeasured, std::memory_order_release);
        m_calCandidate.confirmed = true;
        m_calCandidate.sessionActive = false;
        m_calCandidate.summary = "Confirmed and applied " + std::to_string(frames) + " frames to DelayCompensationBuffer.";
        return true;
    }

    void cancelLatencyCalibrationSession() {
        std::lock_guard<std::mutex> lock(m_calMutex);
        m_calSamples.clear();
        m_minCalSnrDb = 999.0;
        m_calCandidate = LatencyCalibrationCandidate{};
    }

    [[nodiscard]] LatencyCalibrationCandidate latencyCalibrationCandidate() const {
        std::lock_guard<std::mutex> lock(m_calMutex);
        return m_calCandidate;
    }

    /// Configure external insert connection ports
    bool configure(const ExternalPortInfo& config, double engineSampleRate, size_t engineBufferSize) {
        std::lock_guard<std::mutex> lock(m_configMutex);
        m_config = config;
        m_engineSampleRate.store(engineSampleRate, std::memory_order_release);
        m_engineBufferSize.store(engineBufferSize, std::memory_order_release);
        m_reportedLatencyFrames.store(config.reportedLatencyFrames, std::memory_order_release);
        m_latencyReported.store(config.latencyReported, std::memory_order_release);
        m_stopPlaybackRequested.store(false, std::memory_order_release);
        if (config.latencyReported && config.reportedLatencyFrames > 0) {
            m_latencySource.store(LatencySource::ReportedStatic, std::memory_order_release);
        }

        if (m_config.clientName.empty()) {
            if (m_transport) m_transport->disconnect();
            m_state.store(InsertState::NotConfigured, std::memory_order_release);
            return false;
        }

        // 1. Feedback Loop Detection & Prevention:
        // Check if send port is identical to return port or self-referential
        if (m_config.sendPortL == m_config.returnPortL || m_config.sendPortR == m_config.returnPortR) {
            if (m_transport) m_transport->disconnect();
            m_state.store(InsertState::Error, std::memory_order_release);
            m_lastError = "Feedback loop detected: Send and Return ports are identical!";
            m_isBypassed.store(true, std::memory_order_release);
            return false;
        }

        // 2. Sample Rate & Buffer Matching Check
        if (std::abs(m_config.sampleRate - engineSampleRate) > 0.01) {
            if (m_transport) m_transport->disconnect();
            m_state.store(InsertState::RateMismatch, std::memory_order_release);
            m_lastError = "Sample rate mismatch between Engine and External Host";
            m_isBypassed.store(true, std::memory_order_release);
            return false;
        }

        if (m_config.bufferSize != engineBufferSize) {
            if (m_transport) m_transport->disconnect();
            m_state.store(InsertState::BufferMismatch, std::memory_order_release);
            m_lastError = "Buffer size mismatch between Engine and External Host";
            m_isBypassed.store(true, std::memory_order_release);
            return false;
        }

        if (m_transport) {
            const bool trOk = m_transport->configureAndConnect(m_config, engineSampleRate, engineBufferSize);
            if (!trOk && m_requireLiveTransport.load(std::memory_order_acquire)) {
                m_state.store(InsertState::HostNotFound, std::memory_order_release);
                m_lastError = m_transport->diagnostics().errorDetail;
                if (m_lastError.empty()) {
                    m_lastError = "Live external insert transport failed to connect target ports";
                }
                m_isBypassed.store(true, std::memory_order_release);
                return false;
            }
        }

        m_state.store(InsertState::Connected, std::memory_order_release);
        m_isBypassed.store(false, std::memory_order_release);
        m_lastError.clear();
        return true;
    }

    /// Control-thread PipeWire port discovery (queries `pw-dump` and merges registered test hosts).
    /// Also parses raw `pw-dump` JSON via parsePwDumpPorts for deterministic testing.
    static std::vector<ExternalPortInfo> parsePwDumpPorts(const std::string& pwDumpJson,
                                                          double defaultSampleRate = 48000.0,
                                                          size_t defaultBufferSize = 256)
    {
        std::vector<ExternalPortInfo> result;
        QJsonParseError err{};
        QJsonDocument doc = QJsonDocument::fromJson(QByteArray::fromStdString(pwDumpJson), &err);
        if (err.error != QJsonParseError::NoError || !doc.isArray()) {
            return result;
        }

        struct NodeBucket {
            std::string name;
            std::vector<std::string> inPorts;
            std::vector<std::string> outPorts;
            double rate = 0.0;
            size_t quantum = 0;
            uint32_t latency = 0;
            bool hasLatency = false;
        };
        std::vector<NodeBucket> nodes;
        auto findOrCreateNode = [&](int nodeId, const std::string& nodeName) -> NodeBucket& {
            for (auto& nb : nodes) {
                if (!nodeName.empty() && nb.name == nodeName) return nb;
            }
            NodeBucket b;
            b.name = nodeName.empty() ? ("node_" + std::to_string(nodeId)) : nodeName;
            nodes.push_back(std::move(b));
            return nodes.back();
        };

        std::vector<std::pair<int, std::string>> nodeNamesById;
        const QJsonArray arr = doc.array();
        for (const auto& val : arr) {
            if (!val.isObject()) continue;
            const QJsonObject obj = val.toObject();
            const QString type = obj["type"].toString();
            const int objId = obj["id"].toInt(-1);
            const QJsonObject info = obj["info"].toObject();
            const QJsonObject props = info["props"].toObject();

            if (type.contains("Node")) {
                std::string nName = props["node.name"].toString().toStdString();
                if (nName.empty()) nName = props["application.name"].toString().toStdString();
                if (!nName.empty() && objId >= 0) {
                    nodeNamesById.emplace_back(objId, nName);
                    auto& bucket = findOrCreateNode(objId, nName);
                    const QString lat = props["node.latency"].toString();
                    if (lat.contains('/')) {
                        const auto parts = lat.split('/');
                        bool okQ = false, okR = false;
                        size_t q = parts[0].toULongLong(&okQ);
                        double r = parts[1].toDouble(&okR);
                        if (okQ && q > 0) bucket.quantum = q;
                        if (okR && r > 0.0) bucket.rate = r;
                    }
                    if (props.contains("node.rate")) {
                        const QString nr = props["node.rate"].toString();
                        bool okR = false;
                        double r = nr.contains('/') ? nr.section('/', 1, 1).toDouble(&okR) : nr.toDouble(&okR);
                        if (okR && r > 0.0 && bucket.rate <= 0.0) bucket.rate = r;
                    }
                    if (props.contains("latency.internal.rate")) {
                        bucket.latency = static_cast<uint32_t>(std::max(0, props["latency.internal.rate"].toInt()));
                        bucket.hasLatency = true;
                    }
                }
            }
        }

        for (const auto& val : arr) {
            if (!val.isObject()) continue;
            const QJsonObject obj = val.toObject();
            const QString type = obj["type"].toString();
            if (!type.contains("Port")) continue;
            const QJsonObject info = obj["info"].toObject();
            const QJsonObject props = info["props"].toObject();
            const QString mediaType = props["media.type"].toString();
            if (!mediaType.isEmpty() && mediaType != "Audio") continue;

            const int nodeId = props["node.id"].toInt(-1);
            std::string nodeName;
            for (const auto& kv : nodeNamesById) {
                if (kv.first == nodeId) {
                    nodeName = kv.second;
                    break;
                }
            }
            QString portName = props["port.name"].toString();
            if (portName.isEmpty()) portName = props["port.alias"].toString();
            if (nodeName.empty() && portName.contains(':')) {
                nodeName = portName.section(':', 0, 0).toStdString();
            }
            if (nodeName.empty() || portName.isEmpty()) continue;

            const std::string fullPort = portName.contains(':')
                                             ? portName.toStdString()
                                             : (nodeName + ":" + portName.toStdString());
            auto& bucket = findOrCreateNode(nodeId, nodeName);
            const QString dir = info["direction"].toString().toLower();
            if (dir == "input" || dir == "in") {
                bucket.inPorts.push_back(fullPort);
            } else if (dir == "output" || dir == "out") {
                bucket.outPorts.push_back(fullPort);
            }
        }

        auto sortPortsNatural = [](std::vector<std::string>& ports) {
            auto rank = [](const std::string& p) -> int {
                std::string low = p;
                for (char& c : low) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
                if (low.find("fl") != std::string::npos || low.ends_with("_1") || low.ends_with("_l") || low.ends_with("left")) return 0;
                if (low.find("fr") != std::string::npos || low.ends_with("_2") || low.ends_with("_r") || low.ends_with("right")) return 1;
                return 2;
            };
            std::stable_sort(ports.begin(), ports.end(), [&](const std::string& a, const std::string& b) {
                const int ra = rank(a);
                const int rb = rank(b);
                if (ra != rb) return ra < rb;
                return a < b;
            });
        };

        for (auto& b : nodes) {
            if (b.name.rfind("desk32-", 0) == 0 || b.name == "Desk32") {
                continue;
            }
            if (b.inPorts.size() >= 2 && b.outPorts.size() >= 2) {
                sortPortsNatural(b.inPorts);
                sortPortsNatural(b.outPorts);
                ExternalPortInfo ep;
                ep.clientName = b.name;
                ep.sendPortL = b.inPorts[0];
                ep.sendPortR = b.inPorts[1];
                ep.returnPortL = b.outPorts[0];
                ep.returnPortR = b.outPorts[1];
                ep.sampleRate = b.rate > 0.0 ? b.rate : defaultSampleRate;
                ep.bufferSize = b.quantum > 0 ? b.quantum : defaultBufferSize;
                ep.reportedLatencyFrames = b.latency;
                ep.latencyReported = b.hasLatency;
                result.push_back(std::move(ep));
            }
        }
        return result;
    }

    static std::vector<ExternalPortInfo> discoverPorts(double currentSampleRate = 48000.0,
                                                       size_t currentBufferSize = 256)
    {
        QProcess proc;
        proc.start(QStringLiteral("pw-dump"), QStringList());
        if (!proc.waitForFinished(1000) || proc.exitCode() != 0) {
            return {};
        }
        const std::string out = proc.readAllStandardOutput().toStdString();
        return parsePwDumpPorts(out, currentSampleRate, currentBufferSize);
    }

    /// Disconnect external insert (and tear down live transport if attached)
    void handleDisconnect() {
        {
            std::lock_guard<std::mutex> lock(m_configMutex);
            std::string trDetail;
            if (m_transport) {
                trDetail = m_transport->diagnostics().errorDetail;
                m_transport->disconnect();
            }
            m_lastError = trDetail.empty()
                              ? "External host disconnected or port lost"
                              : ("External host disconnected: " + trDetail);
        }
        m_probeActive.store(false, std::memory_order_release);
        m_rtDisconnectNotice.store(false, std::memory_order_release);
        m_rtMismatchNotice.store(false, std::memory_order_release);
        m_state.store(InsertState::Disconnected, std::memory_order_release);
        // Activate hardware bypass (or mute/stop depending on DisconnectPolicy in process())
        m_isBypassed.store(true, std::memory_order_release);
    }

    /// Transition into RateMismatch / BufferMismatch / Error preserving transport diagnostic message
    void handleTransportMismatch(InsertState mismatchState, const std::string& detail) {
        {
            std::lock_guard<std::mutex> lock(m_configMutex);
            if (m_transport) {
                m_transport->disconnect();
            }
            m_lastError = detail;
        }
        m_probeActive.store(false, std::memory_order_release);
        m_rtDisconnectNotice.store(false, std::memory_order_release);
        m_rtMismatchNotice.store(false, std::memory_order_release);
        m_state.store(mismatchState, std::memory_order_release);
        m_isBypassed.store(true, std::memory_order_release);
    }

    [[nodiscard]] bool consumeRtDisconnectNotice() noexcept {
        return m_rtDisconnectNotice.exchange(false, std::memory_order_acq_rel);
    }

    [[nodiscard]] bool consumeRtMismatchNotice(bool& outIsRate) noexcept {
        if (m_rtMismatchNotice.exchange(false, std::memory_order_acq_rel)) {
            outIsRate = m_rtMismatchIsRate.load(std::memory_order_acquire);
            return true;
        }
        return false;
    }

    /// Simulate or verify port recovery against live discovered ports
    bool handleRecoveryWithVerification(const std::vector<ExternalPortInfo>& discoveredPorts) {
        m_state.store(InsertState::Recovering, std::memory_order_release);
        std::lock_guard<std::mutex> lock(m_configMutex);
        const double sr = m_engineSampleRate.load(std::memory_order_acquire);
        const size_t bs = m_engineBufferSize.load(std::memory_order_acquire);
        for (const auto& candidate : discoveredPorts) {
            if (candidate.clientName == m_config.clientName &&
                candidate.sendPortL == m_config.sendPortL &&
                candidate.sendPortR == m_config.sendPortR &&
                candidate.returnPortL == m_config.returnPortL &&
                candidate.returnPortR == m_config.returnPortR &&
                std::abs(candidate.sampleRate - sr) < 0.01 &&
                candidate.bufferSize == bs) {
                m_config.reportedLatencyFrames = candidate.reportedLatencyFrames;
                m_config.latencyReported = candidate.latencyReported;
                m_reportedLatencyFrames.store(candidate.reportedLatencyFrames, std::memory_order_release);
                m_latencyReported.store(candidate.latencyReported, std::memory_order_release);
                m_state.store(InsertState::Connected, std::memory_order_release);
                m_isBypassed.store(false, std::memory_order_release);
                m_stopPlaybackRequested.store(false, std::memory_order_release);
                m_lastError.clear();
                return true;
            }
        }
        m_state.store(InsertState::Error, std::memory_order_release);
        m_lastError = "Recovery failed: matching external ports not found or sample rate/buffer mismatch";
        return false;
    }

    /// Simulate port recovery event
    void handleRecovery() {
        m_state.store(InsertState::Recovering, std::memory_order_release);
        std::lock_guard<std::mutex> lock(m_configMutex);
        const double sr = m_engineSampleRate.load(std::memory_order_acquire);
        const size_t bs = m_engineBufferSize.load(std::memory_order_acquire);
        // Verify parameters
        if (std::abs(m_config.sampleRate - sr) < 0.01 &&
            m_config.bufferSize == bs) {
            m_state.store(InsertState::Connected, std::memory_order_release);
            m_isBypassed.store(false, std::memory_order_release);
            m_stopPlaybackRequested.store(false, std::memory_order_release);
            m_lastError.clear();
        } else {
            m_state.store(InsertState::Error, std::memory_order_release);
            m_lastError = "Recovery failed: incompatible configuration";
        }
    }

    /// Audio block processing through insert
    /// If bypassed or disconnected, applies DisconnectPolicy (AutoBypass dry pass-through, MuteOutput silence, StopPlayback flag + silence).
    /// Audio Thread execution: strictly zero dynamic allocations, zero deallocations, zero blocking locks (P0-21/P0-23/P0-27).
    void process(double* inOutLeft, double* inOutRight, size_t frames) noexcept {
        // 1. Activate prepared mock processor at block boundary if present
        if (auto* prepared = m_preparedMock.exchange(nullptr, std::memory_order_acq_rel)) {
            if (m_activeMock) {
                retireMock(m_activeMock);
            }
            m_activeMock = prepared;
        }

        // 1b. Monitor live PipeWire / transport graph disconnect & param mismatch events (§8.3)
        if (auto* tr = m_rtTransport.load(std::memory_order_acquire)) {
            if (tr->consumeGraphDisconnectEvent()) {
                m_state.store(InsertState::Disconnected, std::memory_order_release);
                m_isBypassed.store(true, std::memory_order_release);
                m_rtDisconnectNotice.store(true, std::memory_order_release);
            } else if (tr->hasRateMismatch()) {
                m_state.store(InsertState::RateMismatch, std::memory_order_release);
                m_isBypassed.store(true, std::memory_order_release);
                m_rtMismatchIsRate.store(true, std::memory_order_release);
                m_rtMismatchNotice.store(true, std::memory_order_release);
                return;
            } else if (tr->hasBufferMismatch()) {
                m_state.store(InsertState::BufferMismatch, std::memory_order_release);
                m_isBypassed.store(true, std::memory_order_release);
                m_rtMismatchIsRate.store(false, std::memory_order_release);
                m_rtMismatchNotice.store(true, std::memory_order_release);
                return;
            }
        }

        const InsertState curState = m_state.load(std::memory_order_acquire);
        if (curState == InsertState::Disconnected || curState == InsertState::Error) {
            const DisconnectPolicy policy = m_disconnectPolicy.load(std::memory_order_acquire);
            if (policy == DisconnectPolicy::MuteOutput) {
                std::fill_n(inOutLeft, frames, 0.0);
                std::fill_n(inOutRight, frames, 0.0);
                return;
            }
            if (policy == DisconnectPolicy::StopPlayback) {
                m_stopPlaybackRequested.store(true, std::memory_order_release);
                std::fill_n(inOutLeft, frames, 0.0);
                std::fill_n(inOutRight, frames, 0.0);
                return;
            }
            // AutoBypass: dry pass-through
            return;
        }

        if (m_isBypassed.load(std::memory_order_acquire) ||
            (curState != InsertState::Connected && curState != InsertState::Active)) {
            // Passthrough (dry)
            return;
        }

        // Transition Connected -> Active on first active block
        if (curState == InsertState::Connected) {
            m_state.store(InsertState::Active, std::memory_order_release);
        }

        const uint64_t baseFrame = m_totalFramesProcessed.load(std::memory_order_relaxed);
        const bool probeActive = m_probeActive.load(std::memory_order_acquire);
        const bool holdMute = m_calibrationHoldMute.load(std::memory_order_acquire);
        const bool measuring = m_measuringLatency.load(std::memory_order_acquire);
        const uint64_t targetFrame = m_impulseSendFrame.load(std::memory_order_relaxed);
        const bool hasMock = (m_activeMock && m_activeMock->func);
        IExternalInsertTransport* tr = m_rtTransport.load(std::memory_order_acquire);
        const bool hasLiveTransport = (!hasMock && tr && tr->isConnected());

        if (probeActive && (hasMock || hasLiveTransport)) {
            // Silent isolated impulse probe: prevents program audio from corrupting peak/SNR and keeps monitors silent.
            std::fill_n(inOutLeft, frames, 0.0);
            std::fill_n(inOutRight, frames, 0.0);
            const uint64_t sendFrame = m_probeSendFrame.load(std::memory_order_relaxed);
            for (size_t i = 0; i < frames; ++i) {
                if (baseFrame + i == sendFrame) {
                    inOutLeft[i] = 0.9;
                    inOutRight[i] = 0.9;
                }
            }

            bool processedOk = false;
            if (hasMock) {
                m_activeMock->func(inOutLeft, inOutRight, frames);
                processedOk = true;
            } else {
                processedOk = tr->exchangeBlock(inOutLeft, inOutRight, frames);
            }

            if (processedOk) {
                for (size_t i = 0; i < frames; ++i) {
                    const uint64_t f = baseFrame + i;
                    if (f > sendFrame) {
                        const double mag = std::max(std::abs(inOutLeft[i]), std::abs(inOutRight[i]));
                        const double energy = 0.5 * (inOutLeft[i] * inOutLeft[i] + inOutRight[i] * inOutRight[i]);
                        m_probeEnergySqSum += energy;
                        ++m_probeSampleCount;
                        if (mag > m_probePeakAmp) {
                            m_probePeakAmp = mag;
                            m_probePeakFrame = f;
                        }
                    }
                }
            }

            // Mute probe audio before sending downstream to speakers/headphones
            std::fill_n(inOutLeft, frames, 0.0);
            std::fill_n(inOutRight, frames, 0.0);

            const uint64_t endFrame = baseFrame + frames;
            const uint64_t captureEnd = m_probeCaptureEndFrame.load(std::memory_order_relaxed);
            const uint64_t timeoutEnd = m_probeTimeoutFrame.load(std::memory_order_relaxed);
            if ((endFrame >= captureEnd && m_probePeakAmp >= 0.05) || endFrame >= timeoutEnd) {
                if (m_probePeakAmp >= 0.05 && m_probePeakFrame > sendFrame) {
                    const int64_t delayFrames = static_cast<int64_t>(m_probePeakFrame - sendFrame);
                    const double peakSq = 0.5 * (m_probePeakAmp * m_probePeakAmp);
                    const double residualSq = std::max(0.0, m_probeEnergySqSum - peakSq);
                    const double residualCount = static_cast<double>(std::max<uint64_t>(1, m_probeSampleCount > 1 ? (m_probeSampleCount - 1) : 1));
                    const double noiseFloor = std::max(1e-7, std::sqrt(residualSq / residualCount));
                    const double snrDb = 20.0 * std::log10(m_probePeakAmp / noiseFloor);
                    const double sr = std::max(1.0, m_engineSampleRate.load(std::memory_order_acquire));

                    m_probeMeasuredFrames.store(delayFrames, std::memory_order_release);
                    m_probeMeasuredSnrDb.store(snrDb, std::memory_order_release);
                    m_measuredLatencyFrames.store(delayFrames, std::memory_order_release);
                    m_measuredLatencyMs.store((static_cast<double>(delayFrames) / sr) * 1000.0, std::memory_order_release);
                    m_latencySource.store(hasMock ? LatencySource::SimulatedMock : LatencySource::HardwareMeasured,
                                          std::memory_order_release);
                    m_probeTimedOut.store(false, std::memory_order_release);
                    m_probeResultReady.store(true, std::memory_order_release);
                } else {
                    m_probeTimedOut.store(true, std::memory_order_release);
                    m_probeResultReady.store(false, std::memory_order_release);
                }
                m_probeActive.store(false, std::memory_order_release);
            }
        } else if (holdMute && (hasMock || hasLiveTransport)) {
            // Keep flushing silence through external host during inter-probe intervals / pre-settle
            // so no program audio bursts leak into Carla or corrupt subsequent impulse probes.
            std::fill_n(inOutLeft, frames, 0.0);
            std::fill_n(inOutRight, frames, 0.0);
            if (hasMock) {
                m_activeMock->func(inOutLeft, inOutRight, frames);
            } else {
                (void)tr->exchangeBlock(inOutLeft, inOutRight, frames);
            }
            std::fill_n(inOutLeft, frames, 0.0);
            std::fill_n(inOutRight, frames, 0.0);
        } else {
            if (measuring && (hasMock || hasLiveTransport)) {
                // Inject impulse before external loopback processor/transport so loopback sees it
                for (size_t i = 0; i < frames; ++i) {
                    if (baseFrame + i == targetFrame) {
                        inOutLeft[i] += 1.0;
                        inOutRight[i] += 1.0;
                    }
                }
            }

            // Execute active mock processor or live transport without any locks or allocations on Audio Thread
            if (hasMock) {
                m_activeMock->func(inOutLeft, inOutRight, frames);
                if (measuring) {
                    // Automatic peak detection on returned buffer after send frame
                    for (size_t i = 0; i < frames; ++i) {
                        if (baseFrame + i > targetFrame &&
                            (std::abs(inOutLeft[i]) >= 0.5 || std::abs(inOutRight[i]) >= 0.5)) {
                            recordImpulseReturn(baseFrame + i, LatencySource::SimulatedMock);
                            break;
                        }
                    }
                }
            } else if (hasLiveTransport) {
                if (tr->exchangeBlock(inOutLeft, inOutRight, frames) && measuring) {
                    for (size_t i = 0; i < frames; ++i) {
                        if (baseFrame + i > targetFrame &&
                            (std::abs(inOutLeft[i]) >= 0.5 || std::abs(inOutRight[i]) >= 0.5)) {
                            recordImpulseReturn(baseFrame + i, LatencySource::HardwareMeasured);
                            break;
                        }
                    }
                }
            } else if (measuring) {
                // Legacy manual mode (no active processor attached): inject after dry check
                for (size_t i = 0; i < frames; ++i) {
                    if (baseFrame + i == targetFrame) {
                        inOutLeft[i] += 1.0;
                        inOutRight[i] += 1.0;
                    }
                }
            }
        }

        m_totalFramesProcessed.store(baseFrame + frames, std::memory_order_release);
    }

    /// Initiate RT-safe isolated hardware impulse probe (for live PipeWire/external host latency measurement)
    void startHardwareProbe(size_t captureWindowFrames = 8192, size_t maxTimeoutFrames = 48000) noexcept {
        m_probeActive.store(false, std::memory_order_release);
        m_probeResultReady.store(false, std::memory_order_release);
        m_probeTimedOut.store(false, std::memory_order_release);
        m_probeMeasuredFrames.store(-1, std::memory_order_release);
        m_probeMeasuredSnrDb.store(0.0, std::memory_order_release);
        m_probePeakAmp = 0.0;
        m_probePeakFrame = 0;
        m_probeEnergySqSum = 0.0;
        m_probeSampleCount = 0;
        const uint64_t curFrames = m_totalFramesProcessed.load(std::memory_order_acquire);
        const uint64_t sendAt = curFrames + 64;
        m_probeSendFrame.store(sendAt, std::memory_order_release);
        m_probeCaptureEndFrame.store(sendAt + std::max<uint64_t>(512, captureWindowFrames), std::memory_order_release);
        m_probeTimeoutFrame.store(sendAt + std::max<uint64_t>(captureWindowFrames, maxTimeoutFrames), std::memory_order_release);
        m_probeActive.store(true, std::memory_order_release);
    }

    void cancelHardwareProbe() noexcept {
        m_probeActive.store(false, std::memory_order_release);
        m_probeResultReady.store(false, std::memory_order_release);
        m_probeTimedOut.store(false, std::memory_order_release);
    }

    void setCalibrationHoldMute(bool hold) noexcept {
        m_calibrationHoldMute.store(hold, std::memory_order_release);
    }

    [[nodiscard]] bool isCalibrationHoldMute() const noexcept {
        return m_calibrationHoldMute.load(std::memory_order_acquire);
    }

    [[nodiscard]] bool isHardwareProbeActive() const noexcept {
        return m_probeActive.load(std::memory_order_acquire);
    }

    bool pollHardwareProbe(int64_t& outFrames, double& outSnrDb, bool& outTimedOut) noexcept {
        if (m_probeTimedOut.exchange(false, std::memory_order_acq_rel)) {
            outTimedOut = true;
            outFrames = -1;
            outSnrDb = 0.0;
            return true;
        }
        if (m_probeResultReady.exchange(false, std::memory_order_acq_rel)) {
            outTimedOut = false;
            outFrames = m_probeMeasuredFrames.load(std::memory_order_acquire);
            outSnrDb = m_probeMeasuredSnrDb.load(std::memory_order_acquire);
            return true;
        }
        return false;
    }

    /// Initiate round-trip latency measurement using an impulse signal
    void triggerLatencyMeasurement() noexcept {
        m_measuringLatency.store(true, std::memory_order_release);
        const uint64_t curFrames = m_totalFramesProcessed.load(std::memory_order_acquire);
        m_impulseSendFrame.store(curFrames + 64, std::memory_order_release); // Send in 64 frames
        m_measuredLatencyFrames.store(-1, std::memory_order_release);
        m_measuredLatencyMs.store(-1.0, std::memory_order_release);
    }

    /// Record received impulse frame and calculate measured roundtrip latency
    void recordImpulseReturn(uint64_t returnFrame, LatencySource source = LatencySource::SimulatedMock) noexcept {
        uint64_t sendFrame = m_impulseSendFrame.load(std::memory_order_acquire);
        if (returnFrame >= sendFrame) {
            int64_t latencyFrames = static_cast<int64_t>(returnFrame - sendFrame);
            const double sr = std::max(1.0, m_engineSampleRate.load(std::memory_order_acquire));
            m_measuredLatencyFrames.store(latencyFrames, std::memory_order_release);
            m_measuredLatencyMs.store((static_cast<double>(latencyFrames) / sr) * 1000.0, std::memory_order_release);
            m_latencySource.store(source, std::memory_order_release);
            m_measuringLatency.store(false, std::memory_order_release);
        }
    }

private:
    void recomputeCalibrationCandidateLocked() {
        m_calCandidate.sessionActive = true;
        m_calCandidate.confirmed = false;
        m_calCandidate.sampleCount = static_cast<uint32_t>(m_calSamples.size());
        if (m_calSamples.empty()) {
            m_calCandidate.hasMeasurement = false;
            m_calCandidate.confidence = LatencyConfidence::Unmeasured;
            m_calCandidate.summary = "No calibration samples recorded.";
            return;
        }
        std::vector<int64_t> sorted = m_calSamples;
        std::sort(sorted.begin(), sorted.end());
        m_calCandidate.hasMeasurement = true;
        m_calCandidate.minFrames = sorted.front();
        m_calCandidate.maxFrames = sorted.back();
        m_calCandidate.spreadFrames = sorted.back() - sorted.front();
        m_calCandidate.recommendedFrames = sorted[sorted.size() / 2];
        m_calCandidate.peakSnrDb = (m_minCalSnrDb < 900.0) ? m_minCalSnrDb : 0.0;

        if (m_calCandidate.peakSnrDb < 18.0) {
            m_calCandidate.confidence = LatencyConfidence::LowSnr;
            m_calCandidate.summary = "Low SNR (< 18 dB) detected in return path; auto-apply held.";
        } else if (m_calCandidate.spreadFrames > 2) {
            m_calCandidate.confidence = LatencyConfidence::Unstable;
            m_calCandidate.summary = "Round-trip jitter exceeds 2 frames (spread=" +
                                     std::to_string(m_calCandidate.spreadFrames) + "); auto-apply held.";
        } else {
            m_calCandidate.confidence = LatencyConfidence::High;
            m_calCandidate.summary = "Stable round-trip latency candidate ready for confirmation.";
        }
    }

    void retireMock(MockHolder* old) noexcept {
        old->nextRetired = m_retiredMockHead.load(std::memory_order_relaxed);
        while (!m_retiredMockHead.compare_exchange_weak(old->nextRetired, old,
                                                        std::memory_order_release,
                                                        std::memory_order_relaxed));
    }

    std::atomic<InsertState> m_state{InsertState::NotConfigured};
    std::atomic<bool> m_isBypassed{true};
    mutable std::mutex m_configMutex;
    ExternalPortInfo m_config;
    std::shared_ptr<IExternalInsertTransport> m_transport;
    alignas(64) std::atomic<IExternalInsertTransport*> m_rtTransport{nullptr};
    std::atomic<bool> m_requireLiveTransport{false};
    std::atomic<double> m_engineSampleRate{48000.0};
    std::atomic<size_t> m_engineBufferSize{256};
    std::string m_lastError;

    MockHolder* m_activeMock{nullptr};
    alignas(64) std::atomic<MockHolder*> m_preparedMock{nullptr};
    alignas(64) std::atomic<MockHolder*> m_retiredMockHead{nullptr};

    mutable std::mutex m_calMutex;
    std::vector<int64_t> m_calSamples;
    double m_minCalSnrDb = 999.0;
    LatencyCalibrationCandidate m_calCandidate;

    std::atomic<uint32_t> m_xrunCount{0};
    std::atomic<bool> m_measuringLatency{false};
    std::atomic<uint64_t> m_impulseSendFrame{0};
    std::atomic<uint64_t> m_totalFramesProcessed{0};
    std::atomic<bool> m_probeActive{false};
    std::atomic<bool> m_calibrationHoldMute{false};
    std::atomic<bool> m_probeResultReady{false};
    std::atomic<bool> m_probeTimedOut{false};
    std::atomic<uint64_t> m_probeSendFrame{0};
    std::atomic<uint64_t> m_probeCaptureEndFrame{0};
    std::atomic<uint64_t> m_probeTimeoutFrame{0};
    std::atomic<int64_t> m_probeMeasuredFrames{-1};
    std::atomic<double> m_probeMeasuredSnrDb{0.0};
    double m_probePeakAmp{0.0};
    uint64_t m_probePeakFrame{0};
    double m_probeEnergySqSum{0.0};
    uint64_t m_probeSampleCount{0};
    std::atomic<int64_t> m_measuredLatencyFrames{-1};
    std::atomic<double> m_measuredLatencyMs{-1.0};
    std::atomic<uint32_t> m_reportedLatencyFrames{0};
    std::atomic<bool> m_latencyReported{true};
    std::atomic<DisconnectPolicy> m_disconnectPolicy{DisconnectPolicy::AutoBypass};
    std::atomic<bool> m_stopPlaybackRequested{false};
    std::atomic<bool> m_rtDisconnectNotice{false};
    std::atomic<bool> m_rtMismatchNotice{false};
    std::atomic<bool> m_rtMismatchIsRate{false};
    std::atomic<int64_t> m_delayCompensationFrames{-1};
    std::atomic<LatencySource> m_latencySource{LatencySource::Uncalibrated};
};

} // namespace desk32::engine::insert
