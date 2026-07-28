#include "iramix/realtime/Audit.hpp"

#include <bit>
#include <cstddef>

#if defined(_M_IX86) || defined(_M_X64) \
    || defined(__i386__) || defined(__x86_64__)
#include <xmmintrin.h>
#define IRAMIX_X86_FLOATING_POINT_CONTROL 1
#elif defined(__aarch64__) && (defined(__GNUC__) || defined(__clang__))
#define IRAMIX_ARM64_FLOATING_POINT_CONTROL 1
#endif

namespace iramix::realtime {
namespace {

thread_local bool insideCallback = false;
std::atomic<std::uint64_t> allocations {0U};
std::atomic<std::uint64_t> deallocations {0U};
std::atomic<std::uint64_t> blockingLocks {0U};
std::atomic<std::uint64_t> denormalModeEntries {0U};
std::atomic<std::uint64_t> subnormalSamplesFlushed {0U};

[[nodiscard]] std::uint64_t floatingPointControl() noexcept {
#if defined(IRAMIX_X86_FLOATING_POINT_CONTROL)
    return static_cast<std::uint64_t>(_mm_getcsr());
#elif defined(IRAMIX_ARM64_FLOATING_POINT_CONTROL)
    std::uint64_t result = 0U;
    __asm__ volatile("mrs %0, fpcr" : "=r"(result));
    return result;
#else
    return 0U;
#endif
}

void setFloatingPointControl(const std::uint64_t value) noexcept {
#if defined(IRAMIX_X86_FLOATING_POINT_CONTROL)
    _mm_setcsr(static_cast<unsigned int>(value));
#elif defined(IRAMIX_ARM64_FLOATING_POINT_CONTROL)
    __asm__ volatile("msr fpcr, %0" : : "r"(value));
#else
    static_cast<void>(value);
#endif
}

[[nodiscard]] std::uint64_t denormalProtectionMask() noexcept {
#if defined(IRAMIX_X86_FLOATING_POINT_CONTROL)
    constexpr std::uint64_t flushToZero = 1U << 15U;
    constexpr std::uint64_t denormalsAreZero = 1U << 6U;
    return flushToZero | denormalsAreZero;
#elif defined(IRAMIX_ARM64_FLOATING_POINT_CONTROL)
    constexpr std::uint64_t flushToZero = 1U << 24U;
    return flushToZero;
#else
    return 0U;
#endif
}

} // namespace

CallbackScope::CallbackScope() noexcept
    : previousState_ {insideCallback} {
    insideCallback = true;
    if (!previousState_ && denormalProtectionSupported()) {
        previousFloatingPointControl_ = floatingPointControl();
        setFloatingPointControl(
            previousFloatingPointControl_
            | denormalProtectionMask()
        );
        restoresFloatingPointControl_ = true;
        denormalModeEntries.fetch_add(
            1U,
            std::memory_order_relaxed
        );
    }
}

CallbackScope::~CallbackScope() {
    if (restoresFloatingPointControl_) {
        setFloatingPointControl(previousFloatingPointControl_);
    }
    insideCallback = previousState_;
}

void TrackedMutex::lock() {
    recordBlockingLock();
    mutex_.lock();
}

bool TrackedMutex::try_lock() {
    return mutex_.try_lock();
}

void TrackedMutex::unlock() {
    mutex_.unlock();
}

bool isInCallback() noexcept {
    return insideCallback;
}

void recordAllocation() noexcept {
    if (insideCallback) {
        allocations.fetch_add(1U, std::memory_order_relaxed);
    }
}

void recordDeallocation() noexcept {
    if (insideCallback) {
        deallocations.fetch_add(1U, std::memory_order_relaxed);
    }
}

void recordBlockingLock() noexcept {
    if (insideCallback) {
        blockingLocks.fetch_add(1U, std::memory_order_relaxed);
    }
}

bool denormalProtectionSupported() noexcept {
    return denormalProtectionMask() != 0U;
}

bool denormalProtectionActive() noexcept {
    const auto mask = denormalProtectionMask();
    return mask != 0U
        && (floatingPointControl() & mask) == mask;
}

float flushSubnormalSample(const float value) noexcept {
    constexpr std::uint32_t exponentMask = 0x7F80'0000U;
    constexpr std::uint32_t fractionMask = 0x007F'FFFFU;
    constexpr std::uint32_t signMask = 0x8000'0000U;
    const auto bits = std::bit_cast<std::uint32_t>(value);
    if ((bits & exponentMask) != 0U
        || (bits & fractionMask) == 0U) {
        return value;
    }
    if (insideCallback) {
        subnormalSamplesFlushed.fetch_add(
            1U,
            std::memory_order_relaxed
        );
    }
    return std::bit_cast<float>(bits & signMask);
}

void resetAuditCounters() noexcept {
    allocations.store(0U, std::memory_order_relaxed);
    deallocations.store(0U, std::memory_order_relaxed);
    blockingLocks.store(0U, std::memory_order_relaxed);
    denormalModeEntries.store(0U, std::memory_order_relaxed);
    subnormalSamplesFlushed.store(0U, std::memory_order_relaxed);
}

AuditSnapshot auditSnapshot() noexcept {
    return {
        .allocations = allocations.load(std::memory_order_relaxed),
        .deallocations = deallocations.load(std::memory_order_relaxed),
        .blockingLocks = blockingLocks.load(std::memory_order_relaxed),
        .denormalModeEntries =
            denormalModeEntries.load(std::memory_order_relaxed),
        .subnormalSamplesFlushed =
            subnormalSamplesFlushed.load(std::memory_order_relaxed),
    };
}

bool verifyAuditHooks() {
    resetAuditCounters();
    TrackedMutex mutex;
    float flushed = 1.0F;
    bool protectionActive = false;
    {
        CallbackScope callback;
        protectionActive = !denormalProtectionSupported()
            || denormalProtectionActive();
        flushed = flushSubnormalSample(
            std::bit_cast<float>(0x0000'0001U)
        );
        auto* memory = ::operator new(16U);
        ::operator delete(memory);
        mutex.lock();
        mutex.unlock();
    }
    const auto result = auditSnapshot();
    resetAuditCounters();
    return result.allocations == 1U
        && result.deallocations == 1U
        && result.blockingLocks == 1U
        && protectionActive
        && std::bit_cast<std::uint32_t>(flushed) == 0U
        && result.denormalModeEntries
            == (denormalProtectionSupported() ? 1U : 0U)
        && result.subnormalSamplesFlushed == 1U;
}

} // namespace iramix::realtime
