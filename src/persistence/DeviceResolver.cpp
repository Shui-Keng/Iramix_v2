#include "iramix/persistence/DeviceResolver.hpp"

#include <algorithm>
#include <limits>

namespace iramix::persistence {
namespace {

void appendReason(std::string& reason, const std::string& text) {
    if (!reason.empty()) {
        reason += "; ";
    }
    reason += text;
}

[[nodiscard]] const AvailableAudioDevice* findDevice(
    const std::vector<AvailableAudioDevice>& inventory,
    const SessionAudioBackend backend,
    const std::string& deviceId
) noexcept {
    for (const auto& device : inventory) {
        if (device.backend == backend
            && device.deviceId == deviceId) {
            return &device;
        }
    }
    return nullptr;
}

// Inventory order is the caller's preference order, so the first entry on
// a backend is that backend's default device.
[[nodiscard]] const AvailableAudioDevice* findBackendDefault(
    const std::vector<AvailableAudioDevice>& inventory,
    const SessionAudioBackend backend
) noexcept {
    for (const auto& device : inventory) {
        if (device.backend == backend) {
            return &device;
        }
    }
    return nullptr;
}

// Nearest supported rate, preferring the higher one on an exact tie so the
// choice is deterministic rather than dependent on iteration order.
[[nodiscard]] std::uint32_t nearestSampleRate(
    const std::vector<std::uint32_t>& supported,
    const std::uint32_t wanted
) noexcept {
    std::uint32_t best = 0U;
    std::uint64_t bestDistance =
        std::numeric_limits<std::uint64_t>::max();
    for (const auto rate : supported) {
        const std::uint64_t distance = rate > wanted
            ? static_cast<std::uint64_t>(rate - wanted)
            : static_cast<std::uint64_t>(wanted - rate);
        if (distance < bestDistance
            || (distance == bestDistance && rate > best)) {
            best = rate;
            bestDistance = distance;
        }
    }
    return best;
}

} // namespace

DeviceResolution resolveDeviceConfiguration(
    const SessionDeviceConfiguration& stored,
    const std::vector<AvailableAudioDevice>& inventory
) {
    DeviceResolution resolution;

    if (stored.backend == SessionAudioBackend::unspecified) {
        resolution.status = DeviceResolutionStatus::unconfigured;
        resolution.reason =
            "session carried no device configuration";
        return resolution;
    }

    const auto* output = findDevice(
        inventory,
        stored.backend,
        stored.outputDeviceId
    );
    if (output == nullptr) {
        const auto* fallback =
            findBackendDefault(inventory, stored.backend);
        if (fallback == nullptr) {
            // Never move a session to a different audio subsystem on its
            // own: report and select nothing.
            resolution.status =
                DeviceResolutionStatus::unavailableBackend;
            resolution.reason =
                "no device is available on the session audio backend";
            return resolution;
        }
        output = fallback;
        resolution.outputDeviceSubstituted = true;
        appendReason(
            resolution.reason,
            "output device '" + stored.outputDeviceId
                + "' is absent, substituted '" + output->deviceId + "'"
        );
    }

    if (output->supportedSampleRates.empty()) {
        resolution.status =
            DeviceResolutionStatus::unavailableBackend;
        resolution.reason =
            "the selected device reports no usable sample rate";
        return resolution;
    }

    auto& resolved = resolution.resolved;
    resolved.backend = stored.backend;
    resolved.outputDeviceId = output->deviceId;

    // A stored zero means "ask the backend", so adopting a value is not an
    // adjustment of anything the session actually asked for.
    const auto wantedRate = stored.sampleRate;
    if (wantedRate == 0U) {
        resolved.sampleRate = output->supportedSampleRates.front();
    } else {
        resolved.sampleRate =
            nearestSampleRate(output->supportedSampleRates, wantedRate);
        if (resolved.sampleRate != wantedRate) {
            resolution.sampleRateAdjusted = true;
            appendReason(
                resolution.reason,
                "sample rate " + std::to_string(wantedRate)
                    + " is unsupported, using "
                    + std::to_string(resolved.sampleRate)
            );
        }
    }

    if (stored.bufferFrames == 0U) {
        resolved.bufferFrames = output->minimumBufferFrames;
    } else {
        resolved.bufferFrames = std::clamp(
            stored.bufferFrames,
            output->minimumBufferFrames,
            output->maximumBufferFrames
        );
        if (resolved.bufferFrames != stored.bufferFrames) {
            resolution.bufferFramesAdjusted = true;
            appendReason(
                resolution.reason,
                "buffer size " + std::to_string(stored.bufferFrames)
                    + " is out of range, using "
                    + std::to_string(resolved.bufferFrames)
            );
        }
    }

    resolved.outputChannelCount = std::min(
        stored.outputChannelCount,
        output->outputChannelCount
    );
    if (resolved.outputChannelCount != stored.outputChannelCount) {
        resolution.channelCountAdjusted = true;
        appendReason(
            resolution.reason,
            "output channel count reduced to "
                + std::to_string(resolved.outputChannelCount)
        );
    }

    // Input is optional: a session with no stored input device resolves
    // without one rather than acquiring capture hardware it never asked
    // for.
    if (!stored.inputDeviceId.empty()) {
        const auto* input = findDevice(
            inventory,
            stored.backend,
            stored.inputDeviceId
        );
        if (input == nullptr) {
            resolution.inputDeviceSubstituted = true;
            appendReason(
                resolution.reason,
                "input device '" + stored.inputDeviceId
                    + "' is absent, continuing without capture"
            );
        } else {
            resolved.inputDeviceId = input->deviceId;
            resolved.inputChannelCount = std::min(
                stored.inputChannelCount,
                input->inputChannelCount
            );
            if (resolved.inputChannelCount
                != stored.inputChannelCount) {
                resolution.channelCountAdjusted = true;
                appendReason(
                    resolution.reason,
                    "input channel count reduced to "
                        + std::to_string(resolved.inputChannelCount)
                );
            }
        }
    }

    if (resolution.outputDeviceSubstituted
        || resolution.inputDeviceSubstituted) {
        resolution.status = DeviceResolutionStatus::substituted;
    } else if (resolution.sampleRateAdjusted
        || resolution.bufferFramesAdjusted
        || resolution.channelCountAdjusted) {
        resolution.status = DeviceResolutionStatus::adjusted;
    } else {
        resolution.status = DeviceResolutionStatus::restored;
    }
    return resolution;
}

} // namespace iramix::persistence
