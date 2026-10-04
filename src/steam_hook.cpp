//  * CSGC_TRACE=1 — трейс КАЖДОГО вызова Steam-интерфейсов (хвост лога показывает,
//    на каком именно вызове игра умерла).
//  * CSGC_NO_* — можно отключать подмену интерфейсов по одному, для бисекта краша.

#define WIN32_LEAN_AND_MEAN
#define _WINSOCKAPI_

#include "steam_hook.h"
#include "forwarder.h"
#include "log.h"
#include "config.h"
#include "crash_report.h"

#include <windows.h>
#include <funchook.h>
#include <cstring>
#include <cstdarg>
#include <intrin.h>
#include <vector>
#include <queue>
#include <memory>
#include <unordered_map>
#include <mutex>
#include <atomic>

#include <steam/steam_api.h>
#include <steam/steam_gameserver.h>
#include <steam/isteamclient.h>
#include <steam/isteamgamecoordinator.h>
#include <steam/isteamutils.h>
#include <steam/isteamuserstats.h>
#include <steam/isteamuser.h>
#include <steam/isteamgameserver.h>
#include <steam/isteammatchmaking.h>

// ============================================================================
// Трейс
// ============================================================================
#define TR(...) do { if (CSGCConfig::Trace()) GCLog(__VA_ARGS__); } while (0)

// ============================================================================
// Утилиты
// ============================================================================
template<size_t N>
static inline bool InterfaceNameEquals(const char* name, const char (&compare)[N])
{
    if (!name) return false;
    if (strlen(name) != (N - 1)) return false;
    return memcmp(name, compare, N - 1) == 0;
}

// ============================================================================
// Очередь GC-сообщений (наша, из TCP-ответов)
// ============================================================================
class GCMessageQueue
{
public:
    bool IsAvailable(uint32_t& size)
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (m_queue.empty()) return false;
        size = static_cast<uint32_t>(m_queue.front().buffer.size());
        return true;
    }

    bool Retrieve(uint32_t& type, void* buffer, uint32_t bufferSize, uint32_t& size)
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (m_queue.empty()) { size = 0; return false; }
        Message& msg = m_queue.front();
        type = msg.type;
        size = static_cast<uint32_t>(msg.buffer.size());
        if (bufferSize < msg.buffer.size()) return false;
        memcpy(buffer, msg.buffer.data(), msg.buffer.size());
        m_queue.pop();
        return true;
    }

    void Push(uint32_t type, std::vector<uint8_t>&& buffer)
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        Message& msg = m_queue.emplace();
        msg.type = type;
        msg.buffer = std::move(buffer);
    }

private:
    struct Message {
        uint32_t type{};
        std::vector<uint8_t> buffer;
    };
    std::queue<Message> m_queue;
    std::mutex m_mutex;
};

static GCMessageQueue g_gcQueue;

// Сюда форвардер (в асинхронном режиме, из своего потока) кладёт ответы сервера.
//
// CSGC_DROP_GC=9110,9194 — не отдавать в игру сообщения этих типов.
// Нужно, чтобы найти, какое из сообщений сервера ломает состояние клиента
// (игра на коннекте бросает C++ исключение и убивает себя через unwind-действие).
static bool ShouldDropGCMessage(uint32_t msgType)
{
    static std::vector<uint32_t>* drop = nullptr;
    if (!drop) {
        drop = new std::vector<uint32_t>();
        const char* s = CSGCConfig::ConfigString("CSGC_DROP_GC");
        if (s && *s) {
            const char* p = s;
            while (*p) {
                while (*p == ' ' || *p == ',' || *p == ';') p++;
                if (!*p) break;
                char* end = nullptr;
                uint32_t v = (uint32_t)strtoul(p, &end, (*p == '0' && (p[1] == 'x' || p[1] == 'X')) ? 16 : 10);
                drop->push_back(v);
                p = end ? end : p + 1;
            }
        }
        if (!drop->empty()) {
            GCLog("[GC] CSGC_DROP_GC: dropping %d types:", (int)drop->size());
            for (uint32_t t : *drop) GCLog(" %u", t);
            GCLog("\n");
        }
    }
    for (uint32_t t : *drop)
        if (msgType == t || (msgType & 0xFFFF) == (t & 0xFFFF)) return true;
    return false;
}

static void HandleGCResponse(uint32_t msgType, std::vector<uint8_t>&& message)
{
    if (ShouldDropGCMessage(msgType)) {
        GCLog("[GC] dropped type 0x%08X (CSGC_DROP_GC)\n", msgType);
        return;
    }
    g_gcQueue.Push(msgType, std::move(message));
}

// ============================================================================
// SteamUtilsProxy
// ============================================================================
class SteamUtilsProxy final : public ISteamUtils
{
    ISteamUtils* m_original;
    static constexpr SteamAPICall_t kFakeCheckSig = 0x6666666666666666ull;

public:
    explicit SteamUtilsProxy(ISteamUtils* original) : m_original(original) {}

    SteamAPICall_t CheckFileSignature(const char* fileName) override
    {
        GCLog("[UTILS] CheckFileSignature('%s') -> спуф, отдам result=%d\n",
              fileName ? fileName : "(null)", CSGCConfig::SigResult());
        return kFakeCheckSig;
    }

    bool IsAPICallCompleted(SteamAPICall_t call, bool* failed) override
    {
        if (call == kFakeCheckSig) {
            TR("[T] > Utils::IsAPICallCompleted(fake)\n");
            if (failed) *failed = false;
            return true;
        }
        TR("[T] > Utils::IsAPICallCompleted(%llu)\n", (unsigned long long)call);
        bool r = m_original->IsAPICallCompleted(call, failed);
        TR("[T] < Utils::IsAPICallCompleted -> %d\n", r ? 1 : 0);
        return r;
    }

    ESteamAPICallFailure GetAPICallFailureReason(SteamAPICall_t call) override
    {
        if (call == kFakeCheckSig) return k_ESteamAPICallFailureNone;
        TR("[T] > Utils::GetAPICallFailureReason(%llu)\n", (unsigned long long)call);
        return m_original->GetAPICallFailureReason(call);
    }

    bool GetAPICallResult(SteamAPICall_t call, void* cb, int cbSize,
                          int cbExpected, bool* failed) override
    {
        if (call == kFakeCheckSig) {
            TR("[T] > Utils::GetAPICallResult(fake, size=%d, id=%d)\n", cbSize, cbExpected);
            if (failed) *failed = false;
            const int n = cbSize < (int)sizeof(CheckFileSignature_t)
                        ? cbSize : (int)sizeof(CheckFileSignature_t);
            CheckFileSignature_t result{};
            result.m_eCheckFileSignature = (ECheckFileSignature)CSGCConfig::SigResult();
            if (cb && n > 0) {
                memset(cb, 0, cbSize > 0 ? (size_t)cbSize : 0);
                memcpy(cb, &result, (size_t)n);
            }
            GCLog("[UTILS] CheckFileSignature result отдан (size=%d id=%d)\n", cbSize, cbExpected);
            return true;
        }
        TR("[T] > Utils::GetAPICallResult(call=%llu, size=%d, id=%d)\n",
           (unsigned long long)call, cbSize, cbExpected);
        bool r = m_original->GetAPICallResult(call, cb, cbSize, cbExpected, failed);
        TR("[T] < Utils::GetAPICallResult -> %d\n", r ? 1 : 0);
        return r;
    }

