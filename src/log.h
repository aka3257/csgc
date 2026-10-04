#pragma once

#include <cstdio>
#include <cstdarg>
#include <windows.h>
#include <mutex>

inline void GCLog(const char* format, ...)
{
    char buffer[4096];
    va_list ap;
    va_start(ap, format);
    vsnprintf(buffer, sizeof(buffer), format, ap);
    va_end(ap);

    SYSTEMTIME st;
    GetLocalTime(&st);

    char line[4300];
    _snprintf_s(line, sizeof(line), _TRUNCATE,
                "[%02u:%02u:%02u.%03u t%04X] %s",
                (unsigned)st.wHour, (unsigned)st.wMinute, (unsigned)st.wSecond,
                (unsigned)st.wMilliseconds, (unsigned)GetCurrentThreadId(), buffer);

    static std::mutex s_lock;
    std::lock_guard<std::mutex> guard(s_lock);

    static FILE* s_file = nullptr;
    static bool s_giveUp = false;
    if (!s_file && !s_giveUp) {
        s_file = fopen("csgc_full.log", "a");
        if (!s_file) s_giveUp = true;
    }
    if (s_file) {
        fputs(line, s_file);
        fflush(s_file);
    }

    OutputDebugStringA(line);
}
