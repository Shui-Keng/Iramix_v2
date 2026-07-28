#include "iramix/realtime/Audit.hpp"

#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <string>
#include <string_view>

namespace iramix::asio_probe {

int listDrivers();
int run(const std::string& driverName, std::uint32_t seconds);

} // namespace iramix::asio_probe

int main(const int argc, char* argv[]) {
    if (argc == 2 && std::string_view {argv[1]} == "--list-drivers") {
        return iramix::asio_probe::listDrivers();
    }

    std::string driver;
    std::uint32_t seconds = 3U;
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
        } else {
            std::cerr
                << "Usage: iramix_asio_probe --list-drivers | "
                << "--driver NAME [--seconds-per-buffer N]\n";
            return 2;
        }
    }

    if (driver.empty()) {
        std::cerr << "An ASIO driver name is required.\n";
        return 2;
    }
    if (!iramix::realtime::verifyAuditHooks()) {
        std::cerr << "Real-time audit hook self-test failed.\n";
        return 1;
    }
    return iramix::asio_probe::run(driver, seconds);
}
