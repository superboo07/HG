#include "runtime/gs/ps2_gif_arbiter.h"
#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <mutex>

namespace
{
    struct GifArbiterDump
    {
        std::mutex mutex;
        std::ofstream stream;
        uint64_t bytesWritten = 0u;
        uint64_t ordinal = 0u;
        uint64_t byteLimit = 64ull * 1024ull * 1024ull;
        std::chrono::steady_clock::time_point epoch = std::chrono::steady_clock::now();
        double delaySeconds = 0.0;
        double durationSeconds = 10.0;
        int pathFilter = -1;
        bool configured = false;
        bool finished = false;

        void record(GifPathId pathId, const uint8_t *data, uint32_t sizeBytes,
                    bool path2DirectHl)
        {
            std::lock_guard<std::mutex> lock(mutex);
            if (!configured)
            {
                configured = true;
                const char *path = std::getenv("PS2X_GIF_ARBITER_DUMP");
                if (!path || !*path)
                {
                    finished = true;
                    return;
                }
                if (const char *value = std::getenv("PS2X_GIF_ARBITER_DUMP_MAX_BYTES"))
                    byteLimit = std::strtoull(value, nullptr, 0);
                if (const char *value = std::getenv("PS2X_GIF_ARBITER_DUMP_DELAY_SECONDS"))
                    delaySeconds = std::max(0.0, std::strtod(value, nullptr));
                if (const char *value = std::getenv("PS2X_GIF_ARBITER_DUMP_DURATION_SECONDS"))
                    durationSeconds = std::max(0.0, std::strtod(value, nullptr));
                if (const char *value = std::getenv("PS2X_GIF_ARBITER_DUMP_PATH"))
                {
                    const long parsed = std::strtol(value, nullptr, 0);
                    if (parsed >= 1 && parsed <= 3)
                        pathFilter = static_cast<int>(parsed);
                }
                stream.open(path, std::ios::binary | std::ios::trunc);
                if (!stream)
                {
                    std::cerr << "[gif-arbiter-dump] unable to open " << path << std::endl;
                    finished = true;
                    return;
                }
                static constexpr char kHeader[8] = {'P', '2', 'G', 'I', 'F', '0', '0', '1'};
                stream.write(kHeader, sizeof(kHeader));
                bytesWritten = sizeof(kHeader);
                std::cerr << "[gif-arbiter-dump] path=" << path
                          << " delay_seconds=" << delaySeconds
                          << " duration_seconds=" << durationSeconds
                          << " byte_limit=" << byteLimit
                          << " path_filter=" << pathFilter << std::endl;
            }
            if (finished || !data || sizeBytes == 0u)
                return;
            if (pathFilter >= 0 && static_cast<int>(pathId) != pathFilter)
                return;

            const double elapsedSeconds = std::chrono::duration<double>(
                                              std::chrono::steady_clock::now() - epoch)
                                              .count();
            if (elapsedSeconds < delaySeconds)
                return;
            if (durationSeconds > 0.0 && elapsedSeconds >= delaySeconds + durationSeconds)
            {
                finished = true;
                stream.flush();
                return;
            }

            constexpr uint64_t kRecordHeaderBytes = 24u;
            if (byteLimit != 0u && bytesWritten + kRecordHeaderBytes + sizeBytes > byteLimit)
            {
                finished = true;
                stream.flush();
                std::cerr << "[gif-arbiter-dump] byte limit reached after " << ordinal
                          << " packets" << std::endl;
                return;
            }

            const uint8_t flags[4] = {
                static_cast<uint8_t>(pathId),
                static_cast<uint8_t>(path2DirectHl ? 1u : 0u),
                1u,
                0u};
            const uint64_t elapsedNanoseconds = static_cast<uint64_t>(elapsedSeconds * 1.0e9);
            stream.write(reinterpret_cast<const char *>(flags), sizeof(flags));
            stream.write(reinterpret_cast<const char *>(&sizeBytes), sizeof(sizeBytes));
            stream.write(reinterpret_cast<const char *>(&ordinal), sizeof(ordinal));
            stream.write(reinterpret_cast<const char *>(&elapsedNanoseconds), sizeof(elapsedNanoseconds));
            stream.write(reinterpret_cast<const char *>(data), sizeBytes);
            bytesWritten += kRecordHeaderBytes + sizeBytes;
            ++ordinal;
        }
    };

    GifArbiterDump &gifArbiterDump()
    {
        static GifArbiterDump dump;
        return dump;
    }
}

GifArbiter::GifArbiter(ProcessPacketFn processFn)
    : m_processFn(std::move(processFn))
{
}