    uint32 GetAppID() override
    {
        uint32 r = m_original->GetAppID();
        TR("[T] > Utils::GetAppID -> %u\n", r);
        return r;
    }

    uint32 GetSecondsSinceAppActive() override { return m_original->GetSecondsSinceAppActive(); }
    uint32 GetSecondsSinceComputerActive() override { return m_original->GetSecondsSinceComputerActive(); }

    EUniverse GetConnectedUniverse() override
    {
        EUniverse r = m_original->GetConnectedUniverse();
        TR("[T] > Utils::GetConnectedUniverse -> %d\n", (int)r);
        return r;
    }

    uint32 GetServerRealTime() override { return m_original->GetServerRealTime(); }
    const char* GetIPCountry() override { return m_original->GetIPCountry(); }
    bool GetImageSize(int i, uint32* w, uint32* h) override { return m_original->GetImageSize(i, w, h); }
    bool GetImageRGBA(int i, uint8* d, int s) override { return m_original->GetImageRGBA(i, d, s); }

    uint8 GetCurrentBatteryPower() override { return m_original->GetCurrentBatteryPower(); }

    void SetOverlayNotificationPosition(ENotificationPosition p) override
        { m_original->SetOverlayNotificationPosition(p); }

    uint32 GetIPCCallCount() override { return m_original->GetIPCCallCount(); }

    void SetWarningMessageHook(SteamAPIWarningMessageHook_t fn) override
    {
        TR("[T] > Utils::SetWarningMessageHook(%p)\n", (void*)fn);
        m_original->SetWarningMessageHook(fn);
    }

    bool IsOverlayEnabled() override { return m_original->IsOverlayEnabled(); }
    bool BOverlayNeedsPresent() override { return m_original->BOverlayNeedsPresent(); }

    bool ShowGamepadTextInput(EGamepadTextInputMode m, EGamepadTextInputLineMode l,
                              const char* d, uint32 n, const char* e) override
        { return m_original->ShowGamepadTextInput(m, l, d, n, e); }
    uint32 GetEnteredGamepadTextLength() override { return m_original->GetEnteredGamepadTextLength(); }
    bool GetEnteredGamepadTextInput(char* p, uint32 c) override { return m_original->GetEnteredGamepadTextInput(p, c); }
    const char* GetSteamUILanguage() override { return m_original->GetSteamUILanguage(); }
    bool IsSteamRunningInVR() override { return m_original->IsSteamRunningInVR(); }
    void SetOverlayNotificationInset(int h, int v) override { m_original->SetOverlayNotificationInset(h, v); }
    bool IsSteamInBigPictureMode() override { return m_original->IsSteamInBigPictureMode(); }
    void StartVRDashboard() override { m_original->StartVRDashboard(); }
    bool IsVRHeadsetStreamingEnabled() override { return m_original->IsVRHeadsetStreamingEnabled(); }
    void SetVRHeadsetStreamingEnabled(bool e) override { m_original->SetVRHeadsetStreamingEnabled(e); }
    bool IsSteamChinaLauncher() override { return m_original->IsSteamChinaLauncher(); }
    bool InitFilterText(uint32 o) override { return m_original->InitFilterText(o); }
    int FilterText(ETextFilteringContext c, CSteamID s, const char* i,
                   char* o, uint32 n) override
        { return m_original->FilterText(c, s, i, o, n); }
    ESteamIPv6ConnectivityState GetIPv6ConnectivityState(ESteamIPv6ConnectivityProtocol p) override
        { return m_original->GetIPv6ConnectivityState(p); }
    bool IsSteamRunningOnSteamDeck() override { return m_original->IsSteamRunningOnSteamDeck(); }
    bool ShowFloatingGamepadTextInput(EFloatingGamepadTextInputMode m, int x, int y, int w, int h) override
        { return m_original->ShowFloatingGamepadTextInput(m, x, y, w, h); }
    void SetGameLauncherMode(bool m) override { m_original->SetGameLauncherMode(m); }

protected:
    bool GetCSERIPPort(uint32* ip, uint16* port) override
    {
        if (ip) *ip = 0;
        if (port) *port = 0;
        return false;
    }
    void RunFrame() override { TR("[T] > Utils::RunFrame\n"); }
};

// ============================================================================
// SteamUserStatsProxy
// ============================================================================
static std::vector<UserStatsReceived_t> g_userStatsQueue;
static std::mutex g_userStatsMutex;

class SteamUserStatsProxy final : public ISteamUserStats
{
    ISteamUserStats* m_original;

    void QueueStatsResult(CSteamID user)
    {
        UserStatsReceived_t cb{};
        cb.m_nGameID = CSGCConfig::kAppIdOverride;
        cb.m_eResult = k_EResultOK;
        cb.m_steamIDUser = user;
        std::lock_guard<std::mutex> lock(g_userStatsMutex);
        g_userStatsQueue.push_back(cb);
        GCLog("[STATS] UserStatsReceived in order (user=%llu)\n",
              user.ConvertToUint64());
    }

public:
    explicit SteamUserStatsProxy(ISteamUserStats* original) : m_original(original) {}

    bool RequestCurrentStats() override
    {
        GCLog("[STATS] RequestCurrentStats spoofed\n");
        CSteamID me;
        if (SteamUser()) me = SteamUser()->GetSteamID();
        else GCLog("[STATS] WARN: SteamUser() == null\n");
        QueueStatsResult(me);
        return true;
    }

    SteamAPICall_t RequestUserStats(CSteamID s) override
    {
        TR("[T] > Stats::RequestUserStats(%llu)\n", s.ConvertToUint64());
        QueueStatsResult(s);
        return 12345;
    }

    bool GetStat(const char* n, int32* d) override
    {
        if (CSGCConfig::SpoofStats()) { if (d) *d = 0; return true; }
        return m_original->GetStat(n, d);
    }
    bool GetStat(const char* n, float* d) override
    {
        if (CSGCConfig::SpoofStats()) { if (d) *d = 0.f; return true; }
        return m_original->GetStat(n, d);
    }

