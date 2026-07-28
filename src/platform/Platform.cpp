#include "iramix/platform/Platform.hpp"

namespace iramix::platform {

std::string_view operatingSystemName() noexcept {
    switch (currentOperatingSystem()) {
    case OperatingSystem::Windows:
        return "Windows";
    case OperatingSystem::MacOS:
        return "macOS";
    case OperatingSystem::Linux:
        return "Linux";
    }

    return "Unknown";
}

} // namespace iramix::platform

