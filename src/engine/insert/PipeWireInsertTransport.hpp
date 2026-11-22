#pragma once

#include "ExternalInsertBridge.hpp"
#include "engine/buffer/PcmRingBuffer.hpp"

#include <array>
#include <atomic>
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <vector>

struct pw_thread_loop;
struct pw_context;
struct pw_core;
struct pw_registry;
struct pw_stream;
struct pw_proxy;
struct spa_dict;
struct spa_hook;
struct spa_pod;

namespace desk32::engine::insert {

/// Real PipeWire External Insert Send/Return transport (Spec §6.1-6.6, §8.3).
/// Creates Send (`PW_DIRECTION_OUTPUT`) & Return (`PW_DIRECTION_INPUT`) `pw_stream`s,
/// resolves 4 target ports via `pw_registry`, creates 4 `PW_TYPE_INTERFACE_Link` objects,
/// monitors `global_remove` / parameter changes for disconnect & rate/buffer mismatches,
/// and supports deterministic simulation hooks for headless CI verification.
class PipeWireInsertTransport final : public IExternalInsertTransport {
public:
    static constexpr size_t RING_CAPACITY_FRAMES = 32768;

    PipeWireInsertTransport();
    ~PipeWireInsertTransport() override;

    PipeWireInsertTransport(const PipeWireInsertTransport&) = delete;
    PipeWireInsertTransport& operator=(const PipeWireInsertTransport&) = delete;

    /// Native PipeWire registry host/port discovery (control thread, zero subprocess spawn).
    [[nodiscard]] static std::vector<ExternalPortInfo> discoverHosts(
        double defaultSampleRate = 48000.0,
        size_t defaultBufferSize = 256);

    bool configureAndConnect(const ExternalPortInfo& target,
                             double engineSampleRate,
                             size_t engineBufferSize) override;
    void disconnect() override;

    [[nodiscard]] bool isConnected() const noexcept override {
        return m_connected.load(std::memory_order_acquire);
    }

    bool exchangeBlock(double* inOutLeft, double* inOutRight, size_t frames) noexcept override;

    [[nodiscard]] bool hasRateMismatch() const noexcept override {
        return m_rateMismatch.load(std::memory_order_acquire);
    }
    [[nodiscard]] bool hasBufferMismatch() const noexcept override {
        return m_bufferMismatch.load(std::memory_order_acquire);
    }
    [[nodiscard]] bool consumeGraphDisconnectEvent() noexcept override {
        return m_graphDisconnected.exchange(false, std::memory_order_acq_rel);
    }

    [[nodiscard]] InsertTransportDiagnostics diagnostics() const override;

    // -------------------------------------------------------------------------
    // Headless / CI Simulation & Event Injection Hooks
    // -------------------------------------------------------------------------
    void setSimulationMode(bool enabled) noexcept {
        m_simulationMode.store(enabled, std::memory_order_release);
    }
    void setSimulationLoopbackLatencyFrames(uint32_t frames) noexcept {
        m_simLatencyFrames.store(std::min<uint32_t>(frames, static_cast<uint32_t>(RING_CAPACITY_FRAMES / 2)),
                                 std::memory_order_release);
    }
    void simulateGraphNodeRemoved() noexcept {
        m_graphDisconnected.store(true, std::memory_order_release);
        m_connected.store(false, std::memory_order_release);
        std::lock_guard<std::mutex> lock(m_stateMutex);
        m_errorDetail = "Simulated PipeWire graph global_remove for external insert node";
    }
    void simulateRateMismatch(uint32_t actualRate) noexcept {
        m_negotiatedSendRate.store(actualRate, std::memory_order_release);
        m_rateMismatch.store(true, std::memory_order_release);
        std::lock_guard<std::mutex> lock(m_stateMutex);
        m_errorDetail = "PipeWire stream rate mismatch (" + std::to_string(actualRate) + " Hz)";
    }
    void simulateBufferMismatch(uint32_t actualQuantum) noexcept {
        m_negotiatedQuantum.store(actualQuantum, std::memory_order_release);
        m_bufferMismatch.store(true, std::memory_order_release);
        std::lock_guard<std::mutex> lock(m_stateMutex);
        m_errorDetail = "PipeWire stream quantum mismatch (" + std::to_string(actualQuantum) + " frames)";
    }