    bool SetStat(const char* n, int32 d) override { return m_original->SetStat(n, d); }
    bool SetStat(const char* n, float d) override { return m_original->SetStat(n, d); }
    bool UpdateAvgRateStat(const char* n, float c, double l) override { return m_original->UpdateAvgRateStat(n, c, l); }
    bool GetAchievement(const char* n, bool* a) override { return m_original->GetAchievement(n, a); }
    bool SetAchievement(const char* n) override { return m_original->SetAchievement(n); }
    bool ClearAchievement(const char* n) override { return m_original->ClearAchievement(n); }
    bool GetAchievementAndUnlockTime(const char* n, bool* a, uint32* t) override
        { return m_original->GetAchievementAndUnlockTime(n, a, t); }
    bool StoreStats() override { return m_original->StoreStats(); }
    int GetAchievementIcon(const char* n) override { return m_original->GetAchievementIcon(n); }
    const char* GetAchievementDisplayAttribute(const char* n, const char* k) override
        { return m_original->GetAchievementDisplayAttribute(n, k); }
    bool IndicateAchievementProgress(const char* n, uint32 c, uint32 m) override
        { return m_original->IndicateAchievementProgress(n, c, m); }
    uint32 GetNumAchievements() override { return m_original->GetNumAchievements(); }
    const char* GetAchievementName(uint32 i) override { return m_original->GetAchievementName(i); }
    bool GetUserStat(CSteamID s, const char* n, int32* d) override { return m_original->GetUserStat(s, n, d); }
    bool GetUserStat(CSteamID s, const char* n, float* d) override { return m_original->GetUserStat(s, n, d); }
    bool GetUserAchievement(CSteamID s, const char* n, bool* a) override
        { return m_original->GetUserAchievement(s, n, a); }
    bool GetUserAchievementAndUnlockTime(CSteamID s, const char* n, bool* a, uint32* t) override
        { return m_original->GetUserAchievementAndUnlockTime(s, n, a, t); }
    bool ResetAllStats(bool b) override { return m_original->ResetAllStats(b); }
    SteamAPICall_t FindOrCreateLeaderboard(const char* n, ELeaderboardSortMethod s,
                                           ELeaderboardDisplayType d) override
        { return m_original->FindOrCreateLeaderboard(n, s, d); }
    SteamAPICall_t FindLeaderboard(const char* n) override { return m_original->FindLeaderboard(n); }
    const char* GetLeaderboardName(SteamLeaderboard_t l) override { return m_original->GetLeaderboardName(l); }
    int GetLeaderboardEntryCount(SteamLeaderboard_t l) override { return m_original->GetLeaderboardEntryCount(l); }
    ELeaderboardSortMethod GetLeaderboardSortMethod(SteamLeaderboard_t l) override
        { return m_original->GetLeaderboardSortMethod(l); }
    ELeaderboardDisplayType GetLeaderboardDisplayType(SteamLeaderboard_t l) override
        { return m_original->GetLeaderboardDisplayType(l); }
    SteamAPICall_t DownloadLeaderboardEntries(SteamLeaderboard_t l, ELeaderboardDataRequest r,
                                              int s, int e) override
        { return m_original->DownloadLeaderboardEntries(l, r, s, e); }
    SteamAPICall_t DownloadLeaderboardEntriesForUsers(SteamLeaderboard_t l, CSteamID* u, int c) override
        { return m_original->DownloadLeaderboardEntriesForUsers(l, u, c); }
    bool GetDownloadedLeaderboardEntry(SteamLeaderboardEntries_t e, int i, LeaderboardEntry_t* d,
                                       int32* det, int m) override
        { return m_original->GetDownloadedLeaderboardEntry(e, i, d, det, m); }
    SteamAPICall_t UploadLeaderboardScore(SteamLeaderboard_t l, ELeaderboardUploadScoreMethod m,
                                          int32 s, const int32* det, int c) override
        { return m_original->UploadLeaderboardScore(l, m, s, det, c); }
    SteamAPICall_t AttachLeaderboardUGC(SteamLeaderboard_t l, UGCHandle_t u) override
        { return m_original->AttachLeaderboardUGC(l, u); }
    SteamAPICall_t GetNumberOfCurrentPlayers() override { return m_original->GetNumberOfCurrentPlayers(); }
    SteamAPICall_t RequestGlobalAchievementPercentages() override
        { return m_original->RequestGlobalAchievementPercentages(); }
    int GetMostAchievedAchievementInfo(char* n, uint32 l, float* p, bool* a) override
        { return m_original->GetMostAchievedAchievementInfo(n, l, p, a); }
    int GetNextMostAchievedAchievementInfo(int i, char* n, uint32 l, float* p, bool* a) override
        { return m_original->GetNextMostAchievedAchievementInfo(i, n, l, p, a); }
    bool GetAchievementAchievedPercent(const char* n, float* p) override
        { return m_original->GetAchievementAchievedPercent(n, p); }
    SteamAPICall_t RequestGlobalStats(int d) override { return m_original->RequestGlobalStats(d); }
    bool GetGlobalStat(const char* n, int64* d) override { return m_original->GetGlobalStat(n, d); }
    bool GetGlobalStat(const char* n, double* d) override { return m_original->GetGlobalStat(n, d); }
    int32 GetGlobalStatHistory(const char* n, int64* d, uint32 s) override
        { return m_original->GetGlobalStatHistory(n, d, s); }
    int32 GetGlobalStatHistory(const char* n, double* d, uint32 s) override
        { return m_original->GetGlobalStatHistory(n, d, s); }
    bool GetAchievementProgressLimits(const char* n, int32* mn, int32* mx) override
        { return m_original->GetAchievementProgressLimits(n, mn, mx); }
    bool GetAchievementProgressLimits(const char* n, float* mn, float* mx) override
        { return m_original->GetAchievementProgressLimits(n, mn, mx); }
};

// ============================================================================
// SteamUserProxy — только для трейса auth-тикетов (включается CSGC_HOOK_USER=1).
// По умолчанию НЕ используется: тикет должен выдавать настоящий Steam.
// ============================================================================
class SteamUserProxy final : public ISteamUser
{
    ISteamUser* m_original;

public:
    explicit SteamUserProxy(ISteamUser* original) : m_original(original) {}

