#pragma once

#include <cstdio>
#include <cstdarg>
#include <cstring>

namespace psxrecomp {

enum TraceCategory {
    TraceCpu = 0,
    TraceGpu,
    TraceIrq,
    TraceBios,
    TraceCdrom,
    TraceDma,
    TraceHle,
    TraceRuntime,
    TraceOther,
    TraceCategoryCount
};

inline bool* traceCategoryFlags()
{
    static bool flags[TraceCategoryCount] = {
        false, false, true, true, true, true, true, true, true
    };
    return flags;
}

inline void traceSetCategoryEnabled(TraceCategory category, bool enabled)
{
    if (category >= TraceCpu && category < TraceCategoryCount)
        traceCategoryFlags()[category] = enabled;
}

inline bool traceCategoryEnabled(TraceCategory category)
{
    return category >= TraceCpu && category < TraceCategoryCount
        ? traceCategoryFlags()[category] : true;
}

inline TraceCategory traceClassify(const char* fmt)
{
    if (!fmt) return TraceOther;
    if (std::strncmp(fmt, "[GPU", 4) == 0 || std::strncmp(fmt, "[GL", 3) == 0)
        return TraceGpu;
    if (std::strncmp(fmt, "[IRQ", 4) == 0 || std::strncmp(fmt, "[VBLANK", 7) == 0)
        return TraceIrq;
    if (std::strncmp(fmt, "[BIOS", 5) == 0)
        return TraceBios;
    if (std::strncmp(fmt, "[CD", 3) == 0)
        return TraceCdrom;
    if (std::strncmp(fmt, "[DMA", 4) == 0)
        return TraceDma;
    if (std::strncmp(fmt, "[HLE", 4) == 0)
        return TraceHle;
    if (std::strncmp(fmt, "[RUNTIME", 8) == 0 || std::strncmp(fmt, "[TRACE", 6) == 0)
        return TraceRuntime;
    if (std::strncmp(fmt, "[CPU", 4) == 0 || std::strncmp(fmt, "[EXEC", 5) == 0)
        return TraceCpu;
    return TraceOther;
}

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
    if (!traceCategoryEnabled(traceClassify(fmt)))
        return;

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
