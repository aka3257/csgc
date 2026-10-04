#include "steam_hook.h"
#include <windows.h>

extern "C" __declspec(dllexport) void InstallGC(bool dedicated)
{
    (void)dedicated;
    InstallSteamHooks();
}
