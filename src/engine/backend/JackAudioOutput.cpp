#include "JackAudioOutput.hpp"

#include <jack/jack.h>

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>

namespace desk32::engine::backend {

namespace {

std::mutex g_pwJackEnvMutex;

/// Scoped RAII helper setting PipeWire-JACK compatibility env hints only around
/// jack_client_open().
class ScopedPwJackEnvHints {
public:
  ScopedPwJackEnvHints(uint32_t sampleRate, uint32_t bufferFrames,
                       bool includeRateHint)
      : m_lock(g_pwJackEnvMutex) {
    if (const char *oldLat = std::getenv("PIPEWIRE_LATENCY")) {
      m_hadLatency = true;
      m_oldLatency = oldLat;
    }
    if (const char *oldRate = std::getenv("PIPEWIRE_RATE")) {
      m_hadRate = true;
      m_oldRate = oldRate;
    }

    char latBuf[64];
    std::snprintf(latBuf, sizeof(latBuf), "%u/%u",
                  bufferFrames > 0 ? bufferFrames : 256u,
                  sampleRate > 0 ? sampleRate : 48000u);
    ::setenv("PIPEWIRE_LATENCY", latBuf, 1);

    if (includeRateHint && sampleRate > 0) {
      char rateBuf[64];
      std::snprintf(rateBuf, sizeof(rateBuf), "1/%u", sampleRate);
      ::setenv("PIPEWIRE_RATE", rateBuf, 1);
    }
  }

