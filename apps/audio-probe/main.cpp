#include "AudioProbe.hpp"
#include "iramix/realtime/Audit.hpp"

#include <charconv>
#include <cstdint>
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

    std::uint32_t secondsPerBuffer = 200U;
    if (argc == 3 && std::string_view {argv[1]} == "--seconds-per-buffer") {
        if (!parseSeconds(argv[2], secondsPerBuffer)) {
            std::cerr << "Invalid duration.\n";
            return 2;
        }
    } else if (argc != 1) {
        std::cerr
            << "Usage: iramix_audio_probe "
            << "[--seconds-per-buffer N | --self-test]\n";
        return 2;
    }

    if (!iramix::realtime::verifyAuditHooks()) {
        std::cerr << "Real-time audit hook self-test failed.\n";
        return 1;
    }
    return iramix::audio_probe::run(secondsPerBuffer);
}
