#include "iramix/plugin/PluginBridge.hpp"
#include "iramix/plugin/Vst3Host.hpp"

#include <algorithm>
#include <atomic>
#include <cstring>
#include <iostream>
#include <new>
#include <thread>
#include <vector>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <cerrno>
#include <csignal>
#include <fcntl.h>
#include <semaphore.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace iramix::plugin {
namespace {

constexpr std::uint32_t kMaximumChannelCount = 64U;
constexpr std::uint32_t kMaximumFrames = 16'384U;
constexpr std::uint32_t kControlMagic = 0x49'52'50'42U; // "IRPB"
// Matches kMaximumPluginStateBytes in the session schema: a blob the
// persistence layer would refuse to store must not be transferable either.
constexpr std::uint32_t kMaximumStateBytes = 16'777'216U;

constexpr std::uint32_t kStateRequestNone = 0U;
constexpr std::uint32_t kStateRequestRestore = 1U;
constexpr std::uint32_t kStateRequestCapture = 2U;

constexpr std::uint32_t kStateResultOk = 0U;
constexpr std::uint32_t kStateResultRejected = 1U;

constexpr std::uint32_t kMaximumParameterQueueCapacity = 4'096U;

// One control-thread-to-plugin parameter change. Plain fixed-width members
// only: the host writes these, the child reads them, and publication is the
// release store on the write index rather than anything in the record.
struct SharedParameterEvent final {
    std::uint64_t sampleTime;
    std::uint32_t parameterId;
    float value;
};

[[nodiscard]] std::uint32_t roundUpToPowerOfTwo(std::uint32_t value)
    noexcept {
    std::uint32_t result = 1U;
    while (result < value) {
        result <<= 1U;
    }
    return result;
}

#if defined(_WIN32)
// A VST3 module path routinely contains spaces ("Program Files"), and the
// stub-mode arguments never needed quoting, so the original command-line
// builder never had to handle it. Follows the same backslash/quote escaping
// the Windows CRT uses to parse argv back out of the command line.
[[nodiscard]] std::string quoteWindowsArgument(const std::string& argument) {
    if (!argument.empty()
        && argument.find_first_of(" \t\n\v\"") == std::string::npos) {
        return argument;
    }
    std::string quoted = "\"";
    std::size_t backslashes = 0U;
    for (const char character : argument) {
        if (character == '\\') {
            ++backslashes;
            continue;
        }
        if (character == '"') {
            quoted.append(backslashes * 2U + 1U, '\\');
            backslashes = 0U;
            quoted += '"';
            continue;
        }
        quoted.append(backslashes, '\\');
        backslashes = 0U;
        quoted += character;
    }
    quoted.append(backslashes * 2U, '\\');
    quoted += '"';
    return quoted;
}
#endif

// Laid out for cross-process sharing: fixed-width, lock-free atomics only,
// no pointers, no virtuals. Both processes map the same bytes.
struct SharedControl final {
    std::atomic<std::uint32_t> magic;
    std::atomic<std::uint32_t> maximumFrames;
    std::atomic<std::uint32_t> channelCount;
    std::atomic<std::uint32_t> shutdown;
    std::atomic<std::uint32_t> requestSequence;
    std::atomic<std::uint32_t> completionSequence;
    std::atomic<std::uint32_t> frameCount;
    std::atomic<std::uint32_t> childReady;
    std::atomic<std::uint32_t> parentProcessId;
    // State transfer rides its own sequence pair rather than the audio one:
    // the child applies a blob strictly between blocks, so audio is never
    // rendered against a half-restored plugin.
    std::atomic<std::uint32_t> maximumStateBytes;
    std::atomic<std::uint32_t> stateBytes;
    std::atomic<std::uint32_t> stateRequest;
    std::atomic<std::uint32_t> stateSequence;
    std::atomic<std::uint32_t> stateCompletion;
    std::atomic<std::uint32_t> stateResult;
    // Single-producer/single-consumer parameter ring. The host's control
    // thread is the only writer, the plugin process the only reader, and
    // the audio thread touches neither index.
    std::atomic<std::uint32_t> parameterCapacity;
    std::atomic<std::uint32_t> parameterWrite;
    std::atomic<std::uint32_t> parameterRead;
    std::atomic<std::uint64_t> blockStartSample;
    std::atomic<std::uint64_t> parametersApplied;
    std::atomic<std::uint64_t> parametersLate;
    // Written once by a "vst3" child before childReady, never after:
    // enough for the host to address a real parameter by its own ID
    // through the existing setParameter() transport, without a general
    // per-parameter enumeration protocol across the process boundary.
    // Zero for the stand-in and for a plugin with no IEditController.
    std::atomic<std::uint32_t> vst3ParameterCount;
    std::atomic<std::uint32_t> vst3FirstParameterId;
    // Bit pattern of a float, not a float atomic: keeps this struct's
    // lock-free guarantee resting on the uint32/uint64 specializations
    // already asserted below rather than adding a new one.
    std::atomic<std::uint32_t> vst3FirstParameterDefaultBits;
};

static_assert(
    std::atomic<std::uint32_t>::is_always_lock_free
        && std::atomic<std::uint64_t>::is_always_lock_free,
    "cross-process control block requires lock-free atomics"
);

// The parameter ring is placed directly after the audio area, so its
// alignment depends on both being whole multiples of the event alignment.
static_assert(
    sizeof(SharedControl) % alignof(SharedParameterEvent) == 0U
        && (sizeof(float) * 2U) % alignof(SharedParameterEvent) == 0U,
    "parameter events must land aligned in the shared region"
);

[[nodiscard]] std::size_t regionBytes(
    const std::uint32_t maximumFrames,
    const std::uint32_t channelCount,
    const std::uint32_t maximumStateBytes,
    const std::uint32_t parameterCapacity
) noexcept {
    const auto samples = static_cast<std::size_t>(maximumFrames)
        * static_cast<std::size_t>(channelCount);
    return sizeof(SharedControl) + samples * sizeof(float) * 2U
        + static_cast<std::size_t>(parameterCapacity)
            * sizeof(SharedParameterEvent)
        + static_cast<std::size_t>(maximumStateBytes);
}

[[nodiscard]] float* inputBase(SharedControl* const control) noexcept {
    return reinterpret_cast<float*>(
        reinterpret_cast<std::byte*>(control) + sizeof(SharedControl)
    );
}

[[nodiscard]] float* outputBase(
    SharedControl* const control,
    const std::size_t samples
) noexcept {
    return inputBase(control) + samples;
}

[[nodiscard]] SharedParameterEvent* parameterBase(
    SharedControl* const control,
    const std::size_t samples
) noexcept {
    return reinterpret_cast<SharedParameterEvent*>(
        outputBase(control, samples) + samples
    );
}

[[nodiscard]] std::byte* stateBase(
    SharedControl* const control,
    const std::size_t samples,
    const std::uint32_t parameterCapacity
) noexcept {
    return reinterpret_cast<std::byte*>(
        parameterBase(control, samples) + parameterCapacity
    );
}

// macOS caps POSIX shared-memory and semaphore names at 31 characters
// including the leading slash, and reports nothing more specific than a
// failed open when that is exceeded. Names are therefore derived from a
// short token rather than spelled out; Windows has no such limit but uses
// the same token so both sides derive names identically.
[[nodiscard]] std::string sharedMemoryPath(const std::string& token) {
#if defined(_WIN32)
    return "Local\\" + token;
#else
    return "/" + token;
#endif
}

[[nodiscard]] std::string makeToken(
    const std::uint64_t processId,
    const std::uint64_t unique
) {
    static constexpr char digits[] = "0123456789abcdef";
    std::string token = "irx";
    const auto append = [&token](std::uint64_t value) {
        char buffer[17] {};
        std::size_t length = 0U;
        do {
            buffer[length++] = digits[value & 0xFULL];
            value >>= 4U;
        } while (value != 0U && length < sizeof(buffer) - 1U);
        while (length > 0U) {
            token.push_back(buffer[--length]);
        }
    };
    append(processId);
    token.push_back('-');
    append(unique);
    return token;
}

constexpr std::uint32_t kStubStateMagic = 0x49'52'50'53U; // "IRPS"
constexpr std::size_t kStubStateOverhead = 16U; // header 12 + checksum 4

[[nodiscard]] std::uint32_t stateChecksum(
    const std::byte* const bytes,
    const std::size_t length
) noexcept {
    std::uint32_t hash = 2'166'136'261U;
    for (std::size_t index = 0U; index < length; ++index) {
        hash ^= static_cast<std::uint32_t>(bytes[index]);
        hash *= 16'777'619U;
    }
    return hash;
}

void writeLittleEndian(std::byte* const destination, const std::uint32_t value)
    noexcept {
    destination[0] = static_cast<std::byte>(value & 0xFFU);
    destination[1] = static_cast<std::byte>((value >> 8U) & 0xFFU);
    destination[2] = static_cast<std::byte>((value >> 16U) & 0xFFU);
    destination[3] = static_cast<std::byte>((value >> 24U) & 0xFFU);
}

[[nodiscard]] std::uint32_t readLittleEndian(const std::byte* const source)
    noexcept {
    return static_cast<std::uint32_t>(source[0])
        | (static_cast<std::uint32_t>(source[1]) << 8U)
        | (static_cast<std::uint32_t>(source[2]) << 16U)
        | (static_cast<std::uint32_t>(source[3]) << 24U);
}

[[nodiscard]] std::string semaphorePath(const std::string& token) {
#if defined(_WIN32)
    return "Local\\" + token + "e";
#else
    return "/" + token + "s";
#endif
}

} // namespace

