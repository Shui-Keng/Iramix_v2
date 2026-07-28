#pragma once

#include <string_view>

namespace iramix::platform {

enum class OperatingSystem {
    Windows,
    MacOS,
    Linux
};

[[nodiscard]] constexpr OperatingSystem currentOperatingSystem() noexcept {
#if defined(_WIN32)
    return OperatingSystem::Windows;
#elif defined(__APPLE__)
    return OperatingSystem::MacOS;
#elif defined(__linux__)
    return OperatingSystem::Linux;
#else
#error "Iramix currently supports only Windows, macOS, and Linux."
#endif
}

[[nodiscard]] std::string_view operatingSystemName() noexcept;

} // namespace iramix::platform

