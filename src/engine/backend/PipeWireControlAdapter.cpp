#include "PipeWireControlAdapter.hpp"

namespace desk32::engine::backend {

std::vector<AudioOutputDeviceInfo> PipeWireControlAdapter::enumerateOutputDevices() const
{
    if (!m_output) {
        return {};
    }
    return m_output->enumerateOutputDevices();
}

std::string PipeWireControlAdapter::currentTargetDevice() const
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

bool PipeWireControlAdapter::setTargetDevice(const std::string& deviceIdOrName, std::string* outError)
{
    if (!m_output) {
        if (outError) *outError = "PipeWire output not initialized";
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
                *outError = "Unknown audio output sink device: " + deviceIdOrName;
            }
            return false;
        }
    } else {
        resolvedName = deviceIdOrName;
    }

    return m_output->setTargetDevice(resolvedName, outError);
}

bool PipeWireControlAdapter::applyLatencySetting(uint32_t bufferFrames, double /*sampleRate*/)
{
    if (!m_output) {
        return false;
    }
    return m_output->setBufferFrames(bufferFrames);
}

bool PipeWireControlAdapter::renegotiateSampleRate(uint32_t targetRate, double& outSilenceMs)
{
    if (!m_output) {
        outSilenceMs = 0.0;
        return false;
    }
    return m_output->renegotiateSampleRate(targetRate, outSilenceMs);
}

bool PipeWireControlAdapter::setPreferredPcmFormat(OutputPcmFormat format)
{
    if (!m_output) {
        return false;
    }
    return m_output->setPreferredFormat(format);
}

OutputPcmFormat PipeWireControlAdapter::preferredPcmFormat() const
{
    return m_output ? m_output->preferredFormat() : OutputPcmFormat::F32LE;
}

StreamDiagnostics PipeWireControlAdapter::diagnostics() const
{
    return m_output ? m_output->diagnostics() : StreamDiagnostics{};
}

StreamState PipeWireControlAdapter::streamState() const
{
    return m_output ? m_output->state() : StreamState::Unconnected;
}

BackendCapabilitiesReport PipeWireControlAdapter::queryCapabilities(bool offlineSimulation,
                                                                    bool streamInitialized) const
{
    BackendCapabilitiesReport report;
    report.backendName = backendName();
    report.exclusiveAccess = CapabilityAvailability::Unsupported; // PipeWire is a shared audio graph
    report.exclusiveAccessReason = "PipeWire operates as a shared desktop audio graph.";
    report.hardwareBitPerfect = CapabilityAvailability::Unknown;  // Hardware DAC path requires Level 3 capture
    report.hardwareBitPerfectReason = "Hardware DAC physical output requires external Level 3 loopback capture.";
    report.externalInsert = CapabilityAvailability::Supported;
    report.offlineRender = CapabilityAvailability::Supported;

    if (!m_output || !streamInitialized) {
        report.directMode = CapabilityAvailability::Unavailable;
        report.directModeReason = "PipeWire audio output is not initialized.";
        report.sampleRateFollowing = CapabilityAvailability::Unavailable;
        report.sampleRateFollowingReason = "PipeWire audio output is not initialized.";
        report.deviceSwitching = CapabilityAvailability::Unavailable;
        report.deviceSwitchingReason = "PipeWire audio output is not initialized.";
        report.latencyControl = CapabilityAvailability::Unavailable;
        report.latencyControlReason = "PipeWire audio output is not initialized.";
        report.formatSelection = CapabilityAvailability::Unavailable;
        report.formatSelectionReason = "PipeWire audio output is not initialized.";
        return report;
    }

    const StreamState state = m_output->state();
    const StreamDiagnostics diag = m_output->diagnostics();

    if (state == StreamState::Unconnected || state == StreamState::Error) {
        report.directMode = CapabilityAvailability::Unavailable;
        report.directModeReason = "PipeWire output stream is unconnected or in error state.";
        report.sampleRateFollowing = CapabilityAvailability::Unavailable;
        report.sampleRateFollowingReason = report.directModeReason;
        report.deviceSwitching = CapabilityAvailability::Supported;
        report.latencyControl = CapabilityAvailability::Supported;
        report.formatSelection = CapabilityAvailability::Supported;
        return report;
    }

    report.deviceSwitching = CapabilityAvailability::Supported;
    report.latencyControl = CapabilityAvailability::Supported;
    report.formatSelection = CapabilityAvailability::Supported;

    if (offlineSimulation) {
        report.directMode = (diag.directModeStatus == DirectModeStatus::Active)
                                ? CapabilityAvailability::Supported
                                : CapabilityAvailability::Unknown;
        report.sampleRateFollowing = (diag.directModeStatus == DirectModeStatus::Active)
                                         ? CapabilityAvailability::Supported
                                         : CapabilityAvailability::Unknown;
        return report;
    }

    switch (diag.directModeStatus) {
    case DirectModeStatus::Active:
        report.directMode = CapabilityAvailability::Supported;
        report.sampleRateFollowing = CapabilityAvailability::Supported;
        break;
    case DirectModeStatus::Disabled:
        report.directMode = CapabilityAvailability::Unavailable;
        report.sampleRateFollowing = CapabilityAvailability::Unavailable;
        break;
    case DirectModeStatus::Unknown:
    default:
        report.directMode = CapabilityAvailability::Unknown;
        report.sampleRateFollowing = CapabilityAvailability::Unknown;
        break;
    }

    return report;
}

} // namespace desk32::engine::backend