namespace stub {

std::vector<std::byte> encodeState(
    const float gain,
    const std::span<const std::byte> payload
) {
    std::vector<std::byte> blob(kStubStateOverhead + payload.size());
    std::uint32_t gainBits = 0U;
    std::memcpy(&gainBits, &gain, sizeof(gainBits));
    writeLittleEndian(blob.data(), kStubStateMagic);
    writeLittleEndian(blob.data() + 4U, gainBits);
    writeLittleEndian(
        blob.data() + 8U,
        static_cast<std::uint32_t>(payload.size())
    );
    if (!payload.empty()) {
        std::memcpy(blob.data() + 12U, payload.data(), payload.size());
    }
    const auto covered = blob.size() - 4U;
    writeLittleEndian(
        blob.data() + covered,
        stateChecksum(blob.data(), covered)
    );
    return blob;
}

bool decodeStateGain(
    const std::span<const std::byte> state,
    float& gain
) noexcept {
    if (state.size() < kStubStateOverhead
        || readLittleEndian(state.data()) != kStubStateMagic) {
        return false;
    }
    const auto payloadLength = readLittleEndian(state.data() + 8U);
    if (static_cast<std::size_t>(payloadLength) + kStubStateOverhead
        != state.size()) {
        return false;
    }
    const auto covered = state.size() - 4U;
    if (readLittleEndian(state.data() + covered)
        != stateChecksum(state.data(), covered)) {
        return false;
    }
    const auto bits = readLittleEndian(state.data() + 4U);
    float decoded = 0.0F;
    std::memcpy(&decoded, &bits, sizeof(decoded));
    gain = decoded;
    return true;
}

} // namespace stub

struct PluginBridge::Impl final {
    PluginBridgeConfig config {};
    std::string name;
    SharedControl* control {nullptr};
    std::size_t mappedBytes {0U};
    PluginBridgeCounters counters {};
    bool started {false};
    std::uint32_t parameterCapacity {0U};
    std::uint64_t samplePosition {0U};
    // Mirrors the newest queued timestamp so an out-of-order enqueue can be
    // refused without reading an index the plugin process also writes.
    std::uint64_t lastParameterTime {0U};
    bool hasParameterTime {false};

#if defined(_WIN32)
    HANDLE mapping {nullptr};
    HANDLE process {nullptr};
    HANDLE request {nullptr};
#else
    int descriptor {-1};
    ::pid_t process {-1};
    bool owner {false};
    ::sem_t* request {SEM_FAILED};
#endif

    // Wakes the plugin process for one block. The child blocks on this
    // rather than polling, so it costs no core while idle — which is what
    // makes the arrangement viable on a machine with few cores.
    void signalRequest() noexcept {
#if defined(_WIN32)
        if (request != nullptr) {
            SetEvent(request);
        }
#else
        if (request != SEM_FAILED) {
            ::sem_post(request);
        }
#endif
    }

