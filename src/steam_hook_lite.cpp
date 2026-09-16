#define WIN32_LEAN_AND_MEAN
#define _WINSOCKAPI_    // ← блокирует старый winsock.h

#include "steam_hook_lite.h"
#include "forwarder.h"
#include "log.h"

#include <queue>
#include <steam/public/steam/isteamuser.h>
#include <steam/steamnetworkingtypes.h>
#include <windows.h>
#include <funchook.h>
#include <cstring>
#include <vector>
#include <unordered_map>
#include <memory>

#include <steam/steam_api_common.h>
#include <steam/isteamclient.h>
#include <steam/isteamgamecoordinator.h>

// ============================================================================
// 1. Утилита сравнения имён интерфейсов (как в оригинале)
// ============================================================================
template<size_t N>
static inline bool InterfaceMatches(const char* name, const char (&compare)[N])
{
    if (!name) return false;
    size_t length = strlen(name);
    if (length != (N - 1)) return false;
    return memcmp(name, compare, length) == 0;
}

// ============================================================================
// 2. Очередь GC-сообщений
// ============================================================================
class GCMessageQueue
{
public:
    bool IsMessageAvailable(uint32_t& size)
    {
        if (m_messages.empty()) return false;
        size = static_cast<uint32_t>(m_messages.front().buffer.size());
        return true;
    }

    bool RetrieveMessage(uint32_t& type, void* buffer, uint32_t bufferSize, uint32_t& size)
    {
        if (m_messages.empty()) { size = 0; return false; }
        Message& message = m_messages.front();
        type = message.type;
        size = static_cast<uint32_t>(message.buffer.size());
        if (bufferSize < message.buffer.size()) return false;
        memcpy(buffer, message.buffer.data(), message.buffer.size());
        m_messages.pop();
        return true;
    }

    void AddMessage(uint32_t type, std::vector<uint8_t>&& buffer)
    {
        Message& dest = m_messages.emplace();
        dest.type = type;
        dest.buffer = std::move(buffer);
    }

private:
    struct Message {
        uint32_t type{};
        std::vector<uint8_t> buffer;
    };
    std::queue<Message> m_messages;
};

static GCMessageQueue g_messageQueue;
static uint64_t g_currentSteamId = 0;  // заполняется при первом SendMessage

// ============================================================================
// 3. Прокси ISteamGameCoordinator — редиректит на внешний сервер
// ============================================================================
class GameCoordinatorProxyLite final : public ISteamGameCoordinator
{
public:
    EGCResults SendMessage(uint32 unMsgType, const void* pubData, uint32 cubData) override
    {
        uint64_t steamId = 0;
        if (SteamUser()) {
            steamId = SteamUser()->GetSteamID().ConvertToUint64();
        }

        GCLog("[GC] SendMessage: type=0x%08X, size=%u, steamId=%llu\n",
            unMsgType, cubData, steamId);

        // Дамп первых 8 байт pubData
        if (cubData >= 8) {
            const uint8_t* p = (const uint8_t*)pubData;
            GCLog("[GC] pubData[0..7]: %02X %02X %02X %02X %02X %02X %02X %02X\n",
                p[0], p[1], p[2], p[3], p[4], p[5], p[6], p[7]);
        }

        ExternalGCResponse response;
        if (!ForwardToExternalServer(steamId, unMsgType, pubData, cubData, response)) {
            GCLog("[GC] No response from external server\n");
            return k_EGCResultOK;
        }

        std::vector<std::vector<uint8_t>> messages;
        if (SplitGCMessages(response.data, messages)) {
            for (auto& msg : messages) {
                if (msg.size() >= 4) {
                    uint32_t type = 0;
                    memcpy(&type, msg.data(), 4);
                    g_messageQueue.AddMessage(type, std::move(msg));
                    GCLog("[GC] Queued message type=0x%08X\n", type);
                }
            }
        } else {
            g_messageQueue.AddMessage(response.msgType, std::move(response.data));
        }

        return k_EGCResultOK;
    }

    bool IsMessageAvailable(uint32* pcubMsgSize) override
    {
        return g_messageQueue.IsMessageAvailable(*pcubMsgSize);
    }

    EGCResults RetrieveMessage(uint32* punMsgType, void* pubDest, uint32 cubDest, uint32* pcubMsgSize) override
    {
        bool result = g_messageQueue.RetrieveMessage(*punMsgType, pubDest, cubDest, *pcubMsgSize);

        if (!result) {
            if (cubDest < *pcubMsgSize) return k_EGCResultBufferTooSmall;
            return k_EGCResultNoMessage;
        }

        GCLog("[GC] RetrieveMessage: type=%u, size=%u\n", *punMsgType, *pcubMsgSize);
        return k_EGCResultOK;
    }
};

