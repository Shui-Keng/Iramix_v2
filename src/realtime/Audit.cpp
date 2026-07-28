#include "iramix/realtime/Audit.hpp"

#include <cstddef>

namespace iramix::realtime {
namespace {

thread_local bool insideCallback = false;
std::atomic<std::uint64_t> allocations {0U};
std::atomic<std::uint64_t> deallocations {0U};
std::atomic<std::uint64_t> blockingLocks {0U};

} // namespace

CallbackScope::CallbackScope() noexcept
    : previousState_ {insideCallback} {
    insideCallback = true;
}

CallbackScope::~CallbackScope() {
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

void resetAuditCounters() noexcept {
    allocations.store(0U, std::memory_order_relaxed);
    deallocations.store(0U, std::memory_order_relaxed);
    blockingLocks.store(0U, std::memory_order_relaxed);
}

AuditSnapshot auditSnapshot() noexcept {
    return {
        .allocations = allocations.load(std::memory_order_relaxed),
        .deallocations = deallocations.load(std::memory_order_relaxed),
        .blockingLocks = blockingLocks.load(std::memory_order_relaxed),
    };
}

bool verifyAuditHooks() {
    resetAuditCounters();
    TrackedMutex mutex;
    {
        CallbackScope callback;
        auto* memory = ::operator new(16U);
        ::operator delete(memory);
        mutex.lock();
        mutex.unlock();
    }
    const auto result = auditSnapshot();
    resetAuditCounters();
    return result.allocations == 1U
        && result.deallocations == 1U
        && result.blockingLocks == 1U;
}

} // namespace iramix::realtime
