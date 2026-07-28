#pragma once

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <span>
#include <string>
#include <vector>

namespace iramix::plugin {

enum class PluginModuleFormat : std::uint32_t {
    clap = 1U,
    vst3 = 2U,
};

// A file on disk that looks like a plugin module. Discovery decides this
// from layout alone and never loads anything: deciding what to load is a
// filesystem question, and loading it is the dangerous part.
struct PluginScanCandidate final {
    std::filesystem::path modulePath;
    // The bundle or file the user would recognise, which for a VST3 bundle
    // is the directory rather than the binary inside it.
    std::filesystem::path bundlePath;
    PluginModuleFormat format {PluginModuleFormat::vst3};
};

enum class PluginScanStatus : std::uint32_t {
    // The module loaded and declared the entry point its format requires.
    scanned = 1U,
    // The module loaded but is not a plugin of the format its name claims.
    notAPlugin = 2U,
    // The operating system refused to load it: wrong architecture, missing
    // dependency, or a corrupt binary.
    loadFailed = 3U,
    // The scan process died. This is the outcome that matters: a plugin
    // that crashes on load must cost one record, not the scan.
    crashed = 4U,
    // The module did not finish loading within the per-plugin budget.
    timedOut = 5U,
};

struct PluginScanRecord final {
    std::filesystem::path bundlePath;
    PluginModuleFormat format {PluginModuleFormat::vst3};
    PluginScanStatus status {PluginScanStatus::loadFailed};
    // Populated only when the format's metadata could be read. Absent
    // metadata is reported as absent rather than guessed at.
    std::string identifier;
    std::string name;
    std::string vendor;
    std::uint32_t classCount {0U};
    double milliseconds {0.0};
};

// Walks the supplied search paths and returns every plugin module found,
// in a deterministic order.
//
// Takes the roots as an argument rather than consulting the platform's
// standard locations itself, for the same reason DeviceResolver takes an
// injected inventory: it makes the layout rules testable against a
// synthetic tree, with no plugins installed and no filesystem of the
// host's shape required.
[[nodiscard]] std::vector<PluginScanCandidate> discoverPlugins(
    std::span<const std::filesystem::path> searchPaths
);

// Loads one candidate in a **separate process** and reports what it found.
// Never loads the module in the calling process: the entire point is that
// an untrusted third-party binary cannot take the scanner down with it.
[[nodiscard]] PluginScanRecord scanCandidate(
    const PluginScanCandidate& candidate,
    const std::filesystem::path& childExecutable,
    std::chrono::milliseconds timeout
);

// Child-process entry point. Loads the module, writes what it learned to
// `reportPath`, and exits. A crash here produces no report, which is
// exactly how the parent detects one.
[[nodiscard]] int runScanChild(
    const std::string& modulePath,
    const std::string& format,
    const std::string& reportPath
);

} // namespace iramix::plugin