static GameCoordinatorProxyLite* g_gcProxy = nullptr;

static ISteamGameCoordinator* GetOrCreateGCProxy()
{
    if (!g_gcProxy) {
        g_gcProxy = new GameCoordinatorProxyLite();
        GCLog("[GC] Created GameCoordinatorProxyLite\n");
    }
    return g_gcProxy;
}

// ============================================================================
// 4. SteamInterfaceProxy — проксирует интерфейсы, отдавая наш GC
// ============================================================================
class SteamInterfaceProxy
{
public:
    SteamInterfaceProxy(HSteamPipe pipe) : m_pipe(pipe) {}

    void* GetInterface(const char* version, void* original)
    {
        if (InterfaceMatches(version, STEAMGAMECOORDINATOR_INTERFACE_VERSION)) {
            GCLog("[CLIENT] *** Returning GameCoordinatorProxy for %s ***\n", version);
            return GetOrCreateGCProxy();
        }
        return nullptr;  // не наш интерфейс — отдадим оригинал
    }

private:
    const HSteamPipe m_pipe;
};

// ============================================================================
// 5. SteamClientProxy — перехватывает GetISteamGenericInterface
// ============================================================================
class SteamClientProxyLite : public ISteamClient
{
    ISteamClient* m_original{};
    std::unordered_map<uint64_t, SteamInterfaceProxy> m_proxies;

    uint64_t ProxyKey(HSteamPipe pipe, HSteamUser user)
    {
        return static_cast<uint64_t>(pipe) | (static_cast<uint64_t>(user) << 32);
    }

    SteamInterfaceProxy& GetProxy(HSteamPipe pipe, HSteamUser user, bool allowNoUser)
    {
        (void)allowNoUser;
        auto result = m_proxies.try_emplace(ProxyKey(pipe, user), pipe);
        return result.first->second;
    }

    // === ЗАГЛУШКИ для protected-методов (нельзя вызвать оригинал) ===

    void RunFrame() override
    {
        // RunFrame защищён в SDK, вызываем напрямую через vtable или игнорируем.
        // Steam сам вызывает RunFrame на своей стороне — наша заглушка безопасна.
        GCLog("[CLIENT] RunFrame (stub)\n");
    }

    void* DEPRECATED_GetISteamUnifiedMessages(HSteamUser, HSteamPipe, const char*) override
    {
        GCLog("[CLIENT] DEPRECATED_GetISteamUnifiedMessages (stub)\n");
        return nullptr;
    }

    void DEPRECATED_Set_SteamAPI_CPostAPIResultInProcess(void (*func)()) override
    {
        (void)func;
        GCLog("[CLIENT] DEPRECATED_Set_SteamAPI_CPostAPIResultInProcess (stub)\n");
    }

    void DEPRECATED_Remove_SteamAPI_CPostAPIResultInProcess(void (*func)()) override
    {
        (void)func;
        GCLog("[CLIENT] DEPRECATED_Remove_SteamAPI_CPostAPIResultInProcess (stub)\n");
    }

    void Set_SteamAPI_CCheckCallbackRegisteredInProcess(SteamAPI_CheckCallbackRegistered_t func) override
    {
        (void)func;
        GCLog("[CLIENT] Set_SteamAPI_CCheckCallbackRegisteredInProcess (stub)\n");
    }

    ISteamGameSearch* GetISteamGameSearch(HSteamUser u, HSteamPipe p, const char* v) override
        { return m_original->GetISteamGameSearch(u, p, v); }

    ISteamInput* GetISteamInput(HSteamUser u, HSteamPipe p, const char* v) override
        { return m_original->GetISteamInput(u, p, v); }

    ISteamParties* GetISteamParties(HSteamUser u, HSteamPipe p, const char* v) override
        { return m_original->GetISteamParties(u, p, v); }

    ISteamRemotePlay* GetISteamRemotePlay(HSteamUser u, HSteamPipe p, const char* v) override
        { return m_original->GetISteamRemotePlay(u, p, v); }

    void DestroyAllInterfaces() override
    {
        // protected в оригинале — просто заглушка
        GCLog("[CLIENT] DestroyAllInterfaces called (stub)\n");
        m_proxies.clear();
    }
public:
    void SetOriginal(ISteamClient* original)
    {
        m_original = original;
    }