    HSteamUser GetHSteamUser() override { return m_original->GetHSteamUser(); }
    bool BLoggedOn() override { return m_original->BLoggedOn(); }
    CSteamID GetSteamID() override { return m_original->GetSteamID(); }
    int InitiateGameConnection_DEPRECATED(void* a, int b, CSteamID c, uint32 d, uint16 e, bool f) override
    {
        GCLog("[USER] InitiateGameConnection -> ...\n");
        int r = m_original->InitiateGameConnection_DEPRECATED(a, b, c, d, e, f);
        GCLog("[USER] InitiateGameConnection -> %d\n", r);
        return r;
    }
    void TerminateGameConnection_DEPRECATED(uint32 a, uint16 b) override
        { m_original->TerminateGameConnection_DEPRECATED(a, b); }
    void TrackAppUsageEvent(CGameID g, int e, const char* i) override
        { m_original->TrackAppUsageEvent(g, e, i); }
    bool GetUserDataFolder(char* b, int c) override { return m_original->GetUserDataFolder(b, c); }
    void StartVoiceRecording() override { m_original->StartVoiceRecording(); }
    void StopVoiceRecording() override { m_original->StopVoiceRecording(); }
    EVoiceResult GetAvailableVoice(uint32* a, uint32* b, uint32 c) override
        { return m_original->GetAvailableVoice(a, b, c); }
    EVoiceResult GetVoice(bool a, void* b, uint32 c, uint32* d, bool e, void* f, uint32 g, uint32* h, uint32 i) override
        { return m_original->GetVoice(a, b, c, d, e, f, g, h, i); }
    EVoiceResult DecompressVoice(const void* a, uint32 b, void* c, uint32 d, uint32* e, uint32 f) override
        { return m_original->DecompressVoice(a, b, c, d, e, f); }
    uint32 GetVoiceOptimalSampleRate() override { return m_original->GetVoiceOptimalSampleRate(); }
    HAuthTicket GetAuthSessionTicket(void* t, int m, uint32* c,
                                     const SteamNetworkingIdentity* i) override
    {
        GCLog("[USER] GetAuthSessionTicket: -> ...\n");
        HAuthTicket r = m_original->GetAuthSessionTicket(t, m, c, i);
        GCLog("[USER] GetAuthSessionTicket -> handle=%u size=%u\n", r, c ? *c : 0);
        return r;
    }
    EBeginAuthSessionResult BeginAuthSession(const void* t, int c, CSteamID s) override
    {
        EBeginAuthSessionResult r = m_original->BeginAuthSession(t, c, s);
        GCLog("[USER] BeginAuthSession -> %d\n", (int)r);
        return r;
    }
    void EndAuthSession(CSteamID s) override { m_original->EndAuthSession(s); }
    void CancelAuthTicket(HAuthTicket t) override { m_original->CancelAuthTicket(t); }
    EUserHasLicenseForAppResult UserHasLicenseForApp(CSteamID s, AppId_t a) override
        { return m_original->UserHasLicenseForApp(s, a); }
    bool BIsBehindNAT() override { return m_original->BIsBehindNAT(); }
    void AdvertiseGame(CSteamID s, uint32 i, uint16 p) override { m_original->AdvertiseGame(s, i, p); }
    SteamAPICall_t RequestEncryptedAppTicket(void* d, int c) override
        { return m_original->RequestEncryptedAppTicket(d, c); }
    bool GetEncryptedAppTicket(void* t, int m, uint32* c) override
        { return m_original->GetEncryptedAppTicket(t, m, c); }
    int GetGameBadgeLevel(int s, bool f) override { return m_original->GetGameBadgeLevel(s, f); }
    int GetPlayerSteamLevel() override { return m_original->GetPlayerSteamLevel(); }
    SteamAPICall_t RequestStoreAuthURL(const char* u) override { return m_original->RequestStoreAuthURL(u); }
    bool BIsPhoneVerified() override { return m_original->BIsPhoneVerified(); }
    bool BIsTwoFactorEnabled() override { return m_original->BIsTwoFactorEnabled(); }
    bool BIsPhoneIdentifying() override { return m_original->BIsPhoneIdentifying(); }
    bool BIsPhoneRequiringVerification() override { return m_original->BIsPhoneRequiringVerification(); }
    SteamAPICall_t GetMarketEligibility() override { return m_original->GetMarketEligibility(); }
    SteamAPICall_t GetDurationControl() override { return m_original->GetDurationControl(); }
    bool BSetDurationControlOnlineState(EDurationControlOnlineState s) override
        { return m_original->BSetDurationControlOnlineState(s); }
};

// ============================================================================
// SteamGameCoordinatorProxy — TCP-редирект
// ============================================================================
class SteamGameCoordinatorProxy final : public ISteamGameCoordinator
{
    ISteamGameCoordinator* m_original = nullptr;

public:
    void SetOriginal(ISteamGameCoordinator* original) { m_original = original; }

    EGCResults SendMessage(uint32 type, const void* data, uint32 size) override
    {
        GCLog("[GC] SendMessage: type=0x%08X, size=%u\n", type, size);

        if (CSGCConfig::NoForward()) {
            GCLog("[GC] CSGC_NO_FORWARD — not redirecting\n");
            return k_EGCResultOK;
        }

        uint64_t steamId = 0;
        if (SteamUser()) steamId = SteamUser()->GetSteamID().ConvertToUint64();
        if (steamId == 0) steamId = CSGCConfig::TestSteamId();

        ExternalGCResponse response;
        if (!ForwardToExternalServer(steamId, type, data, size, response)) {
            GCLog("[GC] no available answers\n");
            return k_EGCResultOK;
        }

        DeliverGCResponse(response);
        return k_EGCResultOK;
    }

    bool IsMessageAvailable(uint32* size) override
    {
        bool avail = g_gcQueue.IsAvailable(*size);
        if (avail) GCLog("[GC] IsMessageAvailable: %d, size=%u\n", 1, *size);
        return avail;
    }

    EGCResults RetrieveMessage(uint32* type, void* dest, uint32 destSize, uint32* size) override
    {
        bool ok = g_gcQueue.Retrieve(*type, dest, destSize, *size);
        GCLog("[GC] RetrieveMessage: type=0x%08X, destSize=%u, size=%u, ok=%d\n",
            *type, destSize, *size, ok ? 1 : 0);
        if (!ok) {
            if (destSize < *size) return k_EGCResultBufferTooSmall;
            return k_EGCResultNoMessage;
        }
        return k_EGCResultOK;
    }
};

// ============================================================================
// Callback hooks
// ============================================================================
struct CallbackHook { int id; CCallbackBase* cb; };
static std::vector<CallbackHook> g_callbackHooks;
static std::mutex g_callbackMutex;

static bool ShouldHookCallback(int id)
{
    switch (id) {
    case GCMessageAvailable_t::k_iCallback:              // 1701
    case GCMessageFailed_t::k_iCallback:
        return true;
    default:
        return false;
    }
}

static void DeliverCallbacksById(int id, void* param)
{
    std::vector<CCallbackBase*> snapshot;
    {
        std::lock_guard<std::mutex> lock(g_callbackMutex);
        for (auto& h : g_callbackHooks)
            if (h.id == id) snapshot.push_back(h.cb);
    }
    for (CCallbackBase* cb : snapshot) {
        bool alive = false;
        {
            std::lock_guard<std::mutex> lock(g_callbackMutex);
            for (auto& h : g_callbackHooks)
                if (h.cb == cb && h.id == id) { alive = true; break; }
        }
        if (!alive) continue;
        cb->Run(param);
    }
}

static void DrainUserStatsQueue()
{
    std::vector<UserStatsReceived_t> pending;
    {
        std::lock_guard<std::mutex> lock(g_userStatsMutex);
        pending.swap(g_userStatsQueue);
    }
    for (auto& cb : pending) {
        GCLog("[STATS] delivering UserStatsReceived (user=%llu, result=%d)\n",
              cb.m_steamIDUser.ConvertToUint64(), (int)cb.m_eResult);
        DeliverCallbacksById(UserStatsReceived_t::k_iCallback, &cb);
    }
}

// ============================================================================
// Прокси на пару (pipe, user). unique_ptr => адреса стабильны при рехэше.
// ============================================================================
class SteamInterfaceProxy
{
    HSteamPipe m_pipe;

public:
    std::unique_ptr<SteamGameCoordinatorProxy> gc;
    std::unique_ptr<SteamUtilsProxy> utils;
    std::unique_ptr<SteamUserStatsProxy> stats;
    std::unique_ptr<SteamUserProxy> user;

