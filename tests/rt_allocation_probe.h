#pragma once
#include <cstddef>
namespace rtprobe {
#ifdef NYXEN_STREAM_ASAN
inline std::size_t events() noexcept { return 0; }
inline std::size_t liveBytes() noexcept { return 0; }
inline void begin() noexcept {}
inline void end() noexcept {}
#else
std::size_t events() noexcept;
std::size_t liveBytes() noexcept;
void begin() noexcept;
void end() noexcept;
#endif
struct Region {
    Region() noexcept { begin(); }
    ~Region() { end(); }
};
}
