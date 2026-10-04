#pragma once

#include <cstdint>
#include <vector>

struct ExternalGCResponse
{
    uint32_t msgType{};
    std::vector<uint8_t> data;
    bool ok{};
};

using GCResponseSink = void (*)(uint32_t msgType, std::vector<uint8_t>&& message);

void SetGCResponseSink(GCResponseSink sink);

void StartForwarderIfAsync();

bool ForwardToExternalServer(uint64_t steamId,
                             uint32_t msgType,
                             const void* data,
                             uint32_t size,
                             ExternalGCResponse& response);

bool SplitGCMessages(const std::vector<uint8_t>& data,
                     std::vector<std::vector<uint8_t>>& messages);

// Разбирает ответ и отдаёт каждое GC-сообщение в sink.
void DeliverGCResponse(const ExternalGCResponse& response);