    explicit SteamInterfaceProxy(HSteamPipe pipe) : m_pipe(pipe) {}

    void* Get(const char* version, void* original)
    {
        if (!CSGCConfig::NoGC() &&
            InterfaceNameEquals(version, STEAMGAMECOORDINATOR_INTERFACE_VERSION)) {
            GCLog("[IFACE] returning GameCoordinatorProxy\n");
            if (!gc) gc = std::make_unique<SteamGameCoordinatorProxy>();
            gc->SetOriginal((ISteamGameCoordinator*)original);
            return gc.get();
        }

        if (!CSGCConfig::NoUtils() &&
            InterfaceNameEquals(version, STEAMUTILS_INTERFACE_VERSION)) {
            GCLog("[IFACE] returning SteamUtilsProxy\n");
            if (!utils) utils = std::make_unique<SteamUtilsProxy>((ISteamUtils*)original);
            return utils.get();
        }

        if (!CSGCConfig::NoStats() &&
            InterfaceNameEquals(version, STEAMUSERSTATS_INTERFACE_VERSION)) {
            GCLog("[IFACE] returning SteamUserStatsProxy\n");
            if (!stats) stats = std::make_unique<SteamUserStatsProxy>((ISteamUserStats*)original);
            return stats.get();
        }

        if (CSGCConfig::HookUser() &&
            InterfaceNameEquals(version, STEAMUSER_INTERFACE_VERSION)) {
            GCLog("[IFACE] returning SteamUserProxy (CSGC_HOOK_USER=1)\n");
            if (!user) user = std::make_unique<SteamUserProxy>((ISteamUser*)original);
            return user.get();
        }

        TR("[T]   (interface %s not proxying)\n", version ? version : "(null)");
        return nullptr;
    }
};

// ============================================================================
// SteamClientProxy
// ============================================================================
class SteamClientProxy final : public ISteamClient
{
    ISteamClient* m_original{};
    std::unordered_map<uint64_t, std::unique_ptr<SteamInterfaceProxy>> m_proxies;
    std::mutex m_mutex;

    static uint64_t Key(HSteamPipe p, HSteamUser u)
    {
        return (uint64_t)p | ((uint64_t)u << 32);
    }

    SteamInterfaceProxy* GetProxy(HSteamPipe p, HSteamUser u)
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        auto it = m_proxies.find(Key(p, u));
        if (it == m_proxies.end())
            it = m_proxies.emplace(Key(p, u), std::make_unique<SteamInterfaceProxy>(p)).first;
        return it->second.get();
    }

    template<typename T>
    T* Route(T* original, HSteamPipe p, HSteamUser u, const char* version)
    {
        if (!original) return nullptr;
        SteamInterfaceProxy* proxy = GetProxy(p, u);
        void* result = proxy ? proxy->Get(version, original) : nullptr;
        return result ? static_cast<T*>(result) : original;
    }

public:
    void SetOriginal(ISteamClient* original) { m_original = original; }

    HSteamPipe CreateSteamPipe() override
    {
        HSteamPipe p = m_original->CreateSteamPipe();
        TR("[T] > Client::CreateSteamPipe -> %d\n", (int)p);
        return p;
    }
    bool BReleaseSteamPipe(HSteamPipe p) override
    {
        TR("[T] > Client::BReleaseSteamPipe(%d)\n", (int)p);
        return m_original->BReleaseSteamPipe(p);
    }
    HSteamUser ConnectToGlobalUser(HSteamPipe p) override
    {
        HSteamUser u = m_original->ConnectToGlobalUser(p);
        TR("[T] > Client::ConnectToGlobalUser -> %d\n", (int)u);
        return u;
    }
    HSteamUser CreateLocalUser(HSteamPipe* p, EAccountType t) override
        { return m_original->CreateLocalUser(p, t); }
    void ReleaseUser(HSteamPipe p, HSteamUser u) override { m_original->ReleaseUser(p, u); }

    ISteamUser* GetISteamUser(HSteamUser u, HSteamPipe p, const char* v) override
    {
        TR("[T] > Client::GetISteamUser(%s)\n", v ? v : "(null)");
        return Route(m_original->GetISteamUser(u, p, v), p, u, v);
    }
    ISteamGameServer* GetISteamGameServer(HSteamUser u, HSteamPipe p, const char* v) override
        { return m_original->GetISteamGameServer(u, p, v); }
    void SetLocalIPBinding(const SteamIPAddress_t& ip, uint16 port) override
        { m_original->SetLocalIPBinding(ip, port); }
    ISteamFriends* GetISteamFriends(HSteamUser u, HSteamPipe p, const char* v) override
        { return m_original->GetISteamFriends(u, p, v); }
    ISteamUtils* GetISteamUtils(HSteamPipe p, const char* v) override
    {
        TR("[T] > Client::GetISteamUtils(%s)\n", v ? v : "(null)");
        return Route(m_original->GetISteamUtils(p, v), p, 0, v);
    }
    ISteamMatchmaking* GetISteamMatchmaking(HSteamUser u, HSteamPipe p, const char* v) override
        { return m_original->GetISteamMatchmaking(u, p, v); }
    ISteamMatchmakingServers* GetISteamMatchmakingServers(HSteamUser u, HSteamPipe p, const char* v) override
        { return m_original->GetISteamMatchmakingServers(u, p, v); }

    void* GetISteamGenericInterface(HSteamUser u, HSteamPipe p, const char* v) override
    {
        GCLog("[CLIENT] GetISteamGenericInterface: %s\n", v ? v : "(null)");
        void* original = m_original->GetISteamGenericInterface(u, p, v);
        if (!original) return nullptr;
        SteamInterfaceProxy* proxy = GetProxy(p, u);
        void* result = proxy ? proxy->Get(v, original) : nullptr;
        return result ? result : original;
    }

    ISteamUserStats* GetISteamUserStats(HSteamUser u, HSteamPipe p, const char* v) override
    {
        TR("[T] > Client::GetISteamUserStats(%s)\n", v ? v : "(null)");
        return Route(m_original->GetISteamUserStats(u, p, v), p, u, v);
    }
    ISteamGameServerStats* GetISteamGameServerStats(HSteamUser u, HSteamPipe p, const char* v) override
        { return m_original->GetISteamGameServerStats(u, p, v); }
    ISteamApps* GetISteamApps(HSteamUser u, HSteamPipe p, const char* v) override
        { return m_original->GetISteamApps(u, p, v); }
    ISteamNetworking* GetISteamNetworking(HSteamUser u, HSteamPipe p, const char* v) override
        { return m_original->GetISteamNetworking(u, p, v); }
    ISteamRemoteStorage* GetISteamRemoteStorage(HSteamUser u, HSteamPipe p, const char* v) override
        { return m_original->GetISteamRemoteStorage(u, p, v); }
    ISteamScreenshots* GetISteamScreenshots(HSteamUser u, HSteamPipe p, const char* v) override
        { return m_original->GetISteamScreenshots(u, p, v); }

    void RunFrame() override
    {
        TR("[T] > Client::RunFrame (vtable #20, заглушка)\n");
    }

    uint32 GetIPCCallCount() override { return m_original->GetIPCCallCount(); }
    void SetWarningMessageHook(SteamAPIWarningMessageHook_t fn) override { m_original->SetWarningMessageHook(fn); }
    bool BShutdownIfAllPipesClosed() override { return m_original->BShutdownIfAllPipesClosed(); }
    ISteamHTTP* GetISteamHTTP(HSteamUser u, HSteamPipe p, const char* v) override
        { return m_original->GetISteamHTTP(u, p, v); }
    void* DEPRECATED_GetISteamUnifiedMessages(HSteamUser, HSteamPipe, const char*) override
        { return nullptr; }
    ISteamController* GetISteamController(HSteamUser u, HSteamPipe p, const char* v) override
        { return m_original->GetISteamController(u, p, v); }
    ISteamUGC* GetISteamUGC(HSteamUser u, HSteamPipe p, const char* v) override
        { return m_original->GetISteamUGC(u, p, v); }
    ISteamAppList* GetISteamAppList(HSteamUser u, HSteamPipe p, const char* v) override
        { return m_original->GetISteamAppList(u, p, v); }
    ISteamMusic* GetISteamMusic(HSteamUser u, HSteamPipe p, const char* v) override
        { return m_original->GetISteamMusic(u, p, v); }
    ISteamMusicRemote* GetISteamMusicRemote(HSteamUser u, HSteamPipe p, const char* v) override
        { return m_original->GetISteamMusicRemote(u, p, v); }
    ISteamHTMLSurface* GetISteamHTMLSurface(HSteamUser u, HSteamPipe p, const char* v) override
        { return m_original->GetISteamHTMLSurface(u, p, v); }
    void DEPRECATED_Set_SteamAPI_CPostAPIResultInProcess(void (*)()) override {}
    void DEPRECATED_Remove_SteamAPI_CPostAPIResultInProcess(void (*)()) override {}
    void Set_SteamAPI_CCheckCallbackRegisteredInProcess(SteamAPI_CheckCallbackRegistered_t) override {}
    ISteamInventory* GetISteamInventory(HSteamUser u, HSteamPipe p, const char* v) override
        { return m_original->GetISteamInventory(u, p, v); }
    ISteamVideo* GetISteamVideo(HSteamUser u, HSteamPipe p, const char* v) override
        { return m_original->GetISteamVideo(u, p, v); }
    ISteamParentalSettings* GetISteamParentalSettings(HSteamUser u, HSteamPipe p, const char* v) override
        { return m_original->GetISteamParentalSettings(u, p, v); }
    ISteamInput* GetISteamInput(HSteamUser u, HSteamPipe p, const char* v) override
        { return m_original->GetISteamInput(u, p, v); }
    ISteamParties* GetISteamParties(HSteamUser u, HSteamPipe p, const char* v) override
        { return m_original->GetISteamParties(u, p, v); }
    ISteamRemotePlay* GetISteamRemotePlay(HSteamUser u, HSteamPipe p, const char* v) override
        { return m_original->GetISteamRemotePlay(u, p, v); }
    ISteamGameSearch* GetISteamGameSearch(HSteamUser u, HSteamPipe p, const char* v) override
        { return m_original->GetISteamGameSearch(u, p, v); }

    void DestroyAllInterfaces() override
    {
        GCLog("[CLIENT] DestroyAllInterfaces called by game\n");
    }
};

