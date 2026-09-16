#pragma once
#include <cstdio>
#include <cstdarg>
#include <windows.h>

// Универсальный лог — пишет И в файл, И в OutputDebugString
// Файл: csgc_full.log рядом с csgo.exe
inline void GCLog(const char* format, ...)
{
    va_list ap;
    char buffer[4096];
    va_start(ap, format);
    vsnprintf(buffer, sizeof(buffer), format, ap);
    va_end(ap);

    // 1. В файл
    FILE* f = fopen("csgc_full.log", "a");
    if (f) {
        fprintf(f, "%s", buffer);
        fclose(f);
    }

    // 2. В DebugView (если запущен)
    OutputDebugStringA(buffer);
}