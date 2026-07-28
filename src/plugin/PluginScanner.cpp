#include "iramix/plugin/PluginScanner.hpp"

#include <algorithm>
#include <cstdio>
#include <fstream>
#include <system_error>
#include <thread>
#include <vector>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <dlfcn.h>
#include <csignal>
#include <sys/wait.h>
#include <unistd.h>
#endif

#if defined(IRAMIX_HAS_VST3_SDK)
#include "pluginterfaces/base/ipluginbase.h"
#endif

namespace iramix::plugin {
namespace {

// A VST3 bundle is a directory whose binary lives at a platform-specific
// path inside it. A plain .vst3 file is also legal and still common.
[[nodiscard]] const char* vst3ArchitectureFolder() noexcept {
#if defined(_WIN32)
    return "x86_64-win";
#elif defined(__APPLE__)
    return "MacOS";
#else
    return "x86_64-linux";
#endif
}

[[nodiscard]] bool isRegularFile(const std::filesystem::path& path) noexcept {
    std::error_code code;
    return std::filesystem::is_regular_file(path, code);
}

// Resolves a .vst3 bundle directory to the binary inside it. Returns an
// empty path when the bundle does not contain one for this platform,
// which is a normal outcome — a bundle may ship other architectures only.
[[nodiscard]] std::filesystem::path vst3BinaryInBundle(
    const std::filesystem::path& bundle
) {
    std::error_code code;
    const auto architecture =
        bundle / "Contents" / vst3ArchitectureFolder();
    if (!std::filesystem::is_directory(architecture, code)) {
        return {};
    }
    // The binary is conventionally named after the bundle, but not
    // always, so the first regular file wins and the conventional name is
    // preferred when present.
    const auto preferred = architecture / bundle.filename();
    if (isRegularFile(preferred)) {
        return preferred;
    }
    std::filesystem::path fallback;
    std::filesystem::directory_iterator entries {architecture, code};
    if (code) {
        return {};
    }
    for (const auto& entry : entries) {
        if (isRegularFile(entry.path())
            && (fallback.empty() || entry.path() < fallback)) {
            fallback = entry.path();
        }
    }
    return fallback;
}

void collect(
    const std::filesystem::path& root,
    std::vector<PluginScanCandidate>& found,
    int depth
) {
    // Bounded recursion: a symlink loop under a search root must not turn
    // discovery into an unbounded walk.
    if (depth > 6) {
        return;
    }
    std::error_code code;
    std::filesystem::directory_iterator entries {root, code};
    if (code) {
        return;
    }
    for (const auto& entry : entries) {
        const auto& path = entry.path();
        auto extension = path.extension().string();
        std::transform(
            extension.begin(),
            extension.end(),
            extension.begin(),
            [](const unsigned char character) {
                return static_cast<char>(std::tolower(character));
            }
        );

        const bool directory = entry.is_directory(code) && !code;
        if (extension == ".vst3") {
            if (directory) {
                const auto binary = vst3BinaryInBundle(path);
                if (!binary.empty()) {
                    found.push_back({
                        .modulePath = binary,
                        .bundlePath = path,
                        .format = PluginModuleFormat::vst3,
                    });
                }
                // A .vst3 directory is a bundle, never a folder to
                // descend into looking for more plugins.
                continue;
            }
            if (isRegularFile(path)) {
                found.push_back({
                    .modulePath = path,
                    .bundlePath = path,
                    .format = PluginModuleFormat::vst3,
                });
            }
            continue;
        }
        if (extension == ".clap" && isRegularFile(path)) {
            found.push_back({
                .modulePath = path,
                .bundlePath = path,
                .format = PluginModuleFormat::clap,
            });
            continue;
        }
        if (directory) {
            // Vendors nest plugins one or two folders deep, so an
            // unnested walk would miss most of a real installation.
            collect(path, found, depth + 1);
        }
    }
}

[[nodiscard]] const char* formatToken(const PluginModuleFormat format)
    noexcept {
    return format == PluginModuleFormat::clap ? "clap" : "vst3";
}

#if defined(IRAMIX_HAS_VST3_SDK)
// Deliberately not a general escape: report fields are written by the
// child and read by the parent, and a plugin name containing a newline
// would otherwise corrupt the record boundary.
[[nodiscard]] std::string sanitise(const char* const text) {
    std::string result;
    if (text == nullptr) {
        return result;
    }
    for (const char* cursor = text; *cursor != '\0'; ++cursor) {
        const auto character = static_cast<unsigned char>(*cursor);
        result.push_back(
            character < 0x20U || character == 0x7FU ? ' ' : *cursor
        );
        if (result.size() >= 200U) {
            break;
        }
    }
    return result;
}
#endif

} // namespace

std::vector<PluginScanCandidate> discoverPlugins(
    const std::span<const std::filesystem::path> searchPaths
) {
    std::vector<PluginScanCandidate> found;
    for (const auto& root : searchPaths) {
        std::error_code code;
        if (!std::filesystem::is_directory(root, code)) {
            continue;
        }
        collect(root, found, 0);
    }
    // Deterministic order, so a scan cache and a result document do not
    // depend on directory iteration order.
    std::sort(
        found.begin(),
        found.end(),
        [](const PluginScanCandidate& left,
            const PluginScanCandidate& right) {
            if (left.bundlePath == right.bundlePath) {
                return left.modulePath < right.modulePath;
            }
            return left.bundlePath < right.bundlePath;
        }
    );
    found.erase(
        std::unique(
            found.begin(),
            found.end(),
            [](const PluginScanCandidate& left,
                const PluginScanCandidate& right) {
                return left.modulePath == right.modulePath;
            }
        ),
        found.end()
    );
    return found;
}

int runScanChild(
    const std::string& modulePath,
    const std::string& format,
    const std::string& reportPath
) {
    std::string identifier;
    std::string name;
    std::string vendor;
    std::uint32_t classCount = 0U;
    PluginScanStatus status = PluginScanStatus::loadFailed;

#if defined(_WIN32)
    // Loading an untrusted binary must never become a dialog. Without
    // this, a corrupt or wrong-architecture module raises a modal "bad
    // image" box that blocks until someone clicks it — which in a scan is
    // indistinguishable from a hang, and cost this scanner a full timeout
    // per bad module before it was suppressed.
    SetErrorMode(
        SEM_FAILCRITICALERRORS
        | SEM_NOGPFAULTERRORBOX
        | SEM_NOOPENFILEERRORBOX
    );

    // The plugin's own folder must be searched for its dependencies, or a
    // plugin that ships DLLs beside itself fails to load for a reason that
    // has nothing to do with the plugin.
    const HMODULE module = LoadLibraryExA(
        modulePath.c_str(),
        nullptr,
        LOAD_WITH_ALTERED_SEARCH_PATH
    );
#else
    void* const module = ::dlopen(modulePath.c_str(), RTLD_NOW | RTLD_LOCAL);
#endif

    if (module == nullptr) {
        status = PluginScanStatus::loadFailed;
    } else if (format == "clap") {
#if defined(_WIN32)
        const auto entry = GetProcAddress(module, "clap_entry");
#else
        const auto entry = ::dlsym(module, "clap_entry");
#endif
        // Only the presence of the entry symbol is checked. Decoding a
        // CLAP descriptor needs the CLAP headers to pin the struct layout,
        // and this project does not have them; reading the fields from a
        // remembered layout would report plausible garbage rather than
        // fail, which is worse than reporting nothing.
        status = entry != nullptr
            ? PluginScanStatus::scanned
            : PluginScanStatus::notAPlugin;
    } else {
#if defined(_WIN32)
        const auto factoryEntry =
            GetProcAddress(module, "GetPluginFactory");
#else
        auto* const factoryEntry = ::dlsym(module, "GetPluginFactory");
#endif
        if (factoryEntry == nullptr) {
            status = PluginScanStatus::notAPlugin;
        } else {
            status = PluginScanStatus::scanned;
#if defined(IRAMIX_HAS_VST3_SDK)
            // With the SDK present the class list can be read exactly,
            // against the real declarations rather than a local guess.
#if defined(_WIN32)
            using InitDllProc = bool (*)();
            const auto initDll = reinterpret_cast<InitDllProc>(
                reinterpret_cast<void*>(GetProcAddress(module, "InitDll"))
            );
            if (initDll != nullptr) {
                static_cast<void>(initDll());
            }
#endif
            using FactoryProc = Steinberg::IPluginFactory* (*)();
            const auto getFactory = reinterpret_cast<FactoryProc>(
                reinterpret_cast<void*>(factoryEntry)
            );
            if (auto* const factory = getFactory(); factory != nullptr) {
                Steinberg::PFactoryInfo factoryInfo {};
                if (factory->getFactoryInfo(&factoryInfo)
                    == Steinberg::kResultOk) {
                    vendor = sanitise(factoryInfo.vendor);
                }
                const auto classes = factory->countClasses();
                classCount = classes < 0
                    ? 0U
                    : static_cast<std::uint32_t>(classes);
                for (Steinberg::int32 index = 0; index < classes; ++index) {
                    Steinberg::PClassInfo classInfo {};
                    if (factory->getClassInfo(index, &classInfo)
                        != Steinberg::kResultOk) {
                        continue;
                    }
                    // The first audio processor class names the plugin;
                    // the controller class is an implementation detail the
                    // user never picks from a list.
                    if (std::string {classInfo.category}
                        != "Audio Module Class") {
                        continue;
                    }
                    if (name.empty()) {
                        name = sanitise(classInfo.name);
                        std::string hex;
                        for (const auto byte : classInfo.cid) {
                            static constexpr char digits[] = "0123456789ABCDEF";
                            const auto value =
                                static_cast<unsigned char>(byte);
                            hex.push_back(digits[value >> 4U]);
                            hex.push_back(digits[value & 0x0FU]);
                        }
                        identifier = hex;
                    }
                }
                factory->release();
            }
#endif
        }
    }

    // Written last and only on a clean path: a missing report is how the
    // parent distinguishes a crash from a refusal.
    std::ofstream report {reportPath, std::ios::binary | std::ios::trunc};
    if (!report) {
        return 2;
    }
    report << static_cast<std::uint32_t>(status) << '\n'
        << classCount << '\n'
        << identifier << '\n'
        << name << '\n'
        << vendor << '\n';
    report.flush();
    if (!report) {
        return 3;
    }
    // The module is deliberately not unloaded. A plugin that misbehaves in
    // its unload path would turn a successful scan into a crashed one, and
    // the process is about to exit anyway.
    return 0;
}

PluginScanRecord scanCandidate(
    const PluginScanCandidate& candidate,
    const std::filesystem::path& childExecutable,
    const std::chrono::milliseconds timeout
) {
    PluginScanRecord record {};
    record.bundlePath = candidate.bundlePath;
    record.format = candidate.format;
    record.status = PluginScanStatus::crashed;

    std::error_code code;
    const auto reportPath = std::filesystem::temp_directory_path(code)
        / ("iramix-scan-"
            + std::to_string(
#if defined(_WIN32)
                static_cast<std::uint64_t>(GetCurrentProcessId())
#else
                static_cast<std::uint64_t>(::getpid())
#endif
            )
            + "-"
            + std::to_string(
                std::filesystem::hash_value(candidate.modulePath)
            )
            + ".txt");
    std::filesystem::remove(reportPath, code);

    const auto started = std::chrono::steady_clock::now();

#if defined(_WIN32)
    std::string commandLine = "\"" + childExecutable.string()
        + "\" --plugin-scan-child \"" + candidate.modulePath.string()
        + "\" " + formatToken(candidate.format)
        + " \"" + reportPath.string() + "\"";
    std::vector<char> mutableCommand {
        commandLine.begin(),
        commandLine.end(),
    };
    mutableCommand.push_back('\0');
    STARTUPINFOA startup {};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION process {};
    // Not CREATE_DEFAULT_ERROR_MODE: that would give the child the system
    // default error mode, which *enables* the modal "bad image" dialog. A
    // malformed plugin then waits for a user who is not there and the scan
    // burns its whole timeout on it. The child suppresses error boxes
    // itself; see runScanChild.
    if (!CreateProcessA(
            childExecutable.string().c_str(),
            mutableCommand.data(),
            nullptr,
            nullptr,
            FALSE,
            CREATE_NO_WINDOW,
            nullptr,
            nullptr,
            &startup,
            &process
        )) {
        record.status = PluginScanStatus::loadFailed;
        return record;
    }
    CloseHandle(process.hThread);
    const auto waited = WaitForSingleObject(
        process.hProcess,
        static_cast<DWORD>(timeout.count())
    );
    if (waited == WAIT_TIMEOUT) {
        TerminateProcess(process.hProcess, 1U);
        WaitForSingleObject(process.hProcess, 2'000U);
        CloseHandle(process.hProcess);
        record.status = PluginScanStatus::timedOut;
        record.milliseconds = static_cast<double>(timeout.count());
        std::filesystem::remove(reportPath, code);
        return record;
    }
    CloseHandle(process.hProcess);
#else
    const auto child = ::fork();
    if (child < 0) {
        record.status = PluginScanStatus::loadFailed;
        return record;
    }
    if (child == 0) {
        ::execl(
            childExecutable.c_str(),
            childExecutable.c_str(),
            "--plugin-scan-child",
            candidate.modulePath.c_str(),
            formatToken(candidate.format),
            reportPath.c_str(),
            static_cast<char*>(nullptr)
        );
        ::_Exit(126);
    }
    bool finished = false;
    const auto expiry = started + timeout;
    while (std::chrono::steady_clock::now() < expiry) {
        int state = 0;
        const auto result = ::waitpid(child, &state, WNOHANG);
        if (result == child || result < 0) {
            finished = true;
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds {5});
    }
    if (!finished) {
        ::kill(child, SIGKILL);
        int state = 0;
        static_cast<void>(::waitpid(child, &state, 0));
        record.status = PluginScanStatus::timedOut;
        record.milliseconds = static_cast<double>(timeout.count());
        std::filesystem::remove(reportPath, code);
        return record;
    }
#endif

    record.milliseconds = std::chrono::duration<double, std::milli>(
        std::chrono::steady_clock::now() - started
    ).count();

    std::ifstream report {reportPath, std::ios::binary};
    if (!report) {
        // No report and the process is gone: it died before it could say
        // anything. This is the case the whole design exists for.
        record.status = PluginScanStatus::crashed;
        return record;
    }
    std::uint32_t rawStatus = 0U;
    report >> rawStatus >> record.classCount;
    report.ignore();
    std::getline(report, record.identifier);
    std::getline(report, record.name);
    std::getline(report, record.vendor);
    std::filesystem::remove(reportPath, code);

    if (rawStatus < static_cast<std::uint32_t>(PluginScanStatus::scanned)
        || rawStatus
            > static_cast<std::uint32_t>(PluginScanStatus::timedOut)) {
        record.status = PluginScanStatus::crashed;
        return record;
    }
    record.status = static_cast<PluginScanStatus>(rawStatus);
    return record;
}

} // namespace iramix::plugin