static SteamClientProxy* g_steamClientProxy = nullptr;
static SteamClientProxy& ClientProxy()
{
    if (!g_steamClientProxy) g_steamClientProxy = new SteamClientProxy();
    return *g_steamClientProxy;
}

// ============================================================================
// Хуки
// ============================================================================
static void* (*Og_CreateInterface)(const char*, int*) = nullptr;
static void (*Og_SteamAPI_RegisterCallback)(CCallbackBase*, int) = nullptr;
static void (*Og_SteamAPI_UnregisterCallback)(CCallbackBase*) = nullptr;
static void (*Og_SteamAPI_RunCallbacks)() = nullptr;
static void (*Og_RegisterCallResult)(CCallbackBase*, SteamAPICall_t) = nullptr;
static void (*Og_UnregisterCallResult)(CCallbackBase*, SteamAPICall_t) = nullptr;

static void* Hk_CreateInterface(const char* name, int* err)
{
    void* result = Og_CreateInterface(name, err);
    GCLog("[HOOK] CreateInterface: %s -> %p\n", name, result);

    if (InterfaceNameEquals(name, STEAMCLIENT_INTERFACE_VERSION)) {
        if (CSGCConfig::NoClient()) {
            GCLog("[HOOK] CSGC_NO_CLIENT — SteamClient not proxying\n");
            return result;
        }
        GCLog("[HOOK] proxying SteamClient\n");
        ClientProxy().SetOriginal(static_cast<ISteamClient*>(result));
        return &ClientProxy();
    }
    return result;
}

static void Hk_SteamAPI_RegisterCallback(CCallbackBase* cb, int id)
{
    if (ShouldHookCallback(id)) {
        GCLog("[CB] hooked callback id=%d (%p)\n", id, (void*)cb);
        {
            std::lock_guard<std::mutex> lock(g_callbackMutex);
            g_callbackHooks.push_back({ id, cb });
        }
        Og_SteamAPI_RegisterCallback(cb, id);
        return;
    }
    Og_SteamAPI_RegisterCallback(cb, id);
}

static void Hk_SteamAPI_UnregisterCallback(CCallbackBase* cb)
{
    {
        std::lock_guard<std::mutex> lock(g_callbackMutex);
        for (auto it = g_callbackHooks.begin(); it != g_callbackHooks.end(); ++it) {
            if (it->cb == cb) {
                GCLog("[CB] unsubscribed callback id=%d (%p)\n", it->id, (void*)cb);
                g_callbackHooks.erase(it);
                break;
            }
        }
    }
    Og_SteamAPI_UnregisterCallback(cb);
}

static void Hk_RegisterCallResult(CCallbackBase* cb, SteamAPICall_t call)
{
    GCLog("[CR] RegisterCallResult cb=%p call=%llu (id=%d)\n",
          (void*)cb, (unsigned long long)call, cb ? cb->GetICallback() : -1);
    Og_RegisterCallResult(cb, call);
}

static void Hk_UnregisterCallResult(CCallbackBase* cb, SteamAPICall_t call)
{
    GCLog("[CR] UnregisterCallResult cb=%p call=%llu\n", (void*)cb, (unsigned long long)call);
    Og_UnregisterCallResult(cb, call);
}