  ~ScopedPwJackEnvHints() {
    if (m_hadLatency) {
      ::setenv("PIPEWIRE_LATENCY", m_oldLatency.c_str(), 1);
    } else {
      ::unsetenv("PIPEWIRE_LATENCY");
    }
    if (m_hadRate) {
      ::setenv("PIPEWIRE_RATE", m_oldRate.c_str(), 1);
    } else {
      ::unsetenv("PIPEWIRE_RATE");
    }
  }

private:
  std::lock_guard<std::mutex> m_lock;
  bool m_hadLatency = false;
  bool m_hadRate = false;
  std::string m_oldLatency;
  std::string m_oldRate;
};

uint32_t fnv1a32(const std::string &s) noexcept {
  uint32_t h = 2166136261u;
  for (char c : s) {
    h ^= static_cast<unsigned char>(c);
    h *= 16777619u;
  }
  return h == 0 ? 1u : h;
}

bool isSelfJackClientName(const std::string &prefix,
                          const std::string &ourClientName) {
  return prefix == ourClientName || prefix == "Desk32" ||
         prefix.rfind("desk32-", 0) == 0;
}

} // namespace

JackAudioOutput::JackAudioOutput() = default;

JackAudioOutput::~JackAudioOutput() { shutdown(); }

void JackAudioOutput::setClientName(const std::string &name) {
  std::lock_guard<std::mutex> lock(m_clientMutex);
  m_clientName = name.empty() ? std::string("Desk32") : name;
}

std::string JackAudioOutput::clientName() const {
  std::lock_guard<std::mutex> lock(m_clientMutex);
  return m_clientName;
}

std::string JackAudioOutput::lastError() const {
  std::lock_guard<std::mutex> lock(m_clientMutex);
  return m_lastError;
}

bool JackAudioOutput::initialize(uint32_t sampleRate, uint32_t bufferFrames) {
  m_requestedSampleRate = sampleRate > 0 ? sampleRate : 48000u;
  m_requestedBufferFrames = bufferFrames > 0 ? bufferFrames : 256u;
  m_scratchInterleaved.assign(SCRATCH_FRAMES * 2, 0.0f);
  m_paused.store(false, std::memory_order_release);
  m_latencyControlSupported.store(true, std::memory_order_relaxed);
  m_prevCallbackStartNs = 0;

  {
    std::lock_guard<std::mutex> lock(m_clientMutex);
    m_lastError.clear();
    if (m_targetDeviceId.empty()) {
      m_deviceName =
          m_offlineSimulation ? "offline_jack_simulation" : "jack_default";
    }
  }

  m_initialized.store(true, std::memory_order_relaxed);
  return true;
}

void JackAudioOutput::updateRateTracking(uint32_t actualRate) {
  m_negotiatedSampleRate.store(actualRate, std::memory_order_relaxed);
  if (actualRate > 0 && actualRate == m_requestedSampleRate) {
    m_isDirectMode.store(true, std::memory_order_relaxed);
    m_directModeStatus.store(DirectModeStatus::Active,
                             std::memory_order_relaxed);
  } else if (actualRate > 0) {
    m_isDirectMode.store(false, std::memory_order_relaxed);
    m_directModeStatus.store(DirectModeStatus::Disabled,
                             std::memory_order_relaxed);
  } else {
    m_isDirectMode.store(false, std::memory_order_relaxed);
    m_directModeStatus.store(DirectModeStatus::Unknown,
                             std::memory_order_relaxed);
  }
}

bool JackAudioOutput::start() {
  std::lock_guard<std::mutex> lock(m_clientMutex);
  m_lastError.clear();

  if (m_offlineSimulation) {
    m_paused.store(false, std::memory_order_release);
    m_negotiatedSampleRate.store(m_requestedSampleRate,
                                 std::memory_order_relaxed);
    m_negotiatedQuantum.store(m_requestedBufferFrames,
                              std::memory_order_relaxed);
    m_isDirectMode.store(true, std::memory_order_relaxed);
    m_directModeStatus.store(DirectModeStatus::Active,
                             std::memory_order_relaxed);
    m_state.store(StreamState::Streaming, std::memory_order_relaxed);
    return true;
  }

  if (m_client) {
    m_paused.store(false, std::memory_order_release);
    m_state.store(StreamState::Streaming, std::memory_order_relaxed);
    return true;
  }

  if (!m_initialized.load(std::memory_order_relaxed)) {
    m_lastError = "JackAudioOutput not initialized";
    return false;
  }

  m_state.store(StreamState::Connecting, std::memory_order_relaxed);

  jack_status_t status = static_cast<jack_status_t>(0);
  {
    ScopedPwJackEnvHints envHints(
        m_requestedSampleRate, m_requestedBufferFrames,
        m_sendRateHintOnOpen.load(std::memory_order_relaxed));
    m_client =
        jack_client_open(m_clientName.c_str(), JackNoStartServer, &status);
  }

  if (!m_client) {
    m_lastError = "Failed to open PipeWire-JACK client (status=0x" +
                  std::to_string(static_cast<unsigned int>(status)) + ")";
    m_state.store(StreamState::Error, std::memory_order_relaxed);
    m_isDirectMode.store(false, std::memory_order_relaxed);
    m_directModeStatus.store(DirectModeStatus::Unknown,
                             std::memory_order_relaxed);
    return false;
  }

  jack_set_process_callback(m_client, &JackAudioOutput::onProcessCallback,
                            this);
  jack_set_sample_rate_callback(m_client,
                                &JackAudioOutput::onSampleRateCallback, this);
  jack_set_buffer_size_callback(m_client,
                                &JackAudioOutput::onBufferSizeCallback, this);
  jack_set_xrun_callback(m_client, &JackAudioOutput::onXrunCallback, this);
  jack_on_shutdown(m_client, &JackAudioOutput::onShutdownCallback, this);

  m_portL = jack_port_register(m_client, "playback_1", JACK_DEFAULT_AUDIO_TYPE,
                               JackPortIsOutput | JackPortIsTerminal, 0);
  m_portR = jack_port_register(m_client, "playback_2", JACK_DEFAULT_AUDIO_TYPE,
                               JackPortIsOutput | JackPortIsTerminal, 0);
  if (!m_portL || !m_portR) {
    m_lastError = "Failed to register JACK stereo playback ports";
    jack_client_close(m_client);
    m_client = nullptr;
    m_portL = nullptr;
    m_portR = nullptr;
    m_state.store(StreamState::Error, std::memory_order_relaxed);
    return false;
  }

  const uint32_t actualRate =
      static_cast<uint32_t>(jack_get_sample_rate(m_client));
  const uint32_t actualBuf =
      static_cast<uint32_t>(jack_get_buffer_size(m_client));
  m_negotiatedQuantum.store(actualBuf, std::memory_order_relaxed);
  updateRateTracking(actualRate);
  if (actualBuf > SCRATCH_FRAMES) {
    m_scratchInterleaved.assign(static_cast<size_t>(actualBuf) * 2, 0.0f);
  }

  m_paused.store(false, std::memory_order_release);
  if (jack_activate(m_client) != 0) {
    m_lastError = "jack_activate() failed";
    jack_client_close(m_client);
    m_client = nullptr;
    m_portL = nullptr;
    m_portR = nullptr;
    m_state.store(StreamState::Error, std::memory_order_relaxed);
    return false;
  }

  if (!m_targetDeviceId.empty() ||
      m_autoConnect.load(std::memory_order_relaxed)) {
    std::string connErr;
    if (!connectTargetLocked(&connErr) && !m_targetDeviceId.empty()) {
      m_lastError = connErr;
    }
  }

  m_state.store(StreamState::Streaming, std::memory_order_relaxed);
  return true;
}

void JackAudioOutput::pause() {
  m_paused.store(true, std::memory_order_release);
  if (m_state.load(std::memory_order_relaxed) == StreamState::Streaming) {
    m_state.store(StreamState::Paused, std::memory_order_relaxed);
  }
}

void JackAudioOutput::stop() {
  std::lock_guard<std::mutex> lock(m_clientMutex);
  m_paused.store(false, std::memory_order_release);

  if (m_offlineSimulation) {
    m_state.store(StreamState::Paused, std::memory_order_relaxed);
    return;
  }

  if (m_client) {
    jack_deactivate(m_client);
    if (m_portL) {
      jack_port_unregister(m_client, m_portL);
      m_portL = nullptr;
    }
    if (m_portR) {
      jack_port_unregister(m_client, m_portR);
      m_portR = nullptr;
    }
    jack_client_close(m_client);
    m_client = nullptr;
  }

  m_isDirectMode.store(false, std::memory_order_relaxed);
  m_directModeStatus.store(DirectModeStatus::Unknown,
                           std::memory_order_relaxed);
  m_state.store(StreamState::Unconnected, std::memory_order_relaxed);
}

void JackAudioOutput::shutdown() {
  stop();
  m_state.store(StreamState::Unconnected, std::memory_order_relaxed);
  m_initialized.store(false, std::memory_order_relaxed);
}

bool JackAudioOutput::renegotiateSampleRate(uint32_t targetRate,
                                            double &outSilenceMs) {
  const auto t0 = std::chrono::steady_clock::now();
  if (targetRate == 0) {
    outSilenceMs = 0.0;
    return false;
  }

  m_sendRateHintOnOpen.store(true, std::memory_order_relaxed);

  if (m_offlineSimulation || !m_initialized.load(std::memory_order_relaxed)) {
    m_requestedSampleRate = targetRate;
    m_negotiatedSampleRate.store(targetRate, std::memory_order_relaxed);
    updateRateTracking(targetRate);
    const auto t1 = std::chrono::steady_clock::now();
    outSilenceMs = std::chrono::duration<double, std::milli>(t1 - t0).count();
    m_lastRenegotiationSilenceMs.store(outSilenceMs, std::memory_order_relaxed);
    return true;
  }

  const StreamState cur = m_state.load(std::memory_order_relaxed);
  const bool wasActive =
      (cur == StreamState::Streaming || cur == StreamState::Connecting ||
       m_client != nullptr);
  if (wasActive) {
    stop();
  }

  m_requestedSampleRate = targetRate;
  m_negotiatedSampleRate.store(0, std::memory_order_relaxed);
  m_isDirectMode.store(false, std::memory_order_relaxed);
  m_directModeStatus.store(DirectModeStatus::Unknown,
                           std::memory_order_relaxed);

  bool ok = true;
  if (wasActive) {
    ok = start();
  }

  const auto t1 = std::chrono::steady_clock::now();
  outSilenceMs = std::chrono::duration<double, std::milli>(t1 - t0).count();
  m_lastRenegotiationSilenceMs.store(outSilenceMs, std::memory_order_relaxed);
  return ok;
}

std::vector<AudioOutputDeviceInfo> JackAudioOutput::enumerateFromJackClient(
    _jack_client *client, std::vector<TargetPortPair> *outRoutes) const {
  std::vector<AudioOutputDeviceInfo> devices;
  if (outRoutes) {
    outRoutes->clear();
  }
  if (!client) {
    return devices;
  }

  const uint32_t sampleRate =
      static_cast<uint32_t>(jack_get_sample_rate(client));
  const char **portNames =
      jack_get_ports(client, nullptr, JACK_DEFAULT_AUDIO_TYPE, JackPortIsInput);
  if (!portNames) {
    return devices;
  }

  struct Group {
    bool isPhysical = false;
    std::vector<std::string> fullNames;
    std::vector<std::string> shortNames;
  };
  std::map<std::string, Group> groups;

  for (size_t i = 0; portNames[i] != nullptr; ++i) {
    const std::string full = portNames[i];
    const size_t colon = full.find(':');
    if (colon == std::string::npos || colon == 0) {
      continue;
    }
    const std::string clientPrefix = full.substr(0, colon);
    if (isSelfJackClientName(clientPrefix, m_clientName)) {
      continue;
    }
    const std::string shortPort = full.substr(colon + 1);
    jack_port_t *portObj = jack_port_by_name(client, full.c_str());
    const bool isPhys =
        portObj && ((jack_port_flags(portObj) & JackPortIsPhysical) != 0);

    auto &g = groups[clientPrefix];
    g.isPhysical = g.isPhysical || isPhys;
    g.fullNames.push_back(full);
    g.shortNames.push_back(shortPort);
  }
  jack_free(portNames);

  bool assignedDefault = false;
  for (auto &[clientPrefix, g] : groups) {
    if (g.fullNames.empty()) {
      continue;
    }
    std::vector<size_t> order(g.fullNames.size());
    for (size_t k = 0; k < order.size(); ++k)
      order[k] = k;
    std::stable_sort(order.begin(), order.end(), [&](size_t a, size_t b) {
      const int ra = rankStereoChannelPortHint("", g.shortNames[a]);
      const int rb = rankStereoChannelPortHint("", g.shortNames[b]);
      if (ra != rb)
        return ra < rb;
      return g.shortNames[a] < g.shortNames[b];
    });

    AudioOutputDeviceInfo info;
    info.nodeId = fnv1a32(clientPrefix);
    info.name = clientPrefix;
    info.description = g.isPhysical ? (clientPrefix + " (JACK Physical Sink)")
                                    : (clientPrefix + " (JACK Client)");
    info.mediaClass = g.isPhysical ? "JACK/PhysicalSink" : "JACK/ClientInput";
    info.isDefault = g.isPhysical && !assignedDefault;
    if (info.isDefault) {
      assignedDefault = true;
    }
    info.sampleRate = sampleRate;
    info.channels = static_cast<uint32_t>(g.fullNames.size());
    info.nativeFormat = OutputPcmFormat::F32LE;
    devices.push_back(info);

    if (outRoutes) {
      TargetPortPair pair;
      pair.deviceName = clientPrefix;
      pair.portL = g.fullNames[order[0]];
      pair.portR = g.fullNames[order.size() >= 2 ? order[1] : order[0]];
      pair.isPhysical = g.isPhysical;
      outRoutes->push_back(std::move(pair));
    }
  }

  return devices;
}

std::vector<AudioOutputDeviceInfo>
JackAudioOutput::enumerateOutputDevices() const {
  std::lock_guard<std::mutex> lock(m_clientMutex);
  if (m_offlineSimulation) {
    if (!m_simulatedDevices.empty()) {
      auto copy = m_simulatedDevices;
      for (auto &d : copy) {
        d.nativeFormat = OutputPcmFormat::F32LE;
        d.isDefault = m_targetDeviceId.empty()
                          ? d.isDefault
                          : (d.name == m_targetDeviceId ||
                             std::to_string(d.nodeId) == m_targetDeviceId);
      }
      return copy;
    }
    AudioOutputDeviceInfo def;
    def.nodeId = 1;
    def.name = "offline_jack_simulation";
    def.description = "Offline JACK Simulation Sink";
    def.mediaClass = "JACK/PhysicalSink";
    def.isDefault = true;
    def.sampleRate = m_requestedSampleRate;
    def.channels = 2;
    def.nativeFormat = OutputPcmFormat::F32LE;
    return {def};
  }

  std::vector<AudioOutputDeviceInfo> result;
  if (m_client) {
    result = enumerateFromJackClient(m_client);
  } else {
    jack_status_t st = static_cast<jack_status_t>(0);
    jack_client_t *probe =
        jack_client_open("desk32-jack-enum", JackNoStartServer, &st);
    if (probe) {
      result = enumerateFromJackClient(probe);
      jack_client_close(probe);
    }
  }

  for (auto &d : result) {
    if (!m_targetDeviceId.empty()) {
      d.isDefault = (d.name == m_targetDeviceId ||
                     std::to_string(d.nodeId) == m_targetDeviceId);
    }
  }
  return result;
}

void JackAudioOutput::setSimulatedOutputDevices(
    std::vector<AudioOutputDeviceInfo> devices) {
  std::lock_guard<std::mutex> lock(m_clientMutex);
  m_simulatedDevices = std::move(devices);
}

void JackAudioOutput::disconnectPortsLocked() {
  if (!m_client)
    return;
  if (m_portL) {
    jack_port_disconnect(m_client, m_portL);
  }
  if (m_portR) {
    jack_port_disconnect(m_client, m_portR);
  }
}

bool JackAudioOutput::connectTargetLocked(std::string *outError) {
  if (!m_client || !m_portL || !m_portR) {
    if (outError)
      *outError = "JACK client ports are not active";
    return false;
  }

  disconnectPortsLocked();

  std::vector<TargetPortPair> routes;
  const auto devices = enumerateFromJackClient(m_client, &routes);
  (void)devices;

  const TargetPortPair *chosen = nullptr;
  if (m_targetDeviceId.empty() || m_targetDeviceId == "default") {
    for (const auto &r : routes) {
      if (r.isPhysical) {
        chosen = &r;
        break;
      }
    }
    if (!chosen && !routes.empty()) {
      chosen = &routes.front();
    }
  } else {
    for (const auto &r : routes) {
      if (r.deviceName == m_targetDeviceId ||
          std::to_string(fnv1a32(r.deviceName)) == m_targetDeviceId) {
        chosen = &r;
        break;
      }
    }
  }

  if (!chosen) {
    if (outError) {
      *outError = m_targetDeviceId.empty()
                      ? "No JACK playback target ports available"
                      : ("JACK playback target not found: " + m_targetDeviceId);
    }
    return false;
  }

  const int rcL =
      jack_connect(m_client, jack_port_name(m_portL), chosen->portL.c_str());
  const int rcR =
      jack_connect(m_client, jack_port_name(m_portR), chosen->portR.c_str());
  if ((rcL != 0 && rcL != EEXIST) || (rcR != 0 && rcR != EEXIST)) {
    if (outError) {
      *outError = "jack_connect failed for target '" + chosen->deviceName + "'";
    }
    return false;
  }

  m_deviceName = m_targetDeviceId.empty() ? ("jack:" + chosen->deviceName)
                                          : chosen->deviceName;
  return true;
}

bool JackAudioOutput::setTargetDevice(const std::string &deviceIdOrName,
                                      std::string *outError) {
  const std::string normalized =
      (deviceIdOrName == "default") ? std::string{} : deviceIdOrName;
  std::lock_guard<std::mutex> lock(m_clientMutex);

  if (m_offlineSimulation) {
    m_targetDeviceId = normalized;
    m_deviceName = normalized.empty() ? "offline_jack_simulation" : normalized;
    return true;
  }

  const std::string prevTarget = m_targetDeviceId;
  const std::string prevDevName = m_deviceName;
  m_targetDeviceId = normalized;
  m_deviceName = normalized.empty() ? "jack_default" : normalized;

  if (!m_client) {
    return true;
  }

  if (!connectTargetLocked(outError)) {
    m_targetDeviceId = prevTarget;
    m_deviceName = prevDevName;
    connectTargetLocked(nullptr);
    return false;
  }
  return true;
}

std::string JackAudioOutput::targetDeviceId() const {
  std::lock_guard<std::mutex> lock(m_clientMutex);
  return m_targetDeviceId;
}

bool JackAudioOutput::setBufferFrames(uint32_t bufferFrames) {
  if (bufferFrames == 0 || bufferFrames > 16384) {
    return false;
  }
  std::lock_guard<std::mutex> lock(m_clientMutex);
  m_requestedBufferFrames = bufferFrames;
  if (m_offlineSimulation) {
    m_negotiatedQuantum.store(bufferFrames, std::memory_order_relaxed);
    m_latencyControlSupported.store(true, std::memory_order_relaxed);
    return true;
  }
  if (!m_client) {
    return true;
  }

  const int rc =
      jack_set_buffer_size(m_client, static_cast<jack_nframes_t>(bufferFrames));
  if (rc != 0) {
    m_latencyControlSupported.store(false, std::memory_order_relaxed);
    m_lastError = "jack_set_buffer_size rejected by PipeWire-JACK server";
    return false;
  }
  m_latencyControlSupported.store(true, std::memory_order_relaxed);
  m_negotiatedQuantum.store(
      static_cast<uint32_t>(jack_get_buffer_size(m_client)),
      std::memory_order_relaxed);
  return true;
}

bool JackAudioOutput::setPreferredFormat(OutputPcmFormat format) {
  // JACK strictly transports 32-bit float (F32LE) samples.
  return format == OutputPcmFormat::F32LE;
}

StreamDiagnostics JackAudioOutput::diagnostics() const noexcept {
  StreamDiagnostics diag;
  const uint32_t negotiated =
      m_negotiatedSampleRate.load(std::memory_order_relaxed);
  diag.requestedSampleRate = m_requestedSampleRate;
  diag.sampleRate = negotiated > 0 ? negotiated : m_requestedSampleRate;
  diag.channels = 2;
  diag.bufferFrames = m_requestedBufferFrames;
  diag.negotiatedQuantum = m_negotiatedQuantum.load(std::memory_order_relaxed);
  diag.preferredFormat = OutputPcmFormat::F32LE;
  diag.negotiatedFormat = OutputPcmFormat::F32LE;
  diag.formatName = "F32LE";
  diag.outputBitDepth = 32;
  diag.outputFormatLossless = true;
  diag.ditherActive = false; // F32LE never quantizes to integer PCM
  {
    std::lock_guard<std::mutex> lock(m_clientMutex);
    diag.deviceName = m_deviceName;
    diag.targetDeviceId = m_targetDeviceId;
  }
  diag.xrunCount = m_xrunCount.load(std::memory_order_relaxed);
  diag.totalFramesProcessed =
      m_totalFramesProcessed.load(std::memory_order_relaxed);
  diag.isDirectMode = m_isDirectMode.load(std::memory_order_relaxed);
  diag.directModeStatus = m_directModeStatus.load(std::memory_order_relaxed);
  diag.lastCallbackDurationNs =
      m_lastCallbackDurationNs.load(std::memory_order_relaxed);
  diag.maxCallbackDurationNs =
      m_maxCallbackDurationNs.load(std::memory_order_relaxed);
  diag.lastCallbackIntervalNs =
      m_lastCallbackIntervalNs.load(std::memory_order_relaxed);
  diag.expectedPeriodNs = m_expectedPeriodNs.load(std::memory_order_relaxed);
  diag.lastSchedJitterNs = m_lastSchedJitterNs.load(std::memory_order_relaxed);
  diag.maxAbsSchedJitterNs =
      m_maxAbsSchedJitterNs.load(std::memory_order_relaxed);
  diag.callbackCount = m_callbackCount.load(std::memory_order_relaxed);
  return diag;
}

void JackAudioOutput::renderOfflineBlock(float *outInterleaved, size_t frames) {
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

void JackAudioOutput::renderOfflineRawBlock(void *outInterleaved, size_t frames,
                                            OutputPcmFormat *outUsedFormat) {
  if (outUsedFormat) {
    *outUsedFormat = OutputPcmFormat::F32LE;
  }
  renderOfflineBlock(static_cast<float *>(outInterleaved), frames);
}

int JackAudioOutput::onProcessCallback(uint32_t nframes, void *arg) {
  const auto cbStart = std::chrono::steady_clock::now();
  const int64_t cbStartNs =
      std::chrono::duration_cast<std::chrono::nanoseconds>(
          cbStart.time_since_epoch())
          .count();
  auto *self = static_cast<JackAudioOutput *>(arg);
  if (!self || !self->m_portL || !self->m_portR) {
    return 0;
  }

  auto *outL = static_cast<float *>(jack_port_get_buffer(
      self->m_portL, static_cast<jack_nframes_t>(nframes)));
  auto *outR = static_cast<float *>(jack_port_get_buffer(
      self->m_portR, static_cast<jack_nframes_t>(nframes)));
  if (!outL || !outR) {
    return 0;
  }

  if (self->m_paused.load(std::memory_order_acquire) ||
      (!self->m_rawCallback && !self->m_callback)) {
    std::memset(outL, 0, static_cast<size_t>(nframes) * sizeof(float));
    std::memset(outR, 0, static_cast<size_t>(nframes) * sizeof(float));
    return 0;
  }

  const size_t maxChunk = self->m_scratchInterleaved.size() / 2;
  size_t remaining = static_cast<size_t>(nframes);
  size_t offset = 0;

  while (remaining > 0 && maxChunk > 0) {
    const size_t chunk = std::min(remaining, maxChunk);
    if (self->m_rawCallback) {
      self->m_rawCallback(self->m_scratchInterleaved.data(), chunk,
                          OutputPcmFormat::F32LE);
    } else {
      self->m_callback(self->m_scratchInterleaved.data(), chunk);
    }
    const float *src = self->m_scratchInterleaved.data();
    for (size_t i = 0; i < chunk; ++i) {
      outL[offset + i] = src[i * 2];
      outR[offset + i] = src[i * 2 + 1];
    }
    remaining -= chunk;
    offset += chunk;
  }
  if (remaining > 0) {
    std::memset(outL + offset, 0, remaining * sizeof(float));
    std::memset(outR + offset, 0, remaining * sizeof(float));
  }

  self->m_totalFramesProcessed.fetch_add(static_cast<uint64_t>(nframes),
                                         std::memory_order_relaxed);
  self->m_negotiatedQuantum.store(nframes, std::memory_order_relaxed);

  const uint32_t activeSr =
      self->m_negotiatedSampleRate.load(std::memory_order_relaxed);
  const uint32_t effSr = activeSr > 0 ? activeSr : self->m_requestedSampleRate;
  const uint64_t expectedNs =
      (effSr > 0 && nframes > 0)
          ? static_cast<uint64_t>((static_cast<double>(nframes) * 1e9) /
                                  static_cast<double>(effSr))
          : 0ull;
  self->m_expectedPeriodNs.store(expectedNs, std::memory_order_relaxed);

  if (self->m_prevCallbackStartNs > 0) {
    const int64_t intervalNs = cbStartNs - self->m_prevCallbackStartNs;
    self->m_lastCallbackIntervalNs.store(
        intervalNs > 0 ? static_cast<uint64_t>(intervalNs) : 0ull,
        std::memory_order_relaxed);
    if (expectedNs > 0) {
      const int64_t jitterNs = intervalNs - static_cast<int64_t>(expectedNs);
      const uint64_t absJitter =
          static_cast<uint64_t>(jitterNs >= 0 ? jitterNs : -jitterNs);
      self->m_lastSchedJitterNs.store(jitterNs, std::memory_order_relaxed);
      uint64_t curMaxJ =
          self->m_maxAbsSchedJitterNs.load(std::memory_order_relaxed);
      while (absJitter > curMaxJ &&
             !self->m_maxAbsSchedJitterNs.compare_exchange_weak(
                 curMaxJ, absJitter, std::memory_order_relaxed)) {
      }
    }
  }
  self->m_prevCallbackStartNs = cbStartNs;

  const auto cbEnd = std::chrono::steady_clock::now();
  const uint64_t durNs = static_cast<uint64_t>(std::max<int64_t>(
      0, std::chrono::duration_cast<std::chrono::nanoseconds>(cbEnd - cbStart)
             .count()));
  self->m_lastCallbackDurationNs.store(durNs, std::memory_order_relaxed);
  uint64_t curMaxDur =
      self->m_maxCallbackDurationNs.load(std::memory_order_relaxed);
  while (durNs > curMaxDur &&
         !self->m_maxCallbackDurationNs.compare_exchange_weak(
             curMaxDur, durNs, std::memory_order_relaxed)) {
  }
  self->m_callbackCount.fetch_add(1, std::memory_order_relaxed);
  return 0;
}

int JackAudioOutput::onSampleRateCallback(uint32_t nframes, void *arg) {
  auto *self = static_cast<JackAudioOutput *>(arg);
  if (self && nframes > 0) {
    self->updateRateTracking(nframes);
  }
  return 0;
}

int JackAudioOutput::onBufferSizeCallback(uint32_t nframes, void *arg) {
  auto *self = static_cast<JackAudioOutput *>(arg);
  if (self && nframes > 0) {
    self->m_negotiatedQuantum.store(nframes, std::memory_order_relaxed);
  }
  return 0;
}

int JackAudioOutput::onXrunCallback(void *arg) {
  auto *self = static_cast<JackAudioOutput *>(arg);
  if (self) {
    self->m_xrunCount.fetch_add(1, std::memory_order_relaxed);
  }
  return 0;
}

void JackAudioOutput::onShutdownCallback(void *arg) {
  auto *self = static_cast<JackAudioOutput *>(arg);
  if (self) {
    self->m_isDirectMode.store(false, std::memory_order_relaxed);
    self->m_directModeStatus.store(DirectModeStatus::Unknown,
                                   std::memory_order_relaxed);
    self->m_state.store(StreamState::Error, std::memory_order_relaxed);
  }
}

} // namespace desk32::engine::backend
