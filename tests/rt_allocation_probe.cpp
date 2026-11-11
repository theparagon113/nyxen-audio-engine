// Test-only replacement of all standard C++ new/delete forms. Does NOT intercept
// arbitrary malloc/realloc, Windows heaps, mapped memory or foreign DLL allocators.
#include "rt_allocation_probe.h"
#include <cstdint>
#include <cstdlib>
#include <limits>
#include <new>

namespace {
thread_local bool active = false;
thread_local std::size_t observed = 0;
std::size_t live = 0;
struct Header { void* original; std::size_t size; };
void* allocate(std::size_t size, std::size_t alignment) {
    if (active) ++observed;
    if (size == 0) size = 1;
    if (size > std::numeric_limits<std::size_t>::max() - alignment - sizeof(Header)) throw std::bad_alloc();
    auto* raw = std::malloc(size + alignment - 1 + sizeof(Header));
    if (!raw) throw std::bad_alloc();
    const auto address = (reinterpret_cast<std::uintptr_t>(raw) + sizeof(Header) + alignment - 1) & ~(alignment - 1);
    auto* header = reinterpret_cast<Header*>(address) - 1;
    *header = {raw, size};
    live += size;
    return reinterpret_cast<void*>(address);
}
void dispose(void* pointer) noexcept {
    if (!pointer) return;
    const auto* header = reinterpret_cast<Header*>(pointer) - 1;
    live -= header->size;
    std::free(header->original);
}
}
namespace rtprobe {
std::size_t events() noexcept { return observed; }
std::size_t liveBytes() noexcept { return live; }
void begin() noexcept { active = true; }
void end() noexcept { active = false; }
}
void* operator new(std::size_t n) { return allocate(n, alignof(std::max_align_t)); }
void* operator new[](std::size_t n) { return allocate(n, alignof(std::max_align_t)); }
void* operator new(std::size_t n, std::align_val_t a) { return allocate(n, static_cast<std::size_t>(a)); }
void* operator new[](std::size_t n, std::align_val_t a) { return allocate(n, static_cast<std::size_t>(a)); }
void* operator new(std::size_t n, const std::nothrow_t&) noexcept { try { return ::operator new(n); } catch (...) { return nullptr; } }
void* operator new[](std::size_t n, const std::nothrow_t&) noexcept { try { return ::operator new[](n); } catch (...) { return nullptr; } }
void* operator new(std::size_t n, std::align_val_t a, const std::nothrow_t&) noexcept { try { return ::operator new(n, a); } catch (...) { return nullptr; } }
void* operator new[](std::size_t n, std::align_val_t a, const std::nothrow_t&) noexcept { try { return ::operator new[](n, a); } catch (...) { return nullptr; } }
void operator delete(void* p) noexcept { dispose(p); }
void operator delete[](void* p) noexcept { dispose(p); }
void operator delete(void* p, std::size_t) noexcept { dispose(p); }
void operator delete[](void* p, std::size_t) noexcept { dispose(p); }
void operator delete(void* p, std::align_val_t) noexcept { dispose(p); }
void operator delete[](void* p, std::align_val_t) noexcept { dispose(p); }
void operator delete(void* p, std::size_t, std::align_val_t) noexcept { dispose(p); }
void operator delete[](void* p, std::size_t, std::align_val_t) noexcept { dispose(p); }
void operator delete(void* p, const std::nothrow_t&) noexcept { dispose(p); }
void operator delete[](void* p, const std::nothrow_t&) noexcept { dispose(p); }
void operator delete(void* p, std::align_val_t, const std::nothrow_t&) noexcept { dispose(p); }
void operator delete[](void* p, std::align_val_t, const std::nothrow_t&) noexcept { dispose(p); }