    ~Impl() {
        release();
    }

    void release() noexcept {
        if (control != nullptr) {
#if defined(_WIN32)
            UnmapViewOfFile(control);
#else
            ::munmap(control, mappedBytes);
#endif
            control = nullptr;
        }
#if defined(_WIN32)
        if (request != nullptr) {
            CloseHandle(request);
            request = nullptr;
        }
        if (mapping != nullptr) {
            CloseHandle(mapping);
            mapping = nullptr;
        }
        if (process != nullptr) {
            CloseHandle(process);
            process = nullptr;
        }
#else
        if (request != SEM_FAILED) {
            ::sem_close(request);
            request = SEM_FAILED;
        }
        if (descriptor >= 0) {
            ::close(descriptor);
            descriptor = -1;
        }
        if (owner && !name.empty()) {
            ::shm_unlink(sharedMemoryPath(name).c_str());
            ::sem_unlink(semaphorePath(name).c_str());
            owner = false;
        }
#endif
    }

    [[nodiscard]] std::size_t sampleCount() const noexcept {
        return static_cast<std::size_t>(config.maximumFrames)
            * static_cast<std::size_t>(config.channelCount);
    }

    // Unlike processBlock this may sleep, because state transfer runs on
    // the control thread. It is still bounded: a plugin that never answers
    // delays one save or one load and is then given up on.
    //
    // A responsive plugin answers in microseconds, so a brief yield window
    // comes first. Sleeping immediately would charge every restore the
    // host's timer granularity instead — on Windows that is ~15 ms, which
    // is three orders of magnitude above the actual transfer.
    [[nodiscard]] bool awaitState(const std::uint32_t sequence) noexcept {
        const auto beganAt = std::chrono::steady_clock::now();
        const auto expiry = beganAt + config.stateDeadline;
        const auto spinUntil = beganAt + std::chrono::microseconds {500};
        while (control->stateCompletion.load(std::memory_order_acquire)
            != sequence) {
            const auto now = std::chrono::steady_clock::now();
            if (now >= expiry) {
                return false;
            }
            if (now >= spinUntil) {
                std::this_thread::sleep_for(std::chrono::milliseconds {1});
            } else {
                std::this_thread::yield();
            }
        }
        return true;
    }
};

PluginBridge::PluginBridge(std::unique_ptr<Impl> impl)
    : impl_ {std::move(impl)} {}

PluginBridge::~PluginBridge() {
    stop();
}

std::unique_ptr<PluginBridge> PluginBridge::create(
    const PluginBridgeConfig config,
    std::string& error
) {
    error.clear();
    if (config.maximumFrames == 0U
        || config.maximumFrames > kMaximumFrames
        || config.channelCount == 0U
        || config.channelCount > kMaximumChannelCount
        || config.deadline <= std::chrono::microseconds::zero()
        || config.maximumStateBytes > kMaximumStateBytes
        || (config.maximumStateBytes > 0U
            && config.stateDeadline <= std::chrono::milliseconds::zero())
        || config.parameterQueueCapacity
            > kMaximumParameterQueueCapacity) {
        error = "plugin bridge configuration is out of range";
        return {};
    }

    std::unique_ptr<Impl> impl;
    try {
        impl = std::make_unique<Impl>();
    } catch (const std::bad_alloc&) {
        error = "cannot allocate plugin bridge";
        return {};
    }
    impl->config = config;
    // A power-of-two depth lets the ring mask instead of divide, which is
    // what keeps the enqueue free of anything the control thread must not
    // do while audio runs.
    impl->parameterCapacity = config.parameterQueueCapacity == 0U
        ? 0U
        : roundUpToPowerOfTwo(config.parameterQueueCapacity);

    const auto bytes = regionBytes(
        config.maximumFrames,
        config.channelCount,
        config.maximumStateBytes,
        impl->parameterCapacity
    );
    impl->mappedBytes = bytes;

    // A per-instance name so concurrent bridges, including two in one test
    // run, never collide on the same region.
    static std::atomic<std::uint64_t> counter {0U};
    const auto unique = counter.fetch_add(1U, std::memory_order_relaxed);
#if defined(_WIN32)
    impl->name = makeToken(
        static_cast<std::uint64_t>(GetCurrentProcessId()),
        unique
    );
    impl->mapping = CreateFileMappingA(
        INVALID_HANDLE_VALUE,
        nullptr,
        PAGE_READWRITE,
        0U,
        static_cast<DWORD>(bytes),
        sharedMemoryPath(impl->name).c_str()
    );
    if (impl->mapping == nullptr) {
        error = "cannot create plugin bridge shared memory";
        return {};
    }
    impl->control = static_cast<SharedControl*>(
        MapViewOfFile(impl->mapping, FILE_MAP_ALL_ACCESS, 0U, 0U, bytes)
    );
    if (impl->control == nullptr) {
        error = "cannot map plugin bridge shared memory";
        return {};
    }
    impl->request = CreateEventA(
        nullptr,
        FALSE,
        FALSE,
        semaphorePath(impl->name).c_str()
    );
    if (impl->request == nullptr) {
        error = "cannot create plugin bridge request event";
        return {};
    }
#else
    impl->name = makeToken(
        static_cast<std::uint64_t>(::getpid()),
        unique
    );
    const auto shmPath = sharedMemoryPath(impl->name);
    ::shm_unlink(shmPath.c_str());
    impl->descriptor =
        ::shm_open(shmPath.c_str(), O_CREAT | O_EXCL | O_RDWR, 0600);
    if (impl->descriptor < 0) {
        error = "cannot create plugin bridge shared memory";
        return {};
    }
    impl->owner = true;
    if (::ftruncate(impl->descriptor, static_cast<off_t>(bytes)) != 0) {
        error = "cannot size plugin bridge shared memory";
        return {};
    }
    void* const mapped = ::mmap(
        nullptr,
        bytes,
        PROT_READ | PROT_WRITE,
        MAP_SHARED,
        impl->descriptor,
        0
    );
    if (mapped == MAP_FAILED) {
        error = "cannot map plugin bridge shared memory";
        return {};
    }
    impl->control = static_cast<SharedControl*>(mapped);

    const auto semaphoreName = semaphorePath(impl->name);
    ::sem_unlink(semaphoreName.c_str());
    impl->request =
        ::sem_open(semaphoreName.c_str(), O_CREAT | O_EXCL, 0600, 0U);
    if (impl->request == SEM_FAILED) {
        error = "cannot create plugin bridge request semaphore";
        return {};
    }
#endif

    // Construct the control block in place rather than memset over
    // atomics, then zero the audio area, which is plain floats.
    auto* const raw = static_cast<void*>(impl->control);
    impl->control = new (raw) SharedControl {};
    std::memset(
        reinterpret_cast<std::byte*>(raw) + sizeof(SharedControl),
        0,
        bytes - sizeof(SharedControl)
    );
    impl->control->maximumFrames.store(
        config.maximumFrames,
        std::memory_order_relaxed
    );
    impl->control->channelCount.store(
        config.channelCount,
        std::memory_order_relaxed
    );
    impl->control->maximumStateBytes.store(
        config.maximumStateBytes,
        std::memory_order_relaxed
    );
    impl->control->parameterCapacity.store(
        impl->parameterCapacity,
        std::memory_order_relaxed
    );
#if defined(_WIN32)
    impl->control->parentProcessId.store(
        static_cast<std::uint32_t>(GetCurrentProcessId()),
        std::memory_order_relaxed
    );
#else
    impl->control->parentProcessId.store(
        static_cast<std::uint32_t>(::getpid()),
        std::memory_order_relaxed
    );
#endif
    impl->control->magic.store(kControlMagic, std::memory_order_release);

    return std::unique_ptr<PluginBridge> {
        new PluginBridge {std::move(impl)}
    };
}

