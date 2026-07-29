#include "iramix/realtime/Audit.hpp"

#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <string>
#include <string_view>
#include <vector>

namespace iramix::asio_probe {

int listDrivers();
int run(
    const std::string& driverName,
    std::uint32_t seconds,
    const std::vector<long>& buffers
);

} // namespace iramix::asio_probe

namespace {

// Only the sizes Phase 0 declares are accepted. A soak is expensive
// enough that a typo silently measuring the wrong period would cost
// hours, so an unrecognized size is refused rather than attempted.
[[nodiscard]] bool parseBuffers(
    const std::string_view text,
    std::vector<long>& buffers
) {
    std::size_t start = 0U;
    while (start <= text.size()) {
        const auto comma = text.find(',', start);
        const auto piece = text.substr(
            start,
            comma == std::string_view::npos
                ? std::string_view::npos
                : comma - start
        );
        if (piece != "64" && piece != "128" && piece != "256") {
            return false;
        }
        buffers.push_back(std::strtol(std::string {piece}.c_str(), nullptr, 10));
        if (comma == std::string_view::npos) {
            break;
        }
        start = comma + 1U;
    }
    return !buffers.empty();
}

} // namespace

int main(const int argc, char* argv[]) {
    if (argc == 2 && std::string_view {argv[1]} == "--list-drivers") {
        return iramix::asio_probe::listDrivers();
    }

    std::string driver;
    std::uint32_t seconds = 3U;
    std::vector<long> buffers;
    for (int index = 1; index < argc; ++index) {
        const std::string_view argument {argv[index]};
        if (argument == "--driver" && index + 1 < argc) {
            driver = argv[++index];
        } else if (
            argument == "--seconds-per-buffer"
            && index + 1 < argc
        ) {
            const auto parsed = std::strtoul(argv[++index], nullptr, 10);
            if (parsed == 0U) {
                std::cerr << "Invalid duration.\n";
                return 2;
            }
            seconds = static_cast<std::uint32_t>(parsed);
        } else if (argument == "--buffers" && index + 1 < argc) {
            if (!parseBuffers(argv[++index], buffers)) {
                std::cerr
                    << "Invalid buffer list; expected a comma-separated "
                    << "subset of 64,128,256.\n";
                return 2;
            }
        } else {
            std::cerr
                << "Usage: iramix_asio_probe --list-drivers | "
                << "--driver NAME [--seconds-per-buffer N] "
                << "[--buffers 64,128,256]\n";
            return 2;
        }
    }
    if (buffers.empty()) {
        buffers = {64L, 128L, 256L};
    }

    if (driver.empty()) {
        std::cerr << "An ASIO driver name is required.\n";
        return 2;
    }
    if (!iramix::realtime::verifyAuditHooks()) {
        std::cerr << "Real-time audit hook self-test failed.\n";
        return 1;
    }
    return iramix::asio_probe::run(driver, seconds, buffers);
}
