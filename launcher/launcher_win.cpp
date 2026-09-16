#include <windows.h>
#include <wchar.h>
#include <cstdio>
#include "log.h"

// Обязательные экспорты для обхода проверок целостности CS:GO
// Без них игра может крашнуться или проигнорировать загрузку
extern "C" __declspec(dllexport) DWORD NvOptimusEnablement = 1;
extern "C" __declspec(dllexport) int AmdPowerXpressRequestHighPerformance = 1;
extern "C" __declspec(dllexport) bool BSecureAllowed(unsigned char*, int, int) { return true; }
extern "C" __declspec(dllexport) int RuntimeCheck(int, int) { return 0; }
extern "C" __declspec(dllexport) int CountFilesCompletedTrustCheck() { return 0; }
extern "C" __declspec(dllexport) int CountFilesNeedTrustCheck() { return 0; }
extern "C" __declspec(dllexport) int GetTotalFilesLoaded() { return 0; }

static void ErrorMessageBox(const wchar_t* format, ...)
{
    va_list ap;
    wchar_t buffer[4096];
    va_start(ap, format);
    _vsnwprintf_s(buffer, _countof(buffer), _TRUNCATE, format, ap);
    va_end(ap);
    MessageBoxW(nullptr, buffer, L"csgc", MB_OK | MB_ICONERROR);
}

static const wchar_t* LastErrorString()
{
    static wchar_t buffer[4096];
    buffer[0] = '\0';
    int error = GetLastError();
    FormatMessageW(
        FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS | FORMAT_MESSAGE_MAX_WIDTH_MASK,
        nullptr, error, 0, buffer, _countof(buffer), nullptr);
    if (!buffer[0]) _snwprintf_s(buffer, _countof(buffer), _TRUNCATE, L"Unknown error (%d)", error);
    return buffer;
}

static void* LoadModuleAndFindSymbol(const wchar_t* absoluteModulePath, const char* symbol)
{
    HMODULE module = LoadLibraryExW(
        absoluteModulePath,
        nullptr,
        LOAD_LIBRARY_SEARCH_DEFAULT_DIRS | LOAD_LIBRARY_SEARCH_USER_DIRS
    );
    if (!module) {
        ErrorMessageBox(L"Unable to load module:\n%s\n\n%s", absoluteModulePath, LastErrorString());
        return nullptr;
    }
    void* function = GetProcAddress(module, symbol);
    if (!function) {
        ErrorMessageBox(L"Not found symbol '%S' in module:\n%s\n\n%s", symbol, absoluteModulePath, LastErrorString());
        return nullptr;
    }
    return function;
}

int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance, LPSTR lpCmdLine, int nShowCmd)
{
    GCLog("[LAUNCHER] === WinMain started ===\n");

    wchar_t baseDir[MAX_PATH] = { 0 };
    wchar_t modulePath[MAX_PATH] = { 0 };

    DWORD len = GetModuleFileNameW(nullptr, baseDir, _countof(baseDir));
    GCLog("[LAUNCHER] GetModuleFileNameW returned %lu\n", len);
    if (!len || len == _countof(baseDir)) {
        GCLog("[LAUNCHER] FAILED: GetModuleFileNameW\n");
        return 1;
    }

    wchar_t* slash = wcsrchr(baseDir, L'\\');
    if (slash) *slash = L'\0';
    GCLog("[LAUNCHER] baseDir = %ls\n", baseDir);

    {
        wchar_t binDir[MAX_PATH] = { 0 };
        _snwprintf_s(binDir, _countof(binDir), _TRUNCATE, L"%s\\bin", baseDir);
        if (AddDllDirectory(binDir)) {
            GCLog("[LAUNCHER] AddDllDirectory OK: %ls\n", binDir);
        } else {
            GCLog("[LAUNCHER] AddDllDirectory FAILED (err=%lu)\n", GetLastError());
        }
    }

    _snwprintf_s(modulePath, _countof(modulePath), _TRUNCATE, L"%s\\csgc\\csgc.dll", baseDir);
    GCLog("[LAUNCHER] Loading DLL from: %ls\n", modulePath);

    auto InstallGC = (void(*)(bool))LoadModuleAndFindSymbol(modulePath, "InstallGC");
    if (!InstallGC) {
        GCLog("[LAUNCHER] FAILED: InstallGC not found\n");
        return 1;
    }
    GCLog("[LAUNCHER] InstallGC found at %p\n", InstallGC);

    GCLog("[LAUNCHER] Calling InstallGC(false)...\n");
    InstallGC(false);
    GCLog("[LAUNCHER] InstallGC returned\n");

    _snwprintf_s(modulePath, _countof(modulePath), _TRUNCATE, L"%s\\bin\\launcher.dll", baseDir);
    GCLog("[LAUNCHER] Loading launcher.dll from: %ls\n", modulePath);

    auto LauncherMain = (int(*)(bool, HINSTANCE, HINSTANCE, LPSTR, int))LoadModuleAndFindSymbol(modulePath, "LauncherMain");
    if (!LauncherMain) {
        GCLog("[LAUNCHER] FAILED: LauncherMain not found\n");
        return 1;
    }
    GCLog("[LAUNCHER] LauncherMain found at %p, calling...\n", LauncherMain);

    return LauncherMain(true, hInstance, hPrevInstance, lpCmdLine, nShowCmd);
}