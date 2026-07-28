#include "iramix/plugin/PluginBridge.hpp"

#include <algorithm>
#include <atomic>
#include <cstring>
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
};

static_assert(
    std::atomic<std::uint32_t>::is_always_lock_free,
    "cross-process control block requires lock-free atomics"
);

[[nodiscard]] std::size_t regionBytes(
    const std::uint32_t maximumFrames,
    const std::uint32_t channelCount
) noexcept {
    const auto samples = static_cast<std::size_t>(maximumFrames)
        * static_cast<std::size_t>(channelCount);
    return sizeof(SharedControl) + samples * sizeof(float) * 2U;
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

[[nodiscard]] std::string semaphorePath(const std::string& token) {
#if defined(_WIN32)
    return "Local\\" + token + "e";
#else
    return "/" + token + "s";
#endif
}

} // namespace

struct PluginBridge::Impl final {
    PluginBridgeConfig config {};
    std::string name;
    SharedControl* control {nullptr};
    std::size_t mappedBytes {0U};
    PluginBridgeCounters counters {};
    bool started {false};

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
        || config.deadline <= std::chrono::microseconds::zero()) {
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

    const auto bytes =
        regionBytes(config.maximumFrames, config.channelCount);
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

bool PluginBridge::start(
    const std::filesystem::path& childExecutable,
    const std::string& childMode,
    std::string& error
) {
    error.clear();
    if (impl_->started) {
        error = "plugin bridge already started";
        return false;
    }

#if defined(_WIN32)
    std::string commandLine = "\"" + childExecutable.string()
        + "\" --plugin-bridge-child \"" + impl_->name + "\" "
        + childMode;
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
        ::execl(
            childExecutable.c_str(),
            childExecutable.c_str(),
            "--plugin-bridge-child",
            impl_->name.c_str(),
            childMode.c_str(),
            static_cast<char*>(nullptr)
        );
        ::_Exit(126);
    }
    impl_->process = child;
#endif

    impl_->started = true;
    return true;
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
    return impl_->counters;
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
    const std::string& mode
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
        header->channelCount.load(std::memory_order_relaxed)
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
        header->channelCount.load(std::memory_order_relaxed)
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
        const auto sequence =
            control->requestSequence.load(std::memory_order_acquire);
        if (sequence == lastSequence) {
            continue;
        }
        lastSequence = sequence;

        // Fault injection happens after a few good blocks so a test can
        // observe the healthy path and the failure in one run.
        if (handled == 3U) {
            if (mode == "crash") {
                // Abrupt termination with the region still mapped: the
                // host must survive this without the operating system
                // unwinding anything on its behalf.
                ::_Exit(99);
            }
            if (mode == "hang") {
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
        const float* const source = inputBase(control);
        float* const destination = outputBase(control, capacity);
        for (std::size_t index = 0U; index < samples; ++index) {
            // Stand-in for plugin DSP: a deterministic, verifiable
            // transform so a test can prove the audio round-tripped
            // through the other process rather than being zeroed.
            destination[index] = source[index] * 0.5F;
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
