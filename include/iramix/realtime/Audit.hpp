#pragma once

#include <atomic>
#include <cstdint>
#include <mutex>

namespace iramix::realtime {

struct AuditSnapshot {
    std::uint64_t allocations {0U};
    std::uint64_t deallocations {0U};
    std::uint64_t blockingLocks {0U};
    std::uint64_t denormalModeEntries {0U};
    std::uint64_t subnormalSamplesFlushed {0U};
};

class CallbackScope final {
public:
    CallbackScope() noexcept;
    ~CallbackScope();

    CallbackScope(const CallbackScope&) = delete;
    CallbackScope& operator=(const CallbackScope&) = delete;

private:
    bool previousState_ {false};
    bool restoresFloatingPointControl_ {false};
    std::uint64_t previousFloatingPointControl_ {0U};
};

class TrackedMutex final {
public:
    void lock();
    [[nodiscard]] bool try_lock();
    void unlock();

private:
    std::mutex mutex_;
};

[[nodiscard]] bool isInCallback() noexcept;
void recordAllocation() noexcept;
void recordDeallocation() noexcept;
void recordBlockingLock() noexcept;
[[nodiscard]] bool denormalProtectionSupported() noexcept;
[[nodiscard]] bool denormalProtectionActive() noexcept;
[[nodiscard]] float flushSubnormalSample(float value) noexcept;
void resetAuditCounters() noexcept;
[[nodiscard]] AuditSnapshot auditSnapshot() noexcept;
[[nodiscard]] bool verifyAuditHooks();

} // namespace iramix::realtime
