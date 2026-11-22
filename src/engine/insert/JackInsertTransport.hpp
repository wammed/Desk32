#pragma once

#include "BufferedInsertTransport.hpp"

#include <atomic>
#include <cstdint>
#include <string>
#include <vector>

struct _jack_client;
struct _jack_port;

namespace desk32::engine::insert {

/// Real JACK (PipeWire-JACK compatibility layer) External Insert Send/Return transport (Spec §6, §8.3).
/// Registers 4 audio ports (send_L, send_R outputs; return_L, return_R inputs), connects 4 explicit
/// JACK links to the target host client, monitors port/connection removal & rate/buffer mismatches,
/// and validates real connection before reporting Connected.
class JackInsertTransport final : public BufferedInsertTransport {
public:
    JackInsertTransport();
    ~JackInsertTransport() override;

    JackInsertTransport(const JackInsertTransport&) = delete;
    JackInsertTransport& operator=(const JackInsertTransport&) = delete;

    /// Discover JACK clients exposing >= 2 audio inputs and >= 2 audio outputs.
    [[nodiscard]] static std::vector<ExternalPortInfo> discoverHosts(
        double defaultSampleRate = 48000.0,
        size_t defaultBufferSize = 256);

    bool configureAndConnect(const ExternalPortInfo& target,
                             double engineSampleRate,
                             size_t engineBufferSize) override;
    void disconnect() override;

    // JACK static callbacks
    static int onProcessCallback(uint32_t nframes, void* arg);
    static int onSampleRateCallback(uint32_t nframes, void* arg);
    static int onBufferSizeCallback(uint32_t nframes, void* arg);
    static void onPortRegistrationCallback(uint32_t portId, int registered, void* arg);
    static void onPortConnectCallback(uint32_t a, uint32_t b, int connect, void* arg);
    static void onShutdownCallback(void* arg);

private:
    _jack_client* m_client = nullptr;
    _jack_port* m_sendPortL = nullptr;
    _jack_port* m_sendPortR = nullptr;
    _jack_port* m_returnPortL = nullptr;
    _jack_port* m_returnPortR = nullptr;
    std::atomic<bool> m_serverShutdown{false};
};

} // namespace desk32::engine::insert