    // PipeWire static event callbacks
    static void onCoreDone(void* data, uint32_t id, int seq);
    static void onRegistryGlobal(void* data, uint32_t id, uint32_t permissions,
                                 const char* type, uint32_t version,
                                 const struct ::spa_dict* props);
    static void onRegistryGlobalRemove(void* data, uint32_t id);
    static void onSendStateChanged(void* data, int oldState, int newState, const char* error);
    static void onReturnStateChanged(void* data, int oldState, int newState, const char* error);
    static void onSendParamChanged(void* data, uint32_t id, const struct ::spa_pod* param);
    static void onReturnParamChanged(void* data, uint32_t id, const struct ::spa_pod* param);
    static void onSendProcess(void* data);
    static void onReturnProcess(void* data);
    static void onLinkProxyRemoved(void* data);
    static void onLinkProxyError(void* data, int seq, int res, const char* message);

private:
    struct DiscoveredNode {
        std::string name;
        std::string appName;
        std::string nick;
    };

    struct DiscoveredPort {
        uint32_t id = 0;
        uint32_t nodeId = 0;
        uint32_t portOrder = 0;
        std::string name;
        std::string alias;
        std::string channel;
        std::string direction; // "in" or "out"
    };

    struct LinkBinding {
        struct pw_proxy* proxy = nullptr;
        std::unique_ptr<struct spa_hook> listener;
    };

    void tryResolveAndCreateLinksLocked();
    void teardownPipeWireLocked();

    mutable std::mutex m_stateMutex;
    ExternalPortInfo m_targetConfig;
    double m_expectedSampleRate = 48000.0;
    size_t m_expectedBufferSize = 256;
    std::string m_errorDetail;

    std::atomic<bool> m_simulationMode{false};
    std::atomic<uint32_t> m_simLatencyFrames{0};
    std::atomic<bool> m_connected{false};
    std::atomic<bool> m_rateMismatch{false};
    std::atomic<bool> m_bufferMismatch{false};
    std::atomic<bool> m_graphDisconnected{false};
    std::atomic<uint32_t> m_resolvedTargetPorts{0};
    std::atomic<uint32_t> m_activeLinksCreated{0};
    std::atomic<uint64_t> m_sendBlocksWritten{0};
    std::atomic<uint64_t> m_returnBlocksRead{0};
    std::atomic<uint64_t> m_xrunOrUnderruns{0};
    std::atomic<uint32_t> m_negotiatedSendRate{0};
    std::atomic<uint32_t> m_negotiatedReturnRate{0};
    std::atomic<uint32_t> m_negotiatedQuantum{0};
    std::atomic<uint32_t> m_sendNodeId{0};
    std::atomic<uint32_t> m_returnNodeId{0};

    // Pre-allocated RT lock-free SPSC ring buffers and scratch arrays (zero RT alloc)
    buffer::PcmRingBuffer m_sendRing{RING_CAPACITY_FRAMES};
    buffer::PcmRingBuffer m_returnRing{RING_CAPACITY_FRAMES};
    std::array<double, 8192> m_rtReadL{};
    std::array<double, 8192> m_rtReadR{};
    std::array<double, 8192> m_pwSendTmpL{};
    std::array<double, 8192> m_pwSendTmpR{};
    std::array<double, 8192> m_pwRetTmpL{};
    std::array<double, 8192> m_pwRetTmpR{};

    // Pre-allocated simulation delay ring (power-of-two)
    std::vector<double> m_simDelayL;
    std::vector<double> m_simDelayR;
    size_t m_simWritePos = 0;

    // PipeWire objects (protected by pw_thread_loop / m_stateMutex)
    struct pw_thread_loop* m_threadLoop = nullptr;
    struct pw_context* m_context = nullptr;
    struct pw_core* m_core = nullptr;
    struct pw_registry* m_registry = nullptr;
    struct pw_stream* m_sendStream = nullptr;
    struct pw_stream* m_returnStream = nullptr;
    std::unique_ptr<struct spa_hook> m_coreListener;
    std::unique_ptr<struct spa_hook> m_registryListener;
    std::unique_ptr<struct spa_hook> m_sendListener;
    std::unique_ptr<struct spa_hook> m_returnListener;
    int m_syncSeq = 0;
    bool m_syncDone = false;
    bool m_threadLoopStarted = false;

    std::map<uint32_t, DiscoveredNode> m_nodesById;
    std::map<uint32_t, DiscoveredPort> m_portsById;
    std::vector<LinkBinding> m_linkBindings;
    std::set<uint32_t> m_watchedIds;
};

} // namespace desk32::engine::insert
