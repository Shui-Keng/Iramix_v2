#include "iramix/plugin/PluginScanner.hpp"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <string>
#include <string_view>
#include <vector>

namespace {

// Where each platform actually installs plugins. Used only when the caller
// supplies no paths; the scanner itself takes its roots as an argument so
// its layout rules stay testable without an installation.
[[nodiscard]] std::vector<std::filesystem::path> defaultSearchPaths() {
#if defined(_WIN32)
    return {
        "C:/Program Files/Common Files/VST3",
        "C:/Program Files/Common Files/CLAP",
        "C:/Program Files (x86)/Common Files/VST3",
    };
#elif defined(__APPLE__)
    return {
        "/Library/Audio/Plug-Ins/VST3",
        "/Library/Audio/Plug-Ins/CLAP",
    };
#else
    return {
        "/usr/lib/vst3",
        "/usr/lib/clap",
        "/usr/local/lib/vst3",
        "/usr/local/lib/clap",
    };
#endif
}

[[nodiscard]] const char* statusName(
    const iramix::plugin::PluginScanStatus status
) noexcept {
    using Status = iramix::plugin::PluginScanStatus;
    switch (status) {
    case Status::scanned:
        return "scanned";
    case Status::notAPlugin:
        return "not_a_plugin";
    case Status::loadFailed:
        return "load_failed";
    case Status::crashed:
        return "crashed";
    case Status::timedOut:
        return "timed_out";
    }
    return "unknown";
}

} // namespace

int main(const int argc, char* argv[]) {
    namespace plugin = iramix::plugin;

    if (argc == 5
        && std::string_view {argv[1]} == "--plugin-scan-child") {
        return plugin::runScanChild(argv[2], argv[3], argv[4]);
    }

    const auto self = std::filesystem::absolute(argv[0]);
    std::vector<std::filesystem::path> roots;
    bool verbose = false;
    for (int index = 1; index < argc; ++index) {
        const std::string_view argument {argv[index]};
        if (argument == "--verbose") {
            verbose = true;
            continue;
        }
        roots.emplace_back(argument);
    }
    if (roots.empty()) {
        roots = defaultSearchPaths();
    }

    const auto candidates = plugin::discoverPlugins(roots);
    if (candidates.empty()) {
        std::cout
            << "Plugin scan: roots=" << roots.size()
            << ", candidates=0, note=no_plugins_found\n";
        return EXIT_SUCCESS;
    }

    std::size_t scanned = 0U;
    std::size_t named = 0U;
    std::size_t notPlugin = 0U;
    std::size_t loadFailed = 0U;
    std::size_t crashed = 0U;
    std::size_t timedOut = 0U;
    std::size_t clapCount = 0U;
    std::size_t vst3Count = 0U;
    double totalMilliseconds = 0.0;
    double worstMilliseconds = 0.0;

    const auto started = std::chrono::steady_clock::now();
    for (const auto& candidate : candidates) {
        if (candidate.format == plugin::PluginModuleFormat::clap) {
            ++clapCount;
        } else {
            ++vst3Count;
        }
        const auto record = plugin::scanCandidate(
            candidate,
            self,
            std::chrono::seconds {10}
        );
        totalMilliseconds += record.milliseconds;
        worstMilliseconds =
            std::max(worstMilliseconds, record.milliseconds);
        switch (record.status) {
        case plugin::PluginScanStatus::scanned:
            ++scanned;
            if (!record.name.empty()) {
                ++named;
            }
            break;
        case plugin::PluginScanStatus::notAPlugin:
            ++notPlugin;
            break;
        case plugin::PluginScanStatus::loadFailed:
            ++loadFailed;
            break;
        case plugin::PluginScanStatus::crashed:
            ++crashed;
            break;
        case plugin::PluginScanStatus::timedOut:
            ++timedOut;
            break;
        }
        if (verbose) {
            std::cout
                << "  " << statusName(record.status)
                << "  " << record.bundlePath.filename().string()
                << "  classes=" << record.classCount
                << "  name=" << (record.name.empty() ? "-" : record.name)
                << "  vendor="
                << (record.vendor.empty() ? "-" : record.vendor)
                << "  ms=" << record.milliseconds << "\n";
        }
    }
    const double wallMilliseconds =
        std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - started
        ).count();

    std::cout
        << "Plugin scan: candidates=" << candidates.size()
        << ", vst3=" << vst3Count
        << ", clap=" << clapCount
        << ", scanned=" << scanned
        << ", named=" << named
        << ", not_a_plugin=" << notPlugin
        << ", load_failed=" << loadFailed
        << ", crashed=" << crashed
        << ", timed_out=" << timedOut
        << ", host_survived=1"
        << ", worst_module_ms=" << worstMilliseconds
        << ", total_scan_ms=" << wallMilliseconds << "\n";
    return EXIT_SUCCESS;
}
