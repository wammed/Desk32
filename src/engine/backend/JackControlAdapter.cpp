#include "JackControlAdapter.hpp"

namespace desk32::engine::backend {

std::vector<AudioOutputDeviceInfo> JackControlAdapter::enumerateOutputDevices() const
{
    if (!m_output) {
        return {};
    }
    return m_output->enumerateOutputDevices();
}

std::string JackControlAdapter::currentTargetDevice() const
{
    if (!m_output) {
        return "default";
    }
    const std::string target = m_output->targetDeviceId();
    if (!target.empty()) {
        return target;
    }
    const auto diag = m_output->diagnostics();
    return diag.deviceName.empty() ? std::string("default") : diag.deviceName;
}

bool JackControlAdapter::setTargetDevice(const std::string& deviceIdOrName, std::string* outError)
{
    if (!m_output) {
        if (outError) *outError = "JACK output not initialized";
        return false;
    }

    if (deviceIdOrName.empty() || deviceIdOrName == "default") {
        return m_output->setTargetDevice("default", outError);
    }

    const auto devices = m_output->enumerateOutputDevices();
    std::string resolvedName;
    if (!devices.empty()) {
        for (const auto& d : devices) {
            if (d.name == deviceIdOrName ||
                std::to_string(d.nodeId) == deviceIdOrName ||
                (!d.description.empty() && d.description == deviceIdOrName)) {
                resolvedName = d.name;
                break;
            }
        }
        if (resolvedName.empty()) {
            if (outError) {
                *outError = "Unknown JACK playback target: " + deviceIdOrName;
            }
            return false;
        }
    } else {
        resolvedName = deviceIdOrName;
    }

    return m_output->setTargetDevice(resolvedName, outError);
}

bool JackControlAdapter::applyLatencySetting(uint32_t bufferFrames, double /*sampleRate*/)
{
    return m_output ? m_output->setBufferFrames(bufferFrames) : false;
}

bool JackControlAdapter::renegotiateSampleRate(uint32_t targetRate, double& outSilenceMs)
{
    if (!m_output) {
        outSilenceMs = 0.0;
        return false;
    }
    return m_output->renegotiateSampleRate(targetRate, outSilenceMs);
}

bool JackControlAdapter::setPreferredPcmFormat(OutputPcmFormat format)
{
    return m_output ? m_output->setPreferredFormat(format) : false;
}

OutputPcmFormat JackControlAdapter::preferredPcmFormat() const
{
    return m_output ? m_output->preferredFormat() : OutputPcmFormat::F32LE;
}

StreamDiagnostics JackControlAdapter::diagnostics() const
{
    return m_output ? m_output->diagnostics() : StreamDiagnostics{};
}

StreamState JackControlAdapter::streamState() const
{
    return m_output ? m_output->state() : StreamState::Unconnected;
}

BackendCapabilitiesReport JackControlAdapter::queryCapabilities(bool offlineSimulation,
                                                                bool streamInitialized) const
{
    BackendCapabilitiesReport report;
    report.backendName = backendName();
    report.exclusiveAccess = CapabilityAvailability::Unsupported;
    report.exclusiveAccessReason = "PipeWire-JACK operates on a shared PipeWire graph; exclusive device locking is not exposed.";
    report.formatSelection = CapabilityAvailability::Unsupported;
    report.formatSelectionReason = "JACK protocol strictly carries 32-bit float (F32LE) normalized samples.";
    report.hardwareBitPerfect = CapabilityAvailability::Unknown;
    report.hardwareBitPerfectReason = "Hardware DAC physical output requires external Level 3 loopback capture.";
    report.externalInsert = CapabilityAvailability::Supported;
    report.offlineRender = CapabilityAvailability::Supported;

    if (!m_output || !streamInitialized) {
        report.directMode = CapabilityAvailability::Unavailable;
        report.directModeReason = "JACK audio output is not initialized.";
        report.sampleRateFollowing = CapabilityAvailability::Unavailable;
        report.sampleRateFollowingReason = "JACK audio output is not initialized.";
        report.deviceSwitching = CapabilityAvailability::Unavailable;
        report.deviceSwitchingReason = "JACK audio output is not initialized.";
        report.latencyControl = CapabilityAvailability::Unavailable;
        report.latencyControlReason = "JACK audio output is not initialized.";
        return report;
    }

    const StreamState state = m_output->state();
    const StreamDiagnostics diag = m_output->diagnostics();

    if (state == StreamState::Unconnected || state == StreamState::Error) {
        const std::string err = m_output->lastError();
        const std::string reason = err.empty()
                                       ? "PipeWire-JACK server is not connected."
                                       : err;
        report.directMode = CapabilityAvailability::Unavailable;
        report.directModeReason = reason;
        report.sampleRateFollowing = CapabilityAvailability::Unavailable;
        report.sampleRateFollowingReason = reason;
        report.deviceSwitching = CapabilityAvailability::Unavailable;
        report.deviceSwitchingReason = reason;
        report.latencyControl = CapabilityAvailability::Unavailable;
        report.latencyControlReason = reason;
        return report;
    }

    report.deviceSwitching = CapabilityAvailability::Supported;
    report.deviceSwitchingReason = "Port-based output switching via JACK playback ports.";
    report.latencyControl = m_output->latencyControlSupported()
                                ? CapabilityAvailability::Supported
                                : CapabilityAvailability::Unsupported;
    report.latencyControlReason = m_output->latencyControlSupported()
                                      ? "Dynamic quantum/buffer update via jack_set_buffer_size()."
                                      : "PipeWire-JACK server rejected jack_set_buffer_size() request.";

    if (offlineSimulation) {
        report.directMode = (diag.directModeStatus == DirectModeStatus::Active)
                                ? CapabilityAvailability::Supported
                                : CapabilityAvailability::Unknown;
        report.sampleRateFollowing = report.directMode;
        if (report.directMode != CapabilityAvailability::Supported) {
            report.directModeReason = "Offline JACK simulation mode.";
            report.sampleRateFollowingReason = "Offline JACK simulation mode.";
        }
        return report;
    }

    switch (diag.directModeStatus) {
    case DirectModeStatus::Active:
        report.directMode = CapabilityAvailability::Supported;
        report.sampleRateFollowing = CapabilityAvailability::Supported;
        report.directModeReason = "Measured JACK graph sample rate matches requested rate (" +
                                  std::to_string(diag.sampleRate) + " Hz).";
        report.sampleRateFollowingReason = report.directModeReason;
        break;
    case DirectModeStatus::Disabled:
        report.directMode = CapabilityAvailability::Unavailable;
        report.sampleRateFollowing = CapabilityAvailability::Unavailable;
        report.directModeReason = "PipeWire-JACK server remained at " + std::to_string(diag.sampleRate) +
                                  " Hz while track/requested rate is " +
                                  std::to_string(diag.requestedSampleRate) + " Hz.";
        report.sampleRateFollowingReason = report.directModeReason;
        break;
    case DirectModeStatus::Unknown:
    default:
        report.directMode = CapabilityAvailability::Unknown;
        report.sampleRateFollowing = CapabilityAvailability::Unknown;
        report.directModeReason = "JACK server sample rate alignment not yet measured.";
        report.sampleRateFollowingReason = report.directModeReason;
        break;
    }

    return report;
}

} // namespace desk32::engine::backend
