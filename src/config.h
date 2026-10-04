// config.h — переключатели через переменные окружения ИЛИ через файл.
//
// Файл нужен потому, что игру запускает Steam, и переменные окружения
// консоли до неё не доходят. Кладём рядом с csgo.exe файл csgc_flags.txt:
//
//   CSGC_TRACE=1
//   CSGC_HOOK_USER=1
//
// Ключи те же, что у переменных окружения. Окружение имеет приоритет.

#pragma once

#define WIN32_LEAN_AND_MEAN
#define _WINSOCKAPI_
#include <windows.h>
#include <cstdlib>
#include <cstring>
#include <cstdio>
#include <string>
#include <map>

#include "log.h"

namespace CSGCConfig
{
    constexpr uint32_t kAppIdOverride = 4465480;

    inline std::string TrimStr(const char* s)
    {
        std::string r(s ? s : "");
        size_t b = r.find_first_not_of(" \t\r\n\"");
        size_t e = r.find_last_not_of(" \t\r\n\"");
        if (b == std::string::npos) return std::string();
        return r.substr(b, e - b + 1);
    }


    inline const char* FlagsFileGet(const char* name)
    {
        static std::map<std::string, std::string>* kv = nullptr;
        if (!kv) {
            kv = new std::map<std::string, std::string>();
            FILE* f = fopen("csgc_flags.txt", "r");
            if (f) {
                char line[512];
                while (fgets(line, sizeof(line), f)) {
                    char* eq = strchr(line, '=');
                    if (!eq) continue;
                    *eq = '\0';
                    std::string k = TrimStr(line);
                    std::string v = TrimStr(eq + 1);
                    if (!k.empty()) (*kv)[k] = v;
                }
                fclose(f);
            }
            GCLog("[CFG] csgc_flags.txt: %s (%d keys)\n",
                  kv->empty() ? "invalid or empty" : "read", (int)kv->size());
        }
        auto it = kv->find(name);
        return (it == kv->end()) ? nullptr : it->second.c_str();
    }


    inline const char* ConfigString(const char* name)
    {
        static char buf[512];
        DWORD n = GetEnvironmentVariableA(name, buf, sizeof(buf));
        if (n > 0 && n < (DWORD)sizeof(buf) && buf[0] != '\0') return buf;
        return FlagsFileGet(name);
    }

    inline bool EnvFlag(const char* name, bool def)
    {
        const char* v = ConfigString(name);
        if (!v || v[0] == '\0') return def;
        switch (v[0]) {
        case '0': case 'n': case 'N': case 'f': case 'F': return false;
        default: return true;
        }
    }

    inline int ConfigInt(const char* name, int def)
    {
        const char* v = ConfigString(name);
        if (!v || v[0] == '\0') return def;
        return atoi(v);
    }

#define CSGC_FLAG(fn, env, def)                     \
    inline bool fn() {                              \
        static bool v = EnvFlag(env, def);          \
        return v;                                   \
    }

    // --- диагностика ---
    CSGC_FLAG(Trace,        "CSGC_TRACE",        false)  // трейс КАЖДОГО вызова Steam-интерфейсов
    CSGC_FLAG(LogAllGC,     "CSGC_LOG_GC",       false)  // подробный дамп GC-трафика

    // --- бисект: что именно не проксировать ---
    CSGC_FLAG(NoClient,     "CSGC_NO_CLIENT",    false)  // не подменять ISteamClient
    CSGC_FLAG(NoUtils,      "CSGC_NO_UTILS",     false)  // не подменять ISteamUtils
    CSGC_FLAG(NoStats,      "CSGC_NO_STATS",     false)  // не подменять ISteamUserStats
    CSGC_FLAG(NoGC,         "CSGC_NO_GC",        false)  // не подменять ISteamGameCoordinator
    CSGC_FLAG(NoForward,    "CSGC_NO_FORWARD",   false)  // GC работает, но в сеть не ходим
    CSGC_FLAG(NoCallbacks,  "CSGC_NO_CALLBACKS", false)  // не дёргать GCMessageAvailable вручную
    CSGC_FLAG(NoCallResult, "CSGC_NO_CALLRESULT",false)  // без хука SteamAPI_RegisterCallResult
    CSGC_FLAG(NoStatsCb,    "CSGC_NO_STATS_CB",  false)  // не доставлять UserStatsReceived
    CSGC_FLAG(NoCrashH,     "CSGC_NO_CRASHH",    false)  // не ставить свой crash handler
    CSGC_FLAG(Veh,          "CSGC_VEH",          false)  // печатать все брошенные C++ исключения
    CSGC_FLAG(HookUser,     "CSGC_HOOK_USER",    false)  // проксировать ISteamUser (трейс тикетов)
    CSGC_FLAG(SpoofStats,   "CSGC_SPOOF_STATS",  false)  // GetStat всегда true (для тестов)

    // --- поведение ---
    CSGC_FLAG(Async,        "CSGC_ASYNC",        false)  // асинхронный форвардер

    // Что отдавать на CheckFileSignature. Значения ECheckFileSignature:
    //   0 InvalidSignature, 1 ValidSignature, 2 FileNotFound,
    //   3 NoSignaturesFoundForThisApp, 4 NoSignaturesFoundForThisFile
    // По умолчанию 3 (как было). Меняется через CSGC_SIG_RESULT=N без пересборки.
    inline int SigResult()
    {
        static int v = [] {
            int r = ConfigInt("CSGC_SIG_RESULT", 3);
            if (r < 0 || r > 4) r = 3;
            return r;
        }();
        return v;
    }

    inline int NetTimeoutMs()
    {
        static int v = [] {
            int t = ConfigInt("CSGC_TIMEOUT_MS", 3000);
            if (t < 200) t = 200;
            if (t > 30000) t = 30000;
            return t;
        }();
        return v;
    }

    // CSGC_SERVER=127.0.0.1:3257
    inline bool ServerOverride(char* host, size_t cap, int& port)
    {
        const char* s = ConfigString("CSGC_SERVER");
        if (!s || !*s) return false;
        char buf[256];
        strncpy_s(buf, sizeof(buf), s, _TRUNCATE);
        char* colon = strrchr(buf, ':');
        if (colon) {
            *colon = '\0';
            int p = atoi(colon + 1);
            if (p > 0 && p < 65536) port = p;
        }
        strncpy_s(host, cap, buf, _TRUNCATE);
        return host[0] != '\0';
    }

    // CSGC_STEAMID=765... — подставить steamId, если SteamUser() недоступен.
    inline uint64_t TestSteamId()
    {
        static uint64_t v = [] {
            const char* s = ConfigString("CSGC_STEAMID");
            if (!s || !*s) return (uint64_t)0;
            return (uint64_t)_strtoui64(s, nullptr, 10);
        }();
        return v;
    }

    // Печатает эффективную конфигурацию в лог.
    inline void DumpToLog()
    {
        GCLog("[CFG] trace=%d noClient=%d noUtils=%d noStats=%d noGC=%d noForward=%d "
              "noCallbacks=%d noCallResult=%d noStatsCb=%d async=%d hookUser=%d "
              "timeout=%dms sigResult=%d\n",
              Trace(), NoClient(), NoUtils(), NoStats(), NoGC(), NoForward(),
              NoCallbacks(), NoCallResult(), NoStatsCb(), Async(), HookUser(),
              NetTimeoutMs(), SigResult());

        char host[256]; int port = 0;
        if (ServerOverride(host, sizeof(host), port))
            GCLog("[CFG] the server is overridden via CSGC_SERVER\n");
    }
}
