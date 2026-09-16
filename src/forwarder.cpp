#define WIN32_LEAN_AND_MEAN
#define _WINSOCKAPI_    // ← блокирует старый winsock.h

#include "forwarder.h"
#include "log.h"
#include <winsock2.h>
#include <ws2tcpip.h>
#include <cstdio>
#include <cstring>
#include <string>

#pragma comment(lib, "ws2_32.lib")

static std::string g_serverHost = "176.196.110.121";
static int g_serverPort = 3257;

// ============================================================================
// Разделение ответа сервера на отдельные GC-сообщения
// ============================================================================
bool SplitGCMessages(const std::vector<uint8_t>& data,
                     std::vector<std::vector<uint8_t>>& messages)
{
    size_t pos = 0;

    while (pos + 4 <= data.size()) {
        // totalLen (4 байта LE)
        uint32_t totalLen = 0;
        memcpy(&totalLen, data.data() + pos, 4);

        if (totalLen == 0 || totalLen > 1024 * 1024) break;
        if (pos + 4 + totalLen > data.size()) break;

        // Сообщение = [msgType 4][headerSize 4][header][payload] — БЕЗ totalLen
        std::vector<uint8_t> msg(
            data.begin() + pos + 4,
            data.begin() + pos + 4 + totalLen
        );
        messages.push_back(std::move(msg));

        pos += 4 + totalLen;
    }

    return !messages.empty();
}

// ============================================================================
// Подключение к серверу
// ============================================================================
static bool ConnectToServer(SOCKET& sock)
{
    sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock == INVALID_SOCKET) return false;

    struct sockaddr_in serverAddr{};
    serverAddr.sin_family = AF_INET;
    serverAddr.sin_port = htons(g_serverPort);

    if (inet_pton(AF_INET, g_serverHost.c_str(), &serverAddr.sin_addr) <= 0)
    {
        GCLog("[FORWARDER] Invalid address\n");
        closesocket(sock);
        return false;
    }

    if (connect(sock, (struct sockaddr*)&serverAddr, sizeof(serverAddr)) < 0)
    {
        GCLog("[FORWARDER] Connection failed (err=%d)\n", WSAGetLastError());
        closesocket(sock);
        return false;
    }

    return true;
}

static bool SendData(SOCKET sock, const void* data, size_t size)
{
    const uint8_t* bytes = static_cast<const uint8_t*>(data);
    size_t totalSent = 0;

    while (totalSent < size)
    {
        int sent = send(sock, (const char*)(bytes + totalSent), (int)(size - totalSent), 0);
        if (sent <= 0) return false;
        totalSent += sent;
    }
    return true;
}

static bool ReceiveData(SOCKET sock, std::vector<uint8_t>& buffer)
{
    char recvBuffer[4096];

    while (true)
    {
        int received = recv(sock, recvBuffer, sizeof(recvBuffer), 0);
        if (received <= 0) break;
        buffer.insert(buffer.end(), recvBuffer, recvBuffer + received);
    }

    return !buffer.empty();
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

    static bool wsaInitialized = false;
    if (!wsaInitialized) {
        WSADATA wsaData;
        WSAStartup(MAKEWORD(2, 2), &wsaData);
        wsaInitialized = true;
    }

    SOCKET sock;
    if (!ConnectToServer(sock))
    {
        return false;
    }

    // Формируем пакет: [steamId 8][msgType 4][payload]
    std::vector<uint8_t> packet;
    packet.reserve(12 + size);

    // steamId (8 байт LE)
    for (int i = 0; i < 8; i++) {
        packet.push_back(static_cast<uint8_t>((steamId >> (i * 8)) & 0xFF));
    }

    // payload
    packet.insert(packet.end(),
                  static_cast<const uint8_t*>(data),
                  static_cast<const uint8_t*>(data) + size);

    GCLog("[FORWARDER] Sending %zu bytes (steamId=%llu, type=0x%08X)\n",
          packet.size(), steamId, msgType);

    // Дамп первых 32 байт, чтобы видеть формат
    {
        char hex[256] = {0};
        int pos = 0;
        for (size_t i = 0; i < packet.size() && i < 32; i++) {
            pos += sprintf(hex + pos, "%02X ", packet[i]);
        }
        GCLog("[FORWARDER] First 32 bytes: %s\n", hex);
    }

    if (!SendData(sock, packet.data(), packet.size()))
    {
        GCLog("[FORWARDER] SendData failed\n");
        closesocket(sock);
        return false;
    }

    shutdown(sock, SD_SEND);

    std::vector<uint8_t> allData;
    if (!ReceiveData(sock, allData))
    {
        GCLog("[FORWARDER] No data from server\n");
        closesocket(sock);
        return false;
    }

    GCLog("[FORWARDER] Received %zu bytes from server\n", allData.size());

    response.data = std::move(allData);
    response.ok = true;

    closesocket(sock);
    return true;
}