#include "iramix/realtime/Audit.hpp"

#include <cstddef>
#include <cstdlib>
#include <new>

#if defined(_WIN32)
#include <malloc.h>
#endif

namespace {

[[nodiscard]] void* allocate(const std::size_t size) {
    iramix::realtime::recordAllocation();
    if (auto* memory = std::malloc(size); memory != nullptr) {
        return memory;
    }
    throw std::bad_alloc {};
}

[[nodiscard]] void* allocateAligned(
    const std::size_t size,
    const std::size_t alignment
) {
    iramix::realtime::recordAllocation();
#if defined(_WIN32)
    if (auto* memory = _aligned_malloc(size, alignment); memory != nullptr) {
        return memory;
    }
#else
    void* memory = nullptr;
    if (posix_memalign(&memory, alignment, size) == 0) {
        return memory;
    }
#endif
    throw std::bad_alloc {};
}

void deallocate(void* memory) noexcept {
    iramix::realtime::recordDeallocation();
    std::free(memory);
}

void deallocateAligned(void* memory) noexcept {
    iramix::realtime::recordDeallocation();
#if defined(_WIN32)
    _aligned_free(memory);
#else
    std::free(memory);
#endif
}

} // namespace

void* operator new(const std::size_t size) {
    return allocate(size);
}

void* operator new[](const std::size_t size) {
    return allocate(size);
}

void operator delete(void* memory) noexcept {
    deallocate(memory);
}

void operator delete[](void* memory) noexcept {
    deallocate(memory);
}

void operator delete(void* memory, std::size_t) noexcept {
    deallocate(memory);
}

void operator delete[](void* memory, std::size_t) noexcept {
    deallocate(memory);
}

void* operator new(
    const std::size_t size,
    const std::nothrow_t&
) noexcept {
    try {
        return allocate(size);
    } catch (...) {
        return nullptr;
    }
}

void* operator new[](
    const std::size_t size,
    const std::nothrow_t&
) noexcept {
    try {
        return allocate(size);
    } catch (...) {
        return nullptr;
    }
}

void operator delete(
    void* memory,
    const std::nothrow_t&
) noexcept {
    deallocate(memory);
}

void operator delete[](
    void* memory,
    const std::nothrow_t&
) noexcept {
    deallocate(memory);
}

void* operator new(
    const std::size_t size,
    const std::align_val_t alignment
) {
    return allocateAligned(size, static_cast<std::size_t>(alignment));
}

void* operator new[](
    const std::size_t size,
    const std::align_val_t alignment
) {
    return allocateAligned(size, static_cast<std::size_t>(alignment));
}

void operator delete(
    void* memory,
    const std::align_val_t
) noexcept {
    deallocateAligned(memory);
}

void operator delete[](
    void* memory,
    const std::align_val_t
) noexcept {
    deallocateAligned(memory);
}

void operator delete(
    void* memory,
    std::size_t,
    const std::align_val_t
) noexcept {
    deallocateAligned(memory);
}

void operator delete[](
    void* memory,
    std::size_t,
    const std::align_val_t
) noexcept {
    deallocateAligned(memory);
}

void* operator new(
    const std::size_t size,
    const std::align_val_t alignment,
    const std::nothrow_t&
) noexcept {
    try {
        return allocateAligned(
            size,
            static_cast<std::size_t>(alignment)
        );
    } catch (...) {
        return nullptr;
    }
}

void* operator new[](
    const std::size_t size,
    const std::align_val_t alignment,
    const std::nothrow_t&
) noexcept {
    try {
        return allocateAligned(
            size,
            static_cast<std::size_t>(alignment)
        );
    } catch (...) {
        return nullptr;
    }
}

void operator delete(
    void* memory,
    const std::align_val_t,
    const std::nothrow_t&
) noexcept {
    deallocateAligned(memory);
}

void operator delete[](
    void* memory,
    const std::align_val_t,
    const std::nothrow_t&
) noexcept {
    deallocateAligned(memory);
}
