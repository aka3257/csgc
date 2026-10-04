#define WIN32_LEAN_AND_MEAN
#define _WINSOCKAPI_

#include "forwarder.h"
#include "log.h"
#include "config.h"

#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>

#include <cstdio>
#include <cstring>
#include <string>
#include <deque>
#include <mutex>
#include <atomic>

#pragma comment(lib, "ws2_32.lib")

static std::string g_serverHost = "176.196.110.121";
static int g_serverPort = 3257;
static const size_t kMaxResponse = 8u * 1024 * 1024;

static GCResponseSink g_sink = nullptr;
static std::mutex g_sinkMutex;

void SetGCResponseSink(GCResponseSink sink)
{
    std::lock_guard<std::mutex> lock(g_sinkMutex);
    g_sink = sink;
}

// ============================================================================
// Разделение ответа сервера на отдельные GC-сообщения
// ============================================================================
bool SplitGCMessages(const std::vector<uint8_t>& data,
                     std::vector<std::vector<uint8_t>>& messages)
{
    size_t pos = 0;

    while (pos + 4 <= data.size()) {
        uint32_t totalLen = 0;
        memcpy(&totalLen, data.data() + pos, 4);

        if (totalLen == 0 || totalLen > 1024 * 1024) break;
        if (pos + 4 + totalLen > data.size()) break;

        std::vector<uint8_t> msg(
            data.begin() + pos + 4,
            data.begin() + pos + 4 + totalLen
        );
        messages.push_back(std::move(msg));

        pos += 4 + totalLen;
    }

    return !messages.empty();
}

void DeliverGCResponse(const ExternalGCResponse& response)
{
    GCResponseSink sink;
    {
        std::lock_guard<std::mutex> lock(g_sinkMutex);
        sink = g_sink;
    }
    if (!sink) return;

    std::vector<std::vector<uint8_t>> messages;
    if (SplitGCMessages(response.data, messages)) {
        for (auto& msg : messages) {
            if (msg.size() < 4) continue;
            uint32_t msgType = 0;
            memcpy(&msgType, msg.data(), 4);
            sink(msgType, std::move(msg));
        }
    } else if (!response.data.empty()) {
        sink(response.msgType, std::vector<uint8_t>(response.data));
    }
}

// ============================================================================
// Инициализация
// ============================================================================
static bool EnsureWsa()
{
    static std::once_flag once;
    static bool ok = false;
    std::call_once(once, [] {
        WSADATA wsaData;
        ok = (WSAStartup(MAKEWORD(2, 2), &wsaData) == 0);
    });
    return ok;
}

static void InitServerOnce()
{
    static std::once_flag once;
    std::call_once(once, [] {
        char host[256] = { 0 };
        int port = g_serverPort;
        if (CSGCConfig::ServerOverride(host, sizeof(host), port)) {
            g_serverHost = host;
            g_serverPort = port;
            GCLog("[FWD] server = %s:%d (from CSGC_SERVER)\n", g_serverHost.c_str(), g_serverPort);
        } else {
            GCLog("[FWD] server = %s:%d (default)\n", g_serverHost.c_str(), g_serverPort);
        }
    });
}

// ============================================================================
// Подключение с таймаутом
// ============================================================================
static bool ConnectToServer(SOCKET& out)
{
    InitServerOnce();
    if (!EnsureWsa()) {
        GCLog("[FWD] WSAStartup failed\n");
        return false;
    }

    addrinfo hints{};
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;

    char portStr[16];
    _snprintf_s(portStr, sizeof(portStr), _TRUNCATE, "%d", g_serverPort);

    addrinfo* res = nullptr;
    if (getaddrinfo(g_serverHost.c_str(), portStr, &hints, &res) != 0 || !res) {
        GCLog("[FWD] couldn't resolve '%s'\n", g_serverHost.c_str());
        return false;
    }

    SOCKET s = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
    if (s == INVALID_SOCKET) {
        freeaddrinfo(res);
        return false;
    }

    const int timeoutMs = CSGCConfig::NetTimeoutMs();

    u_long nonblocking = 1;
    ioctlsocket(s, FIONBIO, &nonblocking);

    int rc = connect(s, res->ai_addr, (int)res->ai_addrlen);
    if (rc == SOCKET_ERROR) {
        int err = WSAGetLastError();
        if (err != WSAEWOULDBLOCK && err != WSAEINPROGRESS && err != WSAEINVAL) {
            GCLog("[FWD] connect failed instantly (err=%d)\n", err);
            closesocket(s);
            freeaddrinfo(res);
            return false;
        }
        fd_set wf, ef;
        FD_ZERO(&wf); FD_SET(s, &wf);
        FD_ZERO(&ef); FD_SET(s, &ef);
        timeval tv;
        tv.tv_sec = timeoutMs / 1000;
        tv.tv_usec = (timeoutMs % 1000) * 1000;
        int sel = select(0, nullptr, &wf, &ef, &tv);
        if (sel == 0) {
            GCLog("[FWD] connect timeout %d ms — server unreachable\n", timeoutMs);
            closesocket(s);
            freeaddrinfo(res);
            return false;
        }
        if (sel == SOCKET_ERROR) {
            GCLog("[FWD] select failed (err=%d)\n", WSAGetLastError());
            closesocket(s);
            freeaddrinfo(res);
            return false;
        }
        int soerr = 0;
        int len = sizeof(soerr);
        getsockopt(s, SOL_SOCKET, SO_ERROR, (char*)&soerr, &len);
        if (soerr != 0) {
            GCLog("[FWD] connect error %d\n", soerr);
            closesocket(s);
            freeaddrinfo(res);
            return false;
        }
    }

    nonblocking = 0;
    ioctlsocket(s, FIONBIO, &nonblocking);

    DWORD t = (DWORD)timeoutMs;
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, (const char*)&t, sizeof(t));
    setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, (const char*)&t, sizeof(t));

    freeaddrinfo(res);
    out = s;
    return true;
}

