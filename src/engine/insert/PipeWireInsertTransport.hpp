#pragma once

#include "BufferedInsertTransport.hpp"

#include <map>
#include <memory>
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
class PipeWireInsertTransport final : public BufferedInsertTransport {
public:
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
