#include "steam_hook_lite.h"
#include <windows.h>

extern "C" __declspec(dllexport) void InstallGC(bool dedicated)
{
    (void)dedicated;
    InstallSteamHooks();
}