static void Hk_SteamAPI_RunCallbacks()
{
    Og_SteamAPI_RunCallbacks();

    if (!CSGCConfig::NoStatsCb())
        DrainUserStatsQueue();

    if (!CSGCConfig::NoCallbacks()) {
        uint32 msgSize = 0;
        if (g_gcQueue.IsAvailable(msgSize)) {
            GCMessageAvailable_t param{};
            param.m_nMessageSize = msgSize;
            DeliverCallbacksById(GCMessageAvailable_t::k_iCallback, &param);
        }
    }
}

// ============================================================================
// Диагностика выхода из игры.
//
// Когда Source зовёт Error(), он печатает сообщение и вызывает
// Plat_ExitProcess — процесс завершается «чисто»: WER молчит, дампа нет,
// в Event Log пусто. Снаружи это выглядит как «игра просто вылетела».
// tier0.dll экспортирует Error и Plat_ExitProcess ПРОСТЫМИ именами
// (без манглинга), поэтому перехватываем их и пишем причину в лог.
// ============================================================================
static void (__cdecl *Og_Error)(const char*, ...) = nullptr;
static void (__cdecl *Og_Plat_ExitProcess)(int) = nullptr;

static void __cdecl Hk_Error(const char* fmt, ...)
{
    char buf[4096] = { 0 };
    if (fmt) {
        va_list ap;
        va_start(ap, fmt);
        vsnprintf(buf, sizeof(buf), fmt, ap);
        va_end(ap);
    }
    GCLog("\n[TIER0] !!! Error(): %s\n", buf);
    LogStackScan("Error()");

    if (Og_Error) Og_Error("%s", buf);
    ExitProcess(1);   // Error() не возвращается
}

static void __cdecl Hk_Plat_ExitProcess(int code)
{
    GCLog("\n[TIER0] Plat_ExitProcess(%d) — process closing\n", code);
    LogStackScan("Plat_ExitProcess()");

    if (Og_Plat_ExitProcess) Og_Plat_ExitProcess(code);
    ExitProcess((UINT)code);
}

// ============================================================================
// Кто завершает процесс.
//
// Error()/Plat_ExitProcess — не единственные пути. Если процесс гасят напрямую
// через kernel32!ExitProcess или TerminateProcess, tier0-хуки молчат, WER молчит,
// дампа нет. Перехватываем оба и печатаем стек вызовов — он покажет модуль+RVA
// того, кто это сделал.
// ============================================================================
static void (WINAPI *Og_ExitProcess)(UINT) = nullptr;
static BOOL (WINAPI *Og_TerminateProcess)(HANDLE, UINT) = nullptr;

static void WINAPI Hk_ExitProcess(UINT code)
{
    // _ReturnAddress() — точный адрес того, кто позвал, без эвристик по стеку.
    const char* from = DescribeAddress(_ReturnAddress());
    GCLog("\n[EXIT] ExitProcess(%u) — called from %s\n", code, from);
    LogStackScan("ExitProcess()");
    if (Og_ExitProcess) Og_ExitProcess(code);
    TerminateProcess(GetCurrentProcess(), code);
}

static BOOL WINAPI Hk_TerminateProcess(HANDLE h, UINT code)
{
    const bool self = (h == GetCurrentProcess() || h == (HANDLE)(intptr_t)-1);
    const char* from = DescribeAddress(_ReturnAddress());
    GCLog("\n[EXIT] TerminateProcess(handle=%p, code=%u)%s called by: %s\n",
          (void*)h, code, self ? "  <-- self" : "", from);
    if (self) LogStackScan("TerminateProcess()");
    if (Og_TerminateProcess) return Og_TerminateProcess(h, code);
    return FALSE;
}

// ============================================================================
// Перехват консольного вывода движка.
//
// Выход идёт через TerminateProcess, поэтому console.log НЕ флашится и его хвост
// теряется — именно поэтому мы не видели сообщения о причине. Ловим tier0!Msg и
// tier0!Warning: наш лог пишется с флашем на каждой строке, так что последнее
// сообщение движка перед смертью гарантированно останется на диске.
// ============================================================================
static void (__cdecl *Og_Msg)(const char*, ...) = nullptr;
static void (__cdecl *Og_Warning)(const char*, ...) = nullptr;
static void (__cdecl *Og_ConMsg)(const char*, ...) = nullptr;
static void (__cdecl *Og_ConColorMsg)(const void*, const char*, ...) = nullptr;

static void __cdecl Hk_ConMsg(const char* fmt, ...)
{
    char buf[4000];
    buf[0] = '\0';
    if (fmt) {
        va_list ap;
        va_start(ap, fmt);
        vsnprintf(buf, sizeof(buf), fmt, ap);
        va_end(ap);
    }
    GCLog("[CON] %s", buf);
    if (Og_ConMsg) Og_ConMsg("%s", buf);
}

static void __cdecl Hk_ConColorMsg(const void* color, const char* fmt, ...)
{
    char buf[4000];
    buf[0] = '\0';
    if (fmt) {
        va_list ap;
        va_start(ap, fmt);
        vsnprintf(buf, sizeof(buf), fmt, ap);
        va_end(ap);
    }
    GCLog("[CONC] %s", buf);
    if (Og_ConColorMsg) Og_ConColorMsg(color, "%s", buf);
}

static void __cdecl Hk_Msg(const char* fmt, ...)
{
    char buf[4000];
    buf[0] = '\0';
    if (fmt) {
        va_list ap;
        va_start(ap, fmt);
        vsnprintf(buf, sizeof(buf), fmt, ap);
        va_end(ap);
    }
    GCLog("[MSG] %s", buf);
    if (Og_Msg) Og_Msg("%s", buf);
}

static void __cdecl Hk_Warning(const char* fmt, ...)
{
    char buf[4000];
    buf[0] = '\0';
    if (fmt) {
        va_list ap;
        va_start(ap, fmt);
        vsnprintf(buf, sizeof(buf), fmt, ap);
        va_end(ap);
    }
    GCLog("[WARN] %s", buf);
    if (Og_Warning) Og_Warning("%s", buf);
}

// ============================================================================
// Установка хуков через funchook
// ============================================================================
static std::vector<funchook_t*>& FunchookHandles()
{
    static std::vector<funchook_t*>* v = new std::vector<funchook_t*>();
    return *v;
}

static int g_hooksOk = 0;
static int g_hooksFailed = 0;
static std::vector<const char*> g_failedNames;

static bool HookCreate(const char* name, void* target, void* hook, void** bridge)
{
    if (!target || !hook || !bridge) {
        GCLog("[HOOK] ERROR %s: function not found\n", name);
        g_hooksFailed++;
        g_failedNames.push_back(name);
        return false;
    }

    funchook_t* fh = funchook_create();
    if (!fh) {
        GCLog("[HOOK] ERROR %s: funchook_create returned NULL\n", name);
        g_hooksFailed++;
        g_failedNames.push_back(name);
        return false;
    }

    void* temp = target;
    int rv = funchook_prepare(fh, &temp, hook);
    if (rv != 0) {
        GCLog("[HOOK] ERROR %s: prepare rv=%d (%s)\n", name, rv, funchook_error_message(fh));
        FunchookHandles().push_back(fh);
        g_hooksFailed++;
        g_failedNames.push_back(name);
        return false;
    }
    *bridge = temp;

    rv = funchook_install(fh, 0);
    if (rv != 0) {
        GCLog("[HOOK] ERROR %s: install rv=%d (%s)\n", name, rv, funchook_error_message(fh));
        *bridge = nullptr;
        FunchookHandles().push_back(fh);
        g_hooksFailed++;
        g_failedNames.push_back(name);
        return false;
    }

    FunchookHandles().push_back(fh);
    g_hooksOk++;
    GCLog("[HOOK] ok - hook on %s (orig=%p)\n", name, *bridge);
    return true;
}