bool PluginBridge::startWithArguments(
    const std::filesystem::path& childExecutable,
    const std::vector<std::string>& arguments,
    std::string& error
) {
    error.clear();
    if (impl_->started) {
        error = "plugin bridge already started";
        return false;
    }

#if defined(_WIN32)
    std::string commandLine = quoteWindowsArgument(childExecutable.string());
    for (const auto& argument : arguments) {
        commandLine += ' ';
        commandLine += quoteWindowsArgument(argument);
    }
    std::vector<char> mutableCommand {
        commandLine.begin(),
        commandLine.end(),
    };
    mutableCommand.push_back('\0');
    STARTUPINFOA startup {};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION process {};
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
        error = "cannot launch plugin host process";
        return false;
    }
    CloseHandle(process.hThread);
    impl_->process = process.hProcess;
#else
    const auto child = ::fork();
    if (child < 0) {
        error = "cannot fork plugin host process";
        return false;
    }
    if (child == 0) {
        // execv wants a mutable argv, but never writes through it; the
        // strings themselves stay owned by this stack frame for the
        // process's short remaining lifetime before exec replaces it.
        const std::string executable = childExecutable.string();
        std::vector<std::string> storage = arguments;
        std::vector<char*> argv;
        argv.push_back(const_cast<char*>(executable.c_str()));
        for (auto& argument : storage) {
            argv.push_back(argument.data());
        }
        argv.push_back(nullptr);
        ::execv(executable.c_str(), argv.data());
        ::_Exit(126);
    }
    impl_->process = child;
#endif

    impl_->started = true;
    return true;
}

bool PluginBridge::start(
    const std::filesystem::path& childExecutable,
    const std::string& childMode,
    std::string& error
) {
    return startWithArguments(
        childExecutable,
        {"--plugin-bridge-child", impl_->name, childMode},
        error
    );
}

bool PluginBridge::startVst3(
    const std::filesystem::path& childExecutable,
    const std::filesystem::path& vst3ModulePath,
    const std::uint32_t vst3ClassIndex,
    const double sampleRate,
    std::string& error
) {
    return startWithArguments(
        childExecutable,
        {
            "--plugin-bridge-child",
            impl_->name,
            "vst3",
            vst3ModulePath.string(),
            std::to_string(vst3ClassIndex),
            std::to_string(sampleRate),
        },
        error
    );
}

bool PluginBridge::startVst3Fault(
    const std::filesystem::path& childExecutable,
    const std::filesystem::path& vst3ModulePath,
    const std::uint32_t vst3ClassIndex,
    const double sampleRate,
    const bool hang,
    std::string& error
) {
    return startWithArguments(
        childExecutable,
        {
            "--plugin-bridge-child",
            impl_->name,
            hang ? "vst3-hang" : "vst3-crash",
            vst3ModulePath.string(),
            std::to_string(vst3ClassIndex),
            std::to_string(sampleRate),
        },
        error
    );
}