static bool SendData(SOCKET sock, const void* data, size_t size)
{
    const uint8_t* bytes = static_cast<const uint8_t*>(data);
    size_t totalSent = 0;

    while (totalSent < size) {
        int sent = send(sock, (const char*)(bytes + totalSent), (int)(size - totalSent), 0);
        if (sent <= 0) return false;
        totalSent += sent;
    }
    return true;
}

static bool ReceiveData(SOCKET sock, std::vector<uint8_t>& buffer)
{
    char recvBuffer[4096];

    for (;;) {
        int received = recv(sock, recvBuffer, sizeof(recvBuffer), 0);
        if (received > 0) {
            buffer.insert(buffer.end(), recvBuffer, recvBuffer + received);
            if (buffer.size() > kMaxResponse) {
                GCLog("[FWD] answer bigger than %zu bytes — cutting off\n", kMaxResponse);
                break;
            }
            continue;
        }
        if (received == 0) break;
        int err = WSAGetLastError();
        if (err == WSAETIMEDOUT) {
            GCLog("[FWD] recv timeout\n");
            break;
        }
        if (err != WSAEWOULDBLOCK) break;
    }

    return !buffer.empty();
}

// ============================================================================
// Один round-trip (используется и синхронно, и из рабочего потока)
// ============================================================================
static bool DoRoundTrip(uint64_t steamId, uint32_t msgType,
                        const void* data, size_t size,
                        ExternalGCResponse& response)
{
    response = {};
    response.msgType = msgType;

    SOCKET sock;
    if (!ConnectToServer(sock)) return false;

    std::vector<uint8_t> packet;
    packet.reserve(12 + size);

    for (int i = 0; i < 8; i++)
        packet.push_back(static_cast<uint8_t>((steamId >> (i * 8)) & 0xFF));

    if (size > 0) {
        packet.insert(packet.end(),
                      static_cast<const uint8_t*>(data),
                      static_cast<const uint8_t*>(data) + size);
    }

    GCLog("[FWD] sending %zu bytes (steamId=%llu, type=0x%08X)\n",
          packet.size(), (unsigned long long)steamId, msgType);

    if (CSGCConfig::LogAllGC() && !packet.empty()) {
        char hex[256] = { 0 };
        int pos = 0;
        for (size_t i = 0; i < packet.size() && i < 32; i++)
            pos += sprintf(hex + pos, "%02X ", packet[i]);
        GCLog("[FWD] first bytes: %s\n", hex);
    }

    if (!SendData(sock, packet.data(), packet.size())) {
        GCLog("[FWD] send failed\n");
        closesocket(sock);
        return false;
    }

    shutdown(sock, SD_SEND);

    std::vector<uint8_t> allData;
    bool got = ReceiveData(sock, allData);
    closesocket(sock);

    if (!got) {
        GCLog("[FWD] server didn't answer\n");
        return false;
    }

    GCLog("[FWD] got %zu bytes from server\n", allData.size());
    response.data = std::move(allData);
    response.ok = true;
    return true;
}

// ============================================================================
// Асинхронный режим
// ============================================================================
namespace {
    struct OutMessage {
        uint64_t steamId;
        uint32_t type;
        std::vector<uint8_t> payload;
    };

    std::mutex g_queueMutex;
    std::deque<OutMessage> g_queue;
    HANDLE g_wakeEvent = nullptr;
    std::atomic<bool> g_workerStarted{ false };
}

static DWORD WINAPI WorkerThread(LPVOID)
{
    GCLog("[FWD] forwarder main thread started\n");
    for (;;) {
        WaitForSingleObject(g_wakeEvent, INFINITE);
        for (;;) {
            OutMessage msg;
            {
                std::lock_guard<std::mutex> lock(g_queueMutex);
                if (g_queue.empty()) break;
                msg = std::move(g_queue.front());
                g_queue.pop_front();
            }
            ExternalGCResponse response;
            if (DoRoundTrip(msg.steamId, msg.type, msg.payload.data(), msg.payload.size(), response))
                DeliverGCResponse(response);
        }
    }
    return 0;
}

void StartForwarderIfAsync()
{
    if (!CSGCConfig::Async()) return;
    if (g_workerStarted.exchange(true)) return;

    g_wakeEvent = CreateEventA(nullptr, FALSE, FALSE, nullptr);
    if (!g_wakeEvent) {
        GCLog("[FWD] CreateEvent failed, remaining synchronized\n");
        g_workerStarted = false;
        return;
    }
    CreateThread(nullptr, 0, WorkerThread, nullptr, 0, nullptr);
    GCLog("[FWD] CSGC_ASYNC=1 — net went in separated thread\n");
}

// ============================================================================
// Основная функция
// ============================================================================
bool ForwardToExternalServer(uint64_t steamId,
                             uint32_t msgType,
                             const void* data,
                             uint32_t size,
                             ExternalGCResponse& response)
{
    response = {};
    response.msgType = msgType;

    if (CSGCConfig::Async()) {
        if (!g_wakeEvent) return false;
        {
            std::lock_guard<std::mutex> lock(g_queueMutex);
            OutMessage msg;
            msg.steamId = steamId;
            msg.type = msgType;
            msg.payload.assign(static_cast<const uint8_t*>(data),
                               static_cast<const uint8_t*>(data) + size);
            g_queue.push_back(std::move(msg));
        }
        SetEvent(g_wakeEvent);
        GCLog("[FWD] put in order (async), type=0x%08X\n", msgType);
        return false;
    }

    return DoRoundTrip(steamId, msgType, data, size, response);
}
