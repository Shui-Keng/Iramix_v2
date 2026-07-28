#include "AudioProbe.hpp"
#include "iramix/persistence/ProjectStore.hpp"
#include "iramix/persistence/SessionDocument.hpp"
#include "iramix/realtime/Audit.hpp"

#include <algorithm>
#include <charconv>
#include <cstdint>
#include <filesystem>
#include <iostream>
#include <string_view>

namespace {

bool parseSeconds(
    const std::string_view text,
    std::uint32_t& seconds
) {
    const auto* begin = text.data();
    const auto* end = begin + text.size();
    const auto result = std::from_chars(begin, end, seconds);
    return result.ec == std::errc {} && result.ptr == end && seconds != 0U;
}

} // namespace

int main(const int argc, char* argv[]) {
    if (argc == 2 && std::string_view {argv[1]} == "--self-test") {
        if (!iramix::realtime::verifyAuditHooks()) {
            std::cerr << "Real-time audit hook self-test failed.\n";
            return 1;
        }
        std::cout
            << "Real-time allocation/lock and denormal hooks "
               "verified.\n";
        return 0;
    }

    if (argc == 2 && std::string_view {argv[1]} == "--list-devices") {
        const auto inventory =
            iramix::audio_probe::enumerateDevices();
        if (!inventory.supported) {
            std::cerr
                << "Device enumeration unavailable: "
                << inventory.error << "\n";
            return 3;
        }
        std::cout
            << "device_inventory count=" << inventory.devices.size()
            << "\n";
        for (const auto& device : inventory.devices) {
            std::cout
                << "device id=\"" << device.deviceId << "\""
                << " name=\"" << device.name << "\""
                << " inputs=" << device.inputChannelCount
                << " outputs=" << device.outputChannelCount
                << " buffer_frames=" << device.minimumBufferFrames
                << ".." << device.maximumBufferFrames
                << " rates=";
            for (std::size_t index = 0U;
                 index < device.supportedSampleRates.size();
                 ++index) {
                std::cout
                    << (index == 0U ? "" : ",")
                    << device.supportedSampleRates[index];
            }
            std::cout << "\n";
        }
        return 0;
    }

    // Writes a session whose device record names the hardware currently in
    // use, so --restore-device can be checked against a real capture
    // instead of a hand-written fixture.
    if (argc == 3
        && std::string_view {argv[1]} == "--capture-device") {
        const auto inventory =
            iramix::audio_probe::enumerateDevices();
        if (!inventory.supported) {
            std::cerr
                << "Device enumeration unavailable: "
                << inventory.error << "\n";
            return 3;
        }
        const auto output = std::find_if(
            inventory.devices.begin(),
            inventory.devices.end(),
            [](const iramix::persistence::AvailableAudioDevice& device) {
                return device.outputChannelCount > 0U;
            }
        );
        if (output == inventory.devices.end()) {
            std::cerr << "No output device to capture.\n";
            return 3;
        }

        iramix::persistence::SessionDocument document;
        document.revision = 1U;
        document.tracks = {
            {
                .stableId = 1U,
                .type = iramix::persistence::SessionTrackType::master,
                .gain = 1.0F,
                .color = 0U,
                .name = "Master",
            },
        };
        document.device = {
            .backend = output->backend,
            .sampleRate = output->supportedSampleRates.front(),
            .bufferFrames = output->minimumBufferFrames,
            .inputChannelCount = 0U,
            .outputChannelCount = output->outputChannelCount,
            .inputDeviceId = {},
            .outputDeviceId = output->deviceId,
        };

        std::string error;
        const auto payload =
            iramix::persistence::serializeSessionDocument(
                document,
                error
            );
        if (payload.empty()) {
            std::cerr << "Cannot serialize session: " << error << "\n";
            return 3;
        }
        if (!iramix::persistence::saveProjectSnapshot(
                std::filesystem::path {argv[2]},
                payload,
                error
            )) {
            std::cerr << "Cannot write project: " << error << "\n";
            return 3;
        }
        std::cout
            << "device_capture device=\"" << output->deviceId << "\""
            << " name=\"" << output->name << "\""
            << " rate=" << document.device.sampleRate
            << " buffer=" << document.device.bufferFrames
            << " outputs=" << document.device.outputChannelCount
            << " project=\"" << argv[2] << "\"\n";
        return 0;
    }

    std::uint32_t secondsPerBuffer = 200U;
    if (argc == 3 && std::string_view {argv[1]} == "--seconds-per-buffer") {
        if (!parseSeconds(argv[2], secondsPerBuffer)) {
            std::cerr << "Invalid duration.\n";
            return 2;
        }
    } else if (argc >= 3
        && std::string_view {argv[1]} == "--restore-device") {
        if (argc == 5
            && std::string_view {argv[3]} == "--seconds-per-buffer") {
            if (!parseSeconds(argv[4], secondsPerBuffer)) {
                std::cerr << "Invalid duration.\n";
                return 2;
            }
        } else if (argc != 3) {
            std::cerr
                << "Usage: iramix_audio_probe --restore-device PROJECT "
                << "[--seconds-per-buffer N]\n";
            return 2;
        }
        if (!iramix::realtime::verifyAuditHooks()) {
            std::cerr << "Real-time audit hook self-test failed.\n";
            return 1;
        }
        return iramix::audio_probe::runRestoredDevice(
            std::filesystem::path {argv[2]},
            secondsPerBuffer
        );
    } else if (argc != 1) {
        std::cerr
            << "Usage: iramix_audio_probe "
            << "[--seconds-per-buffer N | --self-test | "
               "--list-devices | --capture-device PROJECT | "
               "--restore-device PROJECT]\n";
        return 2;
    }

    if (!iramix::realtime::verifyAuditHooks()) {
        std::cerr << "Real-time audit hook self-test failed.\n";
        return 1;
    }
    return iramix::audio_probe::run(secondsPerBuffer);
}
