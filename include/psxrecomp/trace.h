#pragma once

#include <cstdio>
#include <cstdarg>

namespace psxrecomp {

inline FILE*& traceFileHandle()
{
    static FILE* f = 0;
    return f;
}

inline void traceOpen(const char* path = "psx_full_trace.log")
{
    FILE*& f = traceFileHandle();
    if (!f) f = std::fopen(path, "wb");
}

inline void traceClose()
{
    FILE*& f = traceFileHandle();
    if (f) {
        std::fflush(f);
        std::fclose(f);
        f = 0;
    }
}

inline void tracePrintf(const char* fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    std::vfprintf(stdout, fmt, ap);
    std::fflush(stdout);
    va_end(ap);

    FILE*& f = traceFileHandle();
    if (!f) {
        f = std::fopen("psx_full_trace.log", "ab");
    }
    if (f) {
        va_start(ap, fmt);
        std::vfprintf(f, fmt, ap);
        va_end(ap);
        std::fflush(f);
    }
}

} // namespace psxrecomp