void PluginBridge::stop() noexcept {
    if (impl_ == nullptr) {
        return;
    }
    if (impl_->started && impl_->control != nullptr) {
        impl_->control->shutdown.store(1U, std::memory_order_release);
        // Wake a child that is blocked waiting for work so it can observe
        // the shutdown flag instead of sleeping until it is killed.
        impl_->signalRequest();
    }
#if defined(_WIN32)
    if (impl_->process != nullptr) {
        if (WaitForSingleObject(impl_->process, 2'000U) == WAIT_TIMEOUT) {
            // A hung plugin must never hold the host open.
            TerminateProcess(impl_->process, 1U);
            WaitForSingleObject(impl_->process, 1'000U);
        }
    }
#else
    if (impl_->process > 0) {
        for (int attempt = 0; attempt < 200; ++attempt) {
            int status = 0;
            const auto result =
                ::waitpid(impl_->process, &status, WNOHANG);
            if (result == impl_->process || result < 0) {
                impl_->process = -1;
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds {10});
        }
        if (impl_->process > 0) {
            ::kill(impl_->process, SIGKILL);
            int status = 0;
            static_cast<void>(::waitpid(impl_->process, &status, 0));
            impl_->process = -1;
        }
    }
#endif
    impl_->started = false;
    impl_->release();
}

bool PluginBridge::childRunning() const noexcept {
    if (!impl_->started) {
        return false;
    }
#if defined(_WIN32)
    return impl_->process != nullptr
        && WaitForSingleObject(impl_->process, 0U) == WAIT_TIMEOUT;
#else
    if (impl_->process <= 0) {
        return false;
    }
    int status = 0;
    return ::waitpid(impl_->process, &status, WNOHANG) == 0;
#endif
}

std::string PluginBridge::sharedMemoryName() const {
    return impl_->name;
}

PluginBridgeCounters PluginBridge::counters() const noexcept {
    auto snapshot = impl_->counters;
    // The applied and late counts are the plugin's own tally, read out of
    // the shared region rather than inferred host-side. The host cannot
    // know when the plugin actually acted on an event.
    if (impl_->control != nullptr) {
        snapshot.parametersApplied =
            impl_->control->parametersApplied.load(
                std::memory_order_relaxed
            );
        snapshot.parametersLate =
            impl_->control->parametersLate.load(std::memory_order_relaxed);
    }
    return snapshot;
}

std::uint64_t PluginBridge::samplePosition() const noexcept {
    return impl_->samplePosition;
}

PluginParameterStatus PluginBridge::setParameterById(
    const std::uint32_t parameterId,
    const float value,
    const std::uint64_t sampleTime
) noexcept {
    if (!impl_->started
        || impl_->control == nullptr
        || impl_->parameterCapacity == 0U) {
        return PluginParameterStatus::unavailable;
    }
    // Refused rather than reordered: if two changes to the same parameter
    // could be applied in either order, the rendered result stops being
    // reproducible from the session.
    if (impl_->hasParameterTime && sampleTime < impl_->lastParameterTime) {
        ++impl_->counters.parameterOutOfOrder;
        return PluginParameterStatus::outOfOrder;
    }

    auto* const control = impl_->control;
    const auto write =
        control->parameterWrite.load(std::memory_order_relaxed);
    const auto read = control->parameterRead.load(std::memory_order_acquire);
    if (write - read >= impl_->parameterCapacity) {
        // Saturation is reported to the caller. A bounded queue that
        // silently overwrites its oldest entry would lose an automation
        // move with no trace of having done so.
        ++impl_->counters.parameterOverflows;
        return PluginParameterStatus::queueFull;
    }

    auto* const slot = parameterBase(control, impl_->sampleCount())
        + (write & (impl_->parameterCapacity - 1U));
    slot->sampleTime = sampleTime;
    slot->parameterId = parameterId;
    slot->value = value;
    // Publishes the record above: the child acquires this index before it
    // reads the slot.
    control->parameterWrite.store(write + 1U, std::memory_order_release);

    impl_->lastParameterTime = sampleTime;
    impl_->hasParameterTime = true;
    ++impl_->counters.parametersSent;
    return PluginParameterStatus::accepted;
}

PluginParameterStatus PluginBridge::setParameter(
    const PluginParameterId parameter,
    const float value,
    const std::uint64_t sampleTime
) noexcept {
    return setParameterById(
        static_cast<std::uint32_t>(parameter),
        value,
        sampleTime
    );
}

PluginParameterMetadata PluginBridge::parameterMetadata() const noexcept {
    if (impl_->control == nullptr
        || impl_->control->childReady.load(std::memory_order_acquire)
            == 0U) {
        return {};
    }
    PluginParameterMetadata metadata {};
    metadata.count = impl_->control->vst3ParameterCount.load(
        std::memory_order_relaxed
    );
    metadata.firstParameterId = impl_->control->vst3FirstParameterId.load(
        std::memory_order_relaxed
    );
    const auto bits = impl_->control->vst3FirstParameterDefaultBits.load(
        std::memory_order_relaxed
    );
    std::memcpy(
        &metadata.firstParameterDefault,
        &bits,
        sizeof(metadata.firstParameterDefault)
    );
    return metadata;
}

PluginStateStatus PluginBridge::restoreState(
    const std::span<const std::byte> state
) {
    if (!impl_->started
        || impl_->control == nullptr
        || impl_->config.maximumStateBytes == 0U) {
        return PluginStateStatus::unavailable;
    }
    if (state.size() > impl_->config.maximumStateBytes) {
        // Refused before anything is written: an oversized blob must not
        // reach the shared region at all.
        return PluginStateStatus::tooLarge;
    }

    auto* const control = impl_->control;
    if (!state.empty()) {
        std::memcpy(
            stateBase(
                control,
                impl_->sampleCount(),
                impl_->parameterCapacity
            ),
            state.data(),
            state.size()
        );
    }
    control->stateBytes.store(
        static_cast<std::uint32_t>(state.size()),
        std::memory_order_relaxed
    );
    control->stateRequest.store(
        kStateRequestRestore,
        std::memory_order_relaxed
    );
    const auto sequence =
        control->stateSequence.load(std::memory_order_relaxed) + 1U;
    control->stateSequence.store(sequence, std::memory_order_release);
    impl_->signalRequest();

    if (!impl_->awaitState(sequence)) {
        ++impl_->counters.stateTimeouts;
        return PluginStateStatus::timedOut;
    }
    if (control->stateResult.load(std::memory_order_acquire)
        != kStateResultOk) {
        ++impl_->counters.stateRejections;
        return PluginStateStatus::rejectedByPlugin;
    }
    ++impl_->counters.stateRestores;
    return PluginStateStatus::ok;
}

PluginStateStatus PluginBridge::captureState(
    std::vector<std::byte>& state
) {
    state.clear();
    if (!impl_->started
        || impl_->control == nullptr
        || impl_->config.maximumStateBytes == 0U) {
        return PluginStateStatus::unavailable;
    }

    auto* const control = impl_->control;
    control->stateBytes.store(0U, std::memory_order_relaxed);
    control->stateRequest.store(
        kStateRequestCapture,
        std::memory_order_relaxed
    );
    const auto sequence =
        control->stateSequence.load(std::memory_order_relaxed) + 1U;
    control->stateSequence.store(sequence, std::memory_order_release);
    impl_->signalRequest();

    if (!impl_->awaitState(sequence)) {
        ++impl_->counters.stateTimeouts;
        return PluginStateStatus::timedOut;
    }
    if (control->stateResult.load(std::memory_order_acquire)
        != kStateResultOk) {
        ++impl_->counters.stateRejections;
        return PluginStateStatus::rejectedByPlugin;
    }

    const auto produced = std::min(
        static_cast<std::size_t>(
            control->stateBytes.load(std::memory_order_acquire)
        ),
        static_cast<std::size_t>(impl_->config.maximumStateBytes)
    );
    try {
        state.resize(produced);
    } catch (const std::bad_alloc&) {
        return PluginStateStatus::unavailable;
    }
    if (produced != 0U) {
        std::memcpy(
            state.data(),
            stateBase(
                control,
                impl_->sampleCount(),
                impl_->parameterCapacity
            ),
            produced
        );
    }
    ++impl_->counters.stateCaptures;
    return PluginStateStatus::ok;
}

PluginBlockStatus PluginBridge::processBlock(
    const std::span<const float> interleavedInput,
    const std::span<float> interleavedOutput,
    const std::uint32_t frameCount
) noexcept {
    const auto silence = [&interleavedOutput] {
        std::fill(
            interleavedOutput.begin(),
            interleavedOutput.end(),
            0.0F
        );
    };

    if (!impl_->started || impl_->control == nullptr) {
        silence();
        return PluginBlockStatus::notRunning;
    }
    const auto samples = static_cast<std::size_t>(frameCount)
        * static_cast<std::size_t>(impl_->config.channelCount);
    if (frameCount == 0U
        || frameCount > impl_->config.maximumFrames
        || interleavedInput.size() < samples
        || interleavedOutput.size() < samples) {
        silence();
        return PluginBlockStatus::invalidBlock;
    }

    auto* const control = impl_->control;
    std::memcpy(
        inputBase(control),
        interleavedInput.data(),
        samples * sizeof(float)
    );
    control->frameCount.store(frameCount, std::memory_order_relaxed);
    // Published before the request sequence, so the child sees the window
    // it must schedule parameter events against.
    control->blockStartSample.store(
        impl_->samplePosition,
        std::memory_order_relaxed
    );
    // Transport advances even when the plugin misses: a stalled plugin
    // must not rewind the timeline the rest of the session is on.
    impl_->samplePosition += frameCount;
    const auto sequence =
        control->requestSequence.load(std::memory_order_relaxed) + 1U;
    control->requestSequence.store(sequence, std::memory_order_release);
    impl_->signalRequest();

    // Bounded spin rather than a blocking wait: no syscall, no lock, and a
    // hung child costs exactly one deadline, never an unbounded stall. The
    // cost is a burned core while waiting, which a production bridge would
    // avoid with a futex; see the evidence document.
    const auto expiry = std::chrono::steady_clock::now()
        + impl_->config.deadline;
    while (control->completionSequence.load(std::memory_order_acquire)
        != sequence) {
        if (std::chrono::steady_clock::now() >= expiry) {
            silence();
            ++impl_->counters.deadlineMisses;
            ++impl_->counters.consecutiveDeadlineMisses;
            return childRunning()
                ? PluginBlockStatus::deadlineMissed
                : PluginBlockStatus::processExited;
        }
    }

    std::memcpy(
        interleavedOutput.data(),
        outputBase(control, impl_->sampleCount()),
        samples * sizeof(float)
    );
    ++impl_->counters.processedBlocks;
    impl_->counters.consecutiveDeadlineMisses = 0U;
    return PluginBlockStatus::processed;
}

int PluginBridge::runChild(
    const std::string& sharedMemoryName,
    const std::string& mode,
    const std::string& vst3ModulePath,
    const std::uint32_t vst3ClassIndex,
    const double vst3SampleRate
) {
    SharedControl* control = nullptr;
    std::size_t bytes = 0U;

#if defined(_WIN32)
    const HANDLE mapping = OpenFileMappingA(
        FILE_MAP_ALL_ACCESS,
        FALSE,
        sharedMemoryPath(sharedMemoryName).c_str()
    );
    if (mapping == nullptr) {
        return 20;
    }
    auto* header = static_cast<SharedControl*>(
        MapViewOfFile(
            mapping,
            FILE_MAP_ALL_ACCESS,
            0U,
            0U,
            sizeof(SharedControl)
        )
    );
    if (header == nullptr) {
        CloseHandle(mapping);
        return 21;
    }
    if (header->magic.load(std::memory_order_acquire) != kControlMagic) {
        UnmapViewOfFile(header);
        CloseHandle(mapping);
        return 22;
    }
    bytes = regionBytes(
        header->maximumFrames.load(std::memory_order_relaxed),
        header->channelCount.load(std::memory_order_relaxed),
        header->maximumStateBytes.load(std::memory_order_relaxed),
        header->parameterCapacity.load(std::memory_order_relaxed)
    );
    UnmapViewOfFile(header);
    control = static_cast<SharedControl*>(
        MapViewOfFile(mapping, FILE_MAP_ALL_ACCESS, 0U, 0U, bytes)
    );
    if (control == nullptr) {
        CloseHandle(mapping);
        return 23;
    }
    const HANDLE request = OpenEventA(
        SYNCHRONIZE,
        FALSE,
        semaphorePath(sharedMemoryName).c_str()
    );
    if (request == nullptr) {
        UnmapViewOfFile(control);
        CloseHandle(mapping);
        return 24;
    }
#else
    const int descriptor = ::shm_open(
        sharedMemoryPath(sharedMemoryName).c_str(),
        O_RDWR,
        0
    );
    if (descriptor < 0) {
        return 20;
    }
    auto* header = static_cast<SharedControl*>(
        ::mmap(
            nullptr,
            sizeof(SharedControl),
            PROT_READ | PROT_WRITE,
            MAP_SHARED,
            descriptor,
            0
        )
    );
    if (header == MAP_FAILED) {
        ::close(descriptor);
        return 21;
    }
    if (header->magic.load(std::memory_order_acquire) != kControlMagic) {
        ::munmap(header, sizeof(SharedControl));
        ::close(descriptor);
        return 22;
    }
    bytes = regionBytes(
        header->maximumFrames.load(std::memory_order_relaxed),
        header->channelCount.load(std::memory_order_relaxed),
        header->maximumStateBytes.load(std::memory_order_relaxed),
        header->parameterCapacity.load(std::memory_order_relaxed)
    );
    ::munmap(header, sizeof(SharedControl));
    void* const mapped = ::mmap(
        nullptr,
        bytes,
        PROT_READ | PROT_WRITE,
        MAP_SHARED,
        descriptor,
        0
    );
    ::close(descriptor);
    if (mapped == MAP_FAILED) {
        return 23;
    }
    control = static_cast<SharedControl*>(mapped);
    ::sem_t* const request =
        ::sem_open(semaphorePath(sharedMemoryName).c_str(), 0);
    if (request == SEM_FAILED) {
        ::munmap(control, bytes);
        return 24;
    }
#endif

    const auto maximumFrames =
        control->maximumFrames.load(std::memory_order_relaxed);
    const auto channelCount =
        control->channelCount.load(std::memory_order_relaxed);
    const auto capacity = static_cast<std::size_t>(maximumFrames)
        * static_cast<std::size_t>(channelCount);
    const auto stateCapacity = static_cast<std::size_t>(
        control->maximumStateBytes.load(std::memory_order_relaxed)
    );
    const auto parameterCapacity =
        control->parameterCapacity.load(std::memory_order_relaxed);

    // Live state of the stand-in plugin. The default stands in for a
    // freshly instantiated plugin that has never been given a blob.
    float gain = 0.5F;
    bool bypassed = false;
    std::vector<std::byte> payload;

    // Real hosting, not the stand-in: every block runs through this
    // plugin's own DSP and its own state format. An open failure here is
    // reported the same way a crashed child is — by exiting before the
    // request loop — because the host has no channel back to this process
    // other than watching whether it kept running. "vst3-crash" and
    // "vst3-hang" host the same real plugin and additionally inject a
    // fault after a few genuine blocks, so the crash/hang recovery
    // contract is proven against a process that was actually running
    // third-party code, not only the synthetic stand-in.
    const bool isVst3 =
        mode == "vst3" || mode == "vst3-crash" || mode == "vst3-hang";
    Vst3Host vst3Host;
    bool vst3Opened = false;
    if (isVst3) {
        std::string openError;
        vst3Opened = vst3Host.open(
            vst3ModulePath,
            vst3ClassIndex,
            maximumFrames,
            channelCount,
            vst3SampleRate,
            openError
        );
        if (!vst3Opened) {
            std::cerr
                << "plugin-bridge-child: vst3 open failed: "
                << openError << "\n";
#if defined(_WIN32)
            CloseHandle(request);
            UnmapViewOfFile(control);
            CloseHandle(mapping);
#else
            ::sem_close(request);
            ::munmap(control, bytes);
#endif
            return 30;
        }
        // Enough for the host to address a real parameter by ID, without
        // a general enumeration protocol: the count, and the first
        // parameter's own ID and default. Written once, before
        // childReady, so there is no window where the host could observe
        // a ready child with stale or half-written metadata.
        Vst3ParameterInfo firstParameter {};
        const auto hasParameter =
            vst3Host.parameterInfo(0U, firstParameter);
        control->vst3ParameterCount.store(
            vst3Host.info().parameterCount,
            std::memory_order_relaxed
        );
        if (hasParameter) {
            std::uint32_t defaultBits = 0U;
            std::memcpy(
                &defaultBits,
                &firstParameter.defaultNormalizedValue,
                sizeof(defaultBits)
            );
            control->vst3FirstParameterId.store(
                firstParameter.id,
                std::memory_order_relaxed
            );
            control->vst3FirstParameterDefaultBits.store(
                defaultBits,
                std::memory_order_relaxed
            );
        }
    }

    control->childReady.store(1U, std::memory_order_release);

    // A plugin process must never outlive its host. The main loop blocks on
    // the request semaphore, which a dead host can no longer post, so
    // orphan detection runs on its own thread rather than by polling the
    // fast path. Without this a crashed host would leave a child blocked
    // forever, which in CI is an indefinitely hung job.
    const auto parentId =
        control->parentProcessId.load(std::memory_order_relaxed);
    std::thread watchdog {[parentId] {
        for (;;) {
            std::this_thread::sleep_for(std::chrono::seconds {1});
#if defined(_WIN32)
            const HANDLE parent = OpenProcess(
                SYNCHRONIZE,
                FALSE,
                static_cast<DWORD>(parentId)
            );
            if (parent == nullptr) {
                ::_Exit(0);
            }
            const auto state = WaitForSingleObject(parent, 0U);
            CloseHandle(parent);
            if (state != WAIT_TIMEOUT) {
                ::_Exit(0);
            }
#else
            if (static_cast<std::uint32_t>(::getppid()) != parentId) {
                ::_Exit(0);
            }
#endif
        }
    }};
    watchdog.detach();

    std::uint32_t handled = 0U;
    std::uint32_t lastSequence = 0U;
    std::uint32_t lastStateSequence = 0U;
    while (control->shutdown.load(std::memory_order_acquire) == 0U) {
        // Block rather than poll. A polling child costs a core, and on a
        // two-core machine that is the core the host is spinning on: an
        // earlier yield-based version of this loop dropped 23% of blocks
        // past their deadline for exactly that reason.
#if defined(_WIN32)
        WaitForSingleObject(request, INFINITE);
#else
        while (::sem_wait(request) != 0 && errno == EINTR) {
        }
#endif
        if (control->shutdown.load(std::memory_order_acquire) != 0U) {
            break;
        }

        // State work is serviced in the same loop as audio, which is
        // exactly what guarantees a blob is applied between blocks and
        // never during one.
        const auto stateSequence =
            control->stateSequence.load(std::memory_order_acquire);
        if (stateSequence != lastStateSequence && stateCapacity != 0U) {
            lastStateSequence = stateSequence;
            std::byte* const blob =
                stateBase(control, capacity, parameterCapacity);
            const auto stateKind =
                control->stateRequest.load(std::memory_order_relaxed);
            if (stateKind == kStateRequestRestore) {
                const auto length = std::min(
                    static_cast<std::size_t>(
                        control->stateBytes.load(std::memory_order_relaxed)
                    ),
                    stateCapacity
                );
                const std::span<const std::byte> incoming {blob, length};
                bool accepted = false;
                if (isVst3) {
                    accepted = vst3Opened
                        && vst3Host.loadState(incoming);
                } else {
                    float restored = 0.0F;
                    if (stub::decodeStateGain(incoming, restored)
                        && restored >= 0.0F
                        && restored <= 4.0F) {
                        gain = restored;
                        payload.assign(
                            incoming.begin() + 12,
                            incoming.end() - 4
                        );
                        accepted = true;
                    }
                }
                // A refused blob leaves the previous state in force.
                // Half-applying it would be worse than not loading it.
                control->stateResult.store(
                    accepted ? kStateResultOk : kStateResultRejected,
                    std::memory_order_relaxed
                );
            } else if (stateKind == kStateRequestCapture) {
                bool accepted = false;
                if (isVst3) {
                    std::vector<std::byte> produced;
                    if (vst3Opened
                        && vst3Host.saveState(produced)
                        && produced.size() <= stateCapacity) {
                        std::memcpy(blob, produced.data(), produced.size());
                        control->stateBytes.store(
                            static_cast<std::uint32_t>(produced.size()),
                            std::memory_order_relaxed
                        );
                        accepted = true;
                    }
                } else {
                    const auto produced = stub::encodeState(gain, payload);
                    if (produced.size() <= stateCapacity) {
                        std::memcpy(blob, produced.data(), produced.size());
                        control->stateBytes.store(
                            static_cast<std::uint32_t>(produced.size()),
                            std::memory_order_relaxed
                        );
                        accepted = true;
                    }
                }
                control->stateResult.store(
                    accepted ? kStateResultOk : kStateResultRejected,
                    std::memory_order_relaxed
                );
            }
            control->stateCompletion.store(
                stateSequence,
                std::memory_order_release
            );
        }

        const auto sequence =
            control->requestSequence.load(std::memory_order_acquire);
        if (sequence == lastSequence) {
            continue;
        }
        lastSequence = sequence;

        // Fault injection happens after a few good blocks so a test can
        // observe the healthy path and the failure in one run.
        if (handled == 3U) {
            if (mode == "crash" || mode == "vst3-crash") {
                // Abrupt termination with the region still mapped: the
                // host must survive this without the operating system
                // unwinding anything on its behalf.
                ::_Exit(99);
            }
            if (mode == "hang" || mode == "vst3-hang") {
                for (;;) {
                    std::this_thread::sleep_for(
                        std::chrono::milliseconds {50}
                    );
                }
            }
        }

        const auto frames =
            control->frameCount.load(std::memory_order_relaxed);
        const auto samples = std::min(
            static_cast<std::size_t>(frames)
                * static_cast<std::size_t>(channelCount),
            capacity
        );

        // Parameter events are drained before the block is rendered, and
        // only up to the end of the block's own window. An event scheduled
        // for a later block stays queued rather than being applied early,
        // which is what makes the rendered result a function of the
        // timeline instead of of delivery timing.
        if (parameterCapacity != 0U) {
            const auto blockStart =
                control->blockStartSample.load(std::memory_order_relaxed);
            const auto blockEnd = blockStart + frames;
            auto* const events = parameterBase(control, capacity);
            auto readIndex =
                control->parameterRead.load(std::memory_order_relaxed);
            const auto writeIndex =
                control->parameterWrite.load(std::memory_order_acquire);
            std::uint64_t applied = 0U;
            std::uint64_t late = 0U;
            while (readIndex != writeIndex) {
                const auto& event =
                    events[readIndex & (parameterCapacity - 1U)];
                if (event.sampleTime >= blockEnd) {
                    break;
                }
                // Counted, not hidden: an event that should already have
                // been rendered is a scheduling failure upstream, and the
                // alternative — discarding it — would silently lose an
                // automation move.
                if (event.sampleTime < blockStart) {
                    ++late;
                }
                if (event.parameterId
                    == static_cast<std::uint32_t>(
                        PluginParameterId::bypass
                    )) {
                    // Host-owned regardless of plugin kind: the session
                    // schema keeps bypass off SessionPlugin's DSP state,
                    // so neither the stand-in nor a real plugin's own
                    // IEditController parameter list may claim it.
                    bypassed = event.value != 0.0F;
                } else if (isVst3) {
                    // Any other ID is the real plugin's own parameter,
                    // named through its IEditController — never the
                    // stand-in's synthetic "gain" concept, which does not
                    // apply here.
                    static_cast<void>(vst3Opened
                        && vst3Host.setParameterNormalized(
                            event.parameterId,
                            event.value
                        ));
                } else if (event.parameterId
                    == static_cast<std::uint32_t>(
                        PluginParameterId::gain
                    )) {
                    gain = event.value;
                }
                ++applied;
                ++readIndex;
            }
            if (applied != 0U) {
                control->parameterRead.store(
                    readIndex,
                    std::memory_order_release
                );
                control->parametersApplied.fetch_add(
                    applied,
                    std::memory_order_relaxed
                );
                if (late != 0U) {
                    control->parametersLate.fetch_add(
                        late,
                        std::memory_order_relaxed
                    );
                }
            }
        }
        const float* const source = inputBase(control);
        float* const destination = outputBase(control, capacity);
        if (isVst3) {
            // Bypass stays host-owned even for a real plugin: the session
            // schema keeps it off SessionPlugin's DSP state, so it must
            // never route through the plugin's own processing.
            if (bypassed) {
                std::memcpy(destination, source, samples * sizeof(float));
            } else if (!vst3Opened
                || !vst3Host.process(source, destination, frames)) {
                std::fill_n(destination, samples, 0.0F);
            }
        } else {
            for (std::size_t index = 0U; index < samples; ++index) {
                // Stand-in for plugin DSP: a deterministic, verifiable
                // transform so a test can prove the audio round-tripped
                // through the other process rather than being zeroed. The
                // coefficient comes from restored state and from parameter
                // events, so the same assertion also proves a restored
                // blob or a queued change reached the DSP.
                destination[index] = bypassed
                    ? source[index]
                    : source[index] * gain;
            }
        }
        ++handled;
        control->completionSequence.store(
            sequence,
            std::memory_order_release
        );
    }

#if defined(_WIN32)
    CloseHandle(request);
    UnmapViewOfFile(control);
    CloseHandle(mapping);
#else
    ::sem_close(request);
    ::munmap(control, bytes);
#endif
    return 0;
}

} // namespace iramix::plugin