    template<typename T>
    T* ProxyInterface(T* original, HSteamUser user, HSteamPipe pipe, const char* version, bool allowNoUser = false)
    {
        SteamInterfaceProxy& proxy = GetProxy(pipe, user, allowNoUser);
        T* result = static_cast<T*>(proxy.GetInterface(version, original));
        return result ? result : original;
    }

#define PROXY_IFACE(func, user, pipe, version, ...) \
    ProxyInterface(m_original->func(user, pipe, version), user, pipe, version, ##__VA_ARGS__)

    // === ГЛАВНЫЙ МЕТОД — через него игра получает GC ===
    void* GetISteamGenericInterface(HSteamUser hSteamUser, HSteamPipe hSteamPipe, const char* pchVersion) override
    {
        GCLog("[CLIENT] GetISteamGenericInterface: %s\n", pchVersion ? pchVersion : "(null)");
        return PROXY_IFACE(GetISteamGenericInterface, hSteamUser, hSteamPipe, pchVersion, true);
    }

    // === ОСТАЛЬНЫЕ МЕТОДЫ — ПРОСТО ПРОКСИРУЕМ ===
    HSteamPipe CreateSteamPipe() override { return m_original->CreateSteamPipe(); }
    bool BReleaseSteamPipe(HSteamPipe hSteamPipe) override { return m_original->BReleaseSteamPipe(hSteamPipe); }
    HSteamUser ConnectToGlobalUser(HSteamPipe hSteamPipe) override { return m_original->ConnectToGlobalUser(hSteamPipe); }
    HSteamUser CreateLocalUser(HSteamPipe* phSteamPipe, EAccountType eAccountType) override { return m_original->CreateLocalUser(phSteamPipe, eAccountType); }
    void ReleaseUser(HSteamPipe hSteamPipe, HSteamUser hUser) override { m_original->ReleaseUser(hSteamPipe, hUser); }

    ISteamUser* GetISteamUser(HSteamUser u, HSteamPipe p, const char* v) override
        { return PROXY_IFACE(GetISteamUser, u, p, v); }

    ISteamGameServer* GetISteamGameServer(HSteamUser u, HSteamPipe p, const char* v) override
        { return PROXY_IFACE(GetISteamGameServer, u, p, v); }

    void SetLocalIPBinding(const SteamIPAddress_t& unIP, uint16 usPort) override
        { m_original->SetLocalIPBinding(unIP, usPort); }

    ISteamFriends* GetISteamFriends(HSteamUser u, HSteamPipe p, const char* v) override
        { return PROXY_IFACE(GetISteamFriends, u, p, v); }

    ISteamUtils* GetISteamUtils(HSteamPipe p, const char* v) override
        { return m_original->GetISteamUtils(p, v); }

    ISteamMatchmaking* GetISteamMatchmaking(HSteamUser u, HSteamPipe p, const char* v) override
        { return PROXY_IFACE(GetISteamMatchmaking, u, p, v); }

    ISteamMatchmakingServers* GetISteamMatchmakingServers(HSteamUser u, HSteamPipe p, const char* v) override
        { return PROXY_IFACE(GetISteamMatchmakingServers, u, p, v); }

    ISteamUserStats* GetISteamUserStats(HSteamUser u, HSteamPipe p, const char* v) override
        { return PROXY_IFACE(GetISteamUserStats, u, p, v); }

    ISteamGameServerStats* GetISteamGameServerStats(HSteamUser u, HSteamPipe p, const char* v) override
        { return PROXY_IFACE(GetISteamGameServerStats, u, p, v); }

    ISteamApps* GetISteamApps(HSteamUser u, HSteamPipe p, const char* v) override
        { return PROXY_IFACE(GetISteamApps, u, p, v); }

    ISteamNetworking* GetISteamNetworking(HSteamUser u, HSteamPipe p, const char* v) override
        { return PROXY_IFACE(GetISteamNetworking, u, p, v); }

    ISteamRemoteStorage* GetISteamRemoteStorage(HSteamUser u, HSteamPipe p, const char* v) override
        { return PROXY_IFACE(GetISteamRemoteStorage, u, p, v); }

    ISteamScreenshots* GetISteamScreenshots(HSteamUser u, HSteamPipe p, const char* v) override
        { return PROXY_IFACE(GetISteamScreenshots, u, p, v); }

    uint32 GetIPCCallCount() override { return m_original->GetIPCCallCount(); }
    void SetWarningMessageHook(SteamAPIWarningMessageHook_t fn) override { m_original->SetWarningMessageHook(fn); }
    bool BShutdownIfAllPipesClosed() override { return m_original->BShutdownIfAllPipesClosed(); }

    ISteamHTTP* GetISteamHTTP(HSteamUser u, HSteamPipe p, const char* v) override
        { return PROXY_IFACE(GetISteamHTTP, u, p, v); }

    ISteamController* GetISteamController(HSteamUser u, HSteamPipe p, const char* v) override
        { return PROXY_IFACE(GetISteamController, u, p, v); }

    ISteamUGC* GetISteamUGC(HSteamUser u, HSteamPipe p, const char* v) override
        { return PROXY_IFACE(GetISteamUGC, u, p, v); }

    ISteamAppList* GetISteamAppList(HSteamUser u, HSteamPipe p, const char* v) override
        { return PROXY_IFACE(GetISteamAppList, u, p, v); }

    ISteamMusic* GetISteamMusic(HSteamUser u, HSteamPipe p, const char* v) override
        { return PROXY_IFACE(GetISteamMusic, u, p, v); }

    ISteamMusicRemote* GetISteamMusicRemote(HSteamUser u, HSteamPipe p, const char* v) override
        { return PROXY_IFACE(GetISteamMusicRemote, u, p, v); }

    ISteamHTMLSurface* GetISteamHTMLSurface(HSteamUser u, HSteamPipe p, const char* v) override
        { return PROXY_IFACE(GetISteamHTMLSurface, u, p, v); }

    ISteamInventory* GetISteamInventory(HSteamUser u, HSteamPipe p, const char* v) override
        { return PROXY_IFACE(GetISteamInventory, u, p, v); }

    ISteamVideo* GetISteamVideo(HSteamUser u, HSteamPipe p, const char* v) override
        { return PROXY_IFACE(GetISteamVideo, u, p, v); }

    ISteamParentalSettings* GetISteamParentalSettings(HSteamUser u, HSteamPipe p, const char* v) override
        { return PROXY_IFACE(GetISteamParentalSettings, u, p, v); }
};

static SteamClientProxyLite g_steamClientProxy;

// ============================================================================
// 6. Хук на CreateInterface
// ============================================================================
static void* (*Og_CreateInterface)(const char*, int*) = nullptr;

static void* Hk_CreateInterface(const char* name, int* errorCode)
{
    void* result = Og_CreateInterface(name, errorCode);

    if (InterfaceMatches(name, STEAMCLIENT_INTERFACE_VERSION)) {
        GCLog("[HOOK] *** Intercepted %s, wrapping in proxy ***\n", name);
        g_steamClientProxy.SetOriginal(static_cast<ISteamClient*>(result));
        return &g_steamClientProxy;
    }

    return result;
}

// ============================================================================
// 7. Хуки на callback'и
// ============================================================================
struct CallbackHook {
    int id;
    CCallbackBase* callback;
};

static bool ShouldHookCallback(int id)
{
    switch (id) {
    case GCMessageAvailable_t::k_iCallback:
    case GCMessageFailed_t::k_iCallback:
        return true;
    default:
        return false;
    }
}

class CallbackAccessor : public CCallbackBase
{
public:
    bool IsGameServer() { return m_nCallbackFlags & k_ECallbackFlagsGameServer; }
    void SetRegistered() { m_nCallbackFlags |= k_ECallbackFlagsRegistered; }
    void UnsetRegistered() { m_nCallbackFlags &= ~k_ECallbackFlagsRegistered; }
};

static std::vector<CallbackHook> g_hookedCallbacks;

static void (*Og_SteamAPI_RegisterCallback)(CCallbackBase*, int) = nullptr;
static void (*Og_SteamAPI_UnregisterCallback)(CCallbackBase*) = nullptr;
static void (*Og_SteamAPI_RunCallbacks)() = nullptr;

static void Hk_SteamAPI_RegisterCallback(CCallbackBase* cb, int id)
{
    if (ShouldHookCallback(id)) {
        GCLog("[CB] Hooked callback id=%d\n", id);
        g_hookedCallbacks.push_back({ id, cb });
        static_cast<CallbackAccessor*>(cb)->SetRegistered();
        return;
    }
    Og_SteamAPI_RegisterCallback(cb, id);
}

static void Hk_SteamAPI_UnregisterCallback(CCallbackBase* cb)
{
    for (auto it = g_hookedCallbacks.begin(); it != g_hookedCallbacks.end(); ++it) {
        if (it->callback == cb) {
            static_cast<CallbackAccessor*>(cb)->UnsetRegistered();
            g_hookedCallbacks.erase(it);
            return;
        }
    }
    Og_SteamAPI_UnregisterCallback(cb);
}

static void Hk_SteamAPI_RunCallbacks()
{
    Og_SteamAPI_RunCallbacks();

    // Проверяем, есть ли сообщение в очереди
    uint32 msgSize = 0;
    if (!g_messageQueue.IsMessageAvailable(msgSize)) return;

    // Дёргаем все перехваченные callback'и
    GCMessageAvailable_t param{};
    param.m_nMessageSize = msgSize;

    for (auto& cb : g_hookedCallbacks) {
        if (cb.id == GCMessageAvailable_t::k_iCallback) {
            cb.callback->Run(&param);
        }
    }
}

// ============================================================================
// 8. Установка хуков
// ============================================================================
static bool HookFunction(const char* name, void* target, void* hook, void** bridge)
{
    if (!target) {
        GCLog("[HOOK] HookFunction: target %s is null\n", name);
        return false;
    }
    funchook_t* fh = funchook_create();
    if (!fh) { GCLog("[HOOK] funchook_create failed for %s\n", name); return false; }
    void* temp = target;
    if (funchook_prepare(fh, &temp, hook) != 0) {
        GCLog("[HOOK] funchook_prepare failed for %s\n", name);
        return false;
    }
    if (funchook_install(fh, 0) != 0) {
        GCLog("[HOOK] funchook_install failed for %s\n", name);
        return false;
    }
    *bridge = temp;
    GCLog("[HOOK] Hooked %s\n", name);
    return true;
}

static void InstallAllHooks(HMODULE steamclient)
{
    // === 1. CreateInterface в steamclient.dll ===
    void* createInterface = GetProcAddress(steamclient, "CreateInterface");
    if (createInterface) {
        HookFunction("CreateInterface", createInterface,
            (void*)Hk_CreateInterface, (void**)&Og_CreateInterface);
    } else {
        GCLog("[HOOK] CreateInterface not found in steamclient.dll\n");
    }

    // === 2. Callback-хуки в steam_api.dll ===
    HMODULE steamApi = GetModuleHandleA("steam_api.dll");
    if (!steamApi) steamApi = GetModuleHandleA("steam_api64.dll");
    if (!steamApi) {
        GCLog("[HOOK] steam_api.dll not found, callback hooks skipped\n");
        return;
    }

    void* reg = GetProcAddress(steamApi, "SteamAPI_RegisterCallback");
    void* unreg = GetProcAddress(steamApi, "SteamAPI_UnregisterCallback");
    void* run = GetProcAddress(steamApi, "SteamAPI_RunCallbacks");

    if (reg) HookFunction("SteamAPI_RegisterCallback", reg,
        (void*)Hk_SteamAPI_RegisterCallback, (void**)&Og_SteamAPI_RegisterCallback);
    if (unreg) HookFunction("SteamAPI_UnregisterCallback", unreg,
        (void*)Hk_SteamAPI_UnregisterCallback, (void**)&Og_SteamAPI_UnregisterCallback);
    if (run) HookFunction("SteamAPI_RunCallbacks", run,
        (void*)Hk_SteamAPI_RunCallbacks, (void**)&Og_SteamAPI_RunCallbacks);
}

// ============================================================================
// 9. Waiter thread — ждёт появления steamclient.dll
// ============================================================================
static DWORD WINAPI WaitForSteamClientThread(LPVOID)
{
    GCLog("[HOOK] Waiter thread started\n");
    for (int i = 0; i < 600; i++) {
        HMODULE sc = GetModuleHandleA("steamclient.dll");
        if (sc) {
            GCLog("[HOOK] steamclient.dll appeared at %p (after %d ms)\n", sc, i * 100);
            InstallAllHooks(sc);
            return 0;
        }
        if (i > 0 && i % 50 == 0) {
            GCLog("[HOOK] still waiting for steamclient.dll... (%d ms)\n", i * 100);
        }
        Sleep(100);
    }
    GCLog("[HOOK] FAILED: steamclient.dll never appeared after 60s\n");
    return 1;
}

void InstallSteamHooks()
{
    GCLog("[HOOK] === InstallSteamHooks started ===\n");

    HMODULE steamclient = GetModuleHandleA("steamclient.dll");
    if (steamclient) {
        GCLog("[HOOK] steamclient.dll already loaded\n");
        InstallAllHooks(steamclient);
    } else {
        GCLog("[HOOK] steamclient.dll not loaded yet, spawning waiter thread...\n");
        CreateThread(nullptr, 0, WaitForSteamClientThread, nullptr, 0, nullptr);
    }
}