// ============================================================================
// Точка входа
// ============================================================================
static DWORD WINAPI WaitForSteamClientThread(LPVOID);

void InstallSteamHooks()
{
    InstallCrashReporter();
    CSGCConfig::DumpToLog();

    GCLog("[HOOK] === InstallSteamHooks ===\n");

    char appIdBuf[16];
    sprintf(appIdBuf, "%u", CSGCConfig::kAppIdOverride);
    SetEnvironmentVariableA("SteamAppId", appIdBuf);
    GCLog("[HOOK] SteamAppId=%s\n", appIdBuf);

    SetGCResponseSink(HandleGCResponse);
    StartForwarderIfAsync();

    CreateThread(nullptr, 0, WaitForSteamClientThread, nullptr, 0, nullptr);
    GCLog("[HOOK] waiting stream started\n");
}

static DWORD WINAPI WaitForSteamClientThread(LPVOID)
{
    GCLog("[HOOK] waiter thread started\n");

    HMODULE tier0 = nullptr;
    for (int i = 0; i < 300; i++) {
        tier0 = GetModuleHandleA("tier0.dll");
        if (tier0) break;
        Sleep(100);
    }
    if (tier0) {
        GCLog("[HOOK] tier0.dll on address %p\n", tier0);
        HookCreate("tier0!Error", GetProcAddress(tier0, "Error"),
                   (void*)Hk_Error, (void**)&Og_Error);
        HookCreate("tier0!Plat_ExitProcess", GetProcAddress(tier0, "Plat_ExitProcess"),
                   (void*)Hk_Plat_ExitProcess, (void**)&Og_Plat_ExitProcess);
        if (CSGCConfig::Trace()) {
            HookCreate("tier0!Msg", GetProcAddress(tier0, "Msg"),
                       (void*)Hk_Msg, (void**)&Og_Msg);
            HookCreate("tier0!Warning", GetProcAddress(tier0, "Warning"),
                       (void*)Hk_Warning, (void**)&Og_Warning);
            HookCreate("tier0!ConMsg", GetProcAddress(tier0, "?ConMsg@@YAXPBDZZ"),
                       (void*)Hk_ConMsg, (void**)&Og_ConMsg);
            HookCreate("tier0!ConColorMsg", GetProcAddress(tier0, "?ConColorMsg@@YAXABVColor@@PBDZZ"),
                       (void*)Hk_ConColorMsg, (void**)&Og_ConColorMsg);
        }
    } else {
        GCLog("[HOOK] tier0.dll not appeared\n");
    }

    HMODULE kernel32 = GetModuleHandleA("kernel32.dll");
    if (kernel32) {
        HookCreate("kernel32!ExitProcess", GetProcAddress(kernel32, "ExitProcess"),
                   (void*)Hk_ExitProcess, (void**)&Og_ExitProcess);
        HookCreate("kernel32!TerminateProcess", GetProcAddress(kernel32, "TerminateProcess"),
                   (void*)Hk_TerminateProcess, (void**)&Og_TerminateProcess);
    }

    HMODULE steamclient = nullptr;
    for (int i = 0; i < 600; i++) {
        steamclient = GetModuleHandleA("steamclient.dll");
        if (steamclient) break;
        if (i > 0 && i % 50 == 0)
            GCLog("[HOOK] waiting steamclient.dll... (%d ms)\n", i * 100);
        Sleep(100);
    }
    if (!steamclient) {
        GCLog("[HOOK] FAILED: steamclient.dll not appeared after 60с\n");
        return 1;
    }
    GCLog("[HOOK] steamclient.dll on address %p\n", steamclient);

    void* createInterface = GetProcAddress(steamclient, "CreateInterface");
    if (createInterface)
        HookCreate("CreateInterface", createInterface,
                   (void*)Hk_CreateInterface, (void**)&Og_CreateInterface);
    else
        GCLog("[HOOK] CreateInterface not found!\n");

    HMODULE steamApi = GetModuleHandleA("steam_api.dll");
    if (!steamApi) {
        GCLog("[HOOK] steam_api.dll not loaded, waiting...\n");
        for (int i = 0; i < 100; i++) {
            steamApi = GetModuleHandleA("steam_api.dll");
            if (steamApi) break;
            Sleep(100);
        }
    }

    if (steamApi) {
        GCLog("[HOOK] steam_api.dll on address %p\n", steamApi);

        void* reg = GetProcAddress(steamApi, "SteamAPI_RegisterCallback");
        void* unreg = GetProcAddress(steamApi, "SteamAPI_UnregisterCallback");
        void* run = GetProcAddress(steamApi, "SteamAPI_RunCallbacks");
        void* regcr = GetProcAddress(steamApi, "SteamAPI_RegisterCallResult");
        void* unregcr = GetProcAddress(steamApi, "SteamAPI_UnregisterCallResult");

        if (reg) HookCreate("SteamAPI_RegisterCallback", reg,
            (void*)Hk_SteamAPI_RegisterCallback, (void**)&Og_SteamAPI_RegisterCallback);
        if (unreg) HookCreate("SteamAPI_UnregisterCallback", unreg,
            (void*)Hk_SteamAPI_UnregisterCallback, (void**)&Og_SteamAPI_UnregisterCallback);
        if (run) HookCreate("SteamAPI_RunCallbacks", run,
            (void*)Hk_SteamAPI_RunCallbacks, (void**)&Og_SteamAPI_RunCallbacks);
        if (!CSGCConfig::NoCallResult()) {
            if (regcr) HookCreate("SteamAPI_RegisterCallResult", regcr,
                (void*)Hk_RegisterCallResult, (void**)&Og_RegisterCallResult);
            if (unregcr) HookCreate("SteamAPI_UnregisterCallResult", unregcr,
                (void*)Hk_UnregisterCallResult, (void**)&Og_UnregisterCallResult);
        }
    } else {
        GCLog("[HOOK] steam_api.dll not loaded\n");
    }

    if (!g_failedNames.empty()) {
        GCLog("[HOOK] FAILED %d hook(s):", (int)g_failedNames.size());
        for (const char* n : g_failedNames) GCLog(" %s", n);
        GCLog("\n");
        GCLog("[HOOK] without SteamAPI_RunCallbacks GC won't work!\n");
    } else {
        GCLog("[HOOK] all hooks installed (ok=%d)\n", g_hooksOk);
    }
    return 0;
}
