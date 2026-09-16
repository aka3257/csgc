#include <windows.h>
#include <cstdio>
#include <cstdarg>
#include <cstdint>

// Platform::Print
void Platform_Print(const char *format, ...)
{
    va_list ap;
    char buffer[4096];
    va_start(ap, format);
    vsnprintf(buffer, sizeof(buffer), format, ap);
    va_end(ap);
    
    // ВСЕГДА пишем в файл
    FILE *f = fopen("C:\\csgc_log.txt", "a");
    if (f) {
        fprintf(f, "[GC] %s\n", buffer);
        fclose(f);
    }
    
    // И пробуем в консоль игры
    HMODULE tier0 = GetModuleHandleW(L"tier0.dll");
    if (tier0) {
        auto ConColorMsg = (void(*)(const uint8_t*, const char*, ...))GetProcAddress(tier0, "?ConColorMsg@@YAXABVColor@@PBDZZ");
        if (ConColorMsg) {
            uint8_t color[4] = { 0, 255, 128, 255 };
            ConColorMsg(color, "[GC] %s", buffer);
        }
    }
}