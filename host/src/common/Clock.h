#pragma once

#include <windows.h>

#include <cstdint>

namespace dd
{

// Host clock for the wire protocol: QueryPerformanceCounter in microseconds. Desktop
// Duplication present times are QPC ticks too, so they convert with QpcToMicros.
inline int64_t QpcFrequency()
{
    static const int64_t frequency = [] {
        LARGE_INTEGER f;
        QueryPerformanceFrequency(&f);
        return f.QuadPart;
    }();
    return frequency;
}

inline uint64_t QpcToMicros(int64_t ticks)
{
    const int64_t f = QpcFrequency();
    return static_cast<uint64_t>((ticks / f) * 1'000'000 + (ticks % f) * 1'000'000 / f);
}

inline uint64_t NowMicros()
{
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    return QpcToMicros(now.QuadPart);
}

} // namespace dd