bool GifArbiter::isImagePacket(const uint8_t *data, uint32_t sizeBytes)
{
    uint32_t offset = 0u;
    while (data && offset + 16u <= sizeBytes)
    {
        uint64_t tagLo = 0u;
        std::memcpy(&tagLo, data + offset, sizeof(tagLo));
        const uint32_t nloop = static_cast<uint32_t>(tagLo & 0x7FFFu);
        const uint32_t flg = static_cast<uint32_t>((tagLo >> 58u) & 0x3u);
        uint32_t nreg = static_cast<uint32_t>((tagLo >> 60u) & 0xFu);
        if (nreg == 0u)
            nreg = 16u;
        if (flg == 2u || flg == 3u)
            return true;
        const uint64_t payloadBytes = flg == 0u
                                          ? static_cast<uint64_t>(nloop) * nreg * 16u
                                          : (static_cast<uint64_t>(nloop) * nreg * 8u + 15u) & ~15ull;
        const uint64_t next = static_cast<uint64_t>(offset) + 16u + payloadBytes;
        if (next > sizeBytes)
            break;
        offset = static_cast<uint32_t>(next);
    }
    return false;
}

uint32_t GifArbiter::completePacketPrefixBytes(const uint8_t *data, uint32_t sizeBytes)
{
    uint32_t offset = 0u;
    while (data && offset + 16u <= sizeBytes)
    {
        uint64_t tagLo = 0u;
        std::memcpy(&tagLo, data + offset, sizeof(tagLo));
        const uint32_t nloop = static_cast<uint32_t>(tagLo & 0x7FFFu);
        const uint32_t flg = static_cast<uint32_t>((tagLo >> 58u) & 0x3u);
        uint32_t nreg = static_cast<uint32_t>((tagLo >> 60u) & 0xFu);
        if (nreg == 0u)
            nreg = 16u;

        uint64_t payloadBytes = 0u;
        if (flg == 0u)
            payloadBytes = static_cast<uint64_t>(nloop) * nreg * 16u;
        else if (flg == 1u)
            payloadBytes = (static_cast<uint64_t>(nloop) * nreg * 8u + 15u) & ~15ull;
        else
            payloadBytes = static_cast<uint64_t>(nloop) * 16u;

        const uint64_t next = static_cast<uint64_t>(offset) + 16u + payloadBytes;
        if (next > sizeBytes)
            break;
        offset = static_cast<uint32_t>(next);
    }
    return offset;
}

void GifArbiter::submit(GifPathId pathId, const uint8_t *data, uint32_t sizeBytes, bool path2DirectHl)
{
    if (!data || sizeBytes < 16 || !m_processFn)
        return;

    const size_t pathIndex = static_cast<size_t>(pathId) - 1u;
    if (pathIndex >= m_pathCarry.size())
        return;
    std::vector<uint8_t> &carry = m_pathCarry[pathIndex];
    const bool hadCarry = !carry.empty();
    const bool queuedDirectHl = hadCarry ? m_pathCarryDirectHl[pathIndex]
                                         : ((pathId == GifPathId::Path2) && path2DirectHl);
    if (!hadCarry)
        m_pathCarryDirectHl[pathIndex] = queuedDirectHl;
    carry.insert(carry.end(), data, data + sizeBytes);

    const uint32_t completeBytes = completePacketPrefixBytes(
        carry.data(), static_cast<uint32_t>(carry.size()));
    if (completeBytes == 0u)
        return;

    GifArbiterPacket pkt;
    pkt.pathId = pathId;
    pkt.path2DirectHl = queuedDirectHl;
    pkt.path3Image = (pathId == GifPathId::Path3) &&
                     isImagePacket(carry.data(), completeBytes);
    pkt.data.assign(carry.begin(), carry.begin() + completeBytes);
    gifArbiterDump().record(pathId, pkt.data.data(),
                            static_cast<uint32_t>(pkt.data.size()), queuedDirectHl);
    m_queue.push_back(std::move(pkt));

    carry.erase(carry.begin(), carry.begin() + completeBytes);
    if (carry.empty())
        m_pathCarryDirectHl[pathIndex] = false;
    else
        m_pathCarryDirectHl[pathIndex] =
            (pathId == GifPathId::Path2) && path2DirectHl;
}

void GifArbiter::drain()
{
    if (!m_processFn)
        return;

    std::stable_sort(m_queue.begin(), m_queue.end(),
                     [](const GifArbiterPacket &a, const GifArbiterPacket &b)
                     {
                         // DIRECTHL cannot preempt PATH3 IMAGE transfers.
                         if (a.path2DirectHl != b.path2DirectHl || a.path3Image != b.path3Image)
                         {
                             if (a.path3Image && b.path2DirectHl)
                                 return true;
                             if (a.path2DirectHl && b.path3Image)
                                 return false;
                         }
                         return pathPriority(a.pathId) < pathPriority(b.pathId);
                     });

    for (size_t i = 0; i < m_queue.size(); ++i)
    {
        auto &pkt = m_queue[i];
        if (!pkt.data.empty())
        {
            m_processFn(pkt.data.data(), static_cast<uint32_t>(pkt.data.size()));
        }
    }
    m_queue.clear();
}

uint8_t GifArbiter::pathPriority(GifPathId id)
{
    return static_cast<uint8_t>(id);
}
