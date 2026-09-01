// Based on Blackline Interactive implementation
#include "runtime/ps2_memory.h"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cfenv>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <unordered_map>
#include <vector>

enum VIFCmd : uint8_t
{
    VIF_NOP = 0x00,
    VIF_STCYCL = 0x01,
    VIF_OFFSET = 0x02,
    VIF_BASE = 0x03,
    VIF_ITOP = 0x04,
    VIF_STMOD = 0x05,
    VIF_MSKPATH3 = 0x06,
    VIF_MARK = 0x07,
    VIF_FLUSHE = 0x10,
    VIF_FLUSH = 0x11,
    VIF_FLUSHA = 0x13,
    VIF_MSCAL = 0x14,
    VIF_MSCALF = 0x15,
    VIF_MSCNT = 0x17,
    VIF_STMASK = 0x20,
    VIF_STROW = 0x30,
    VIF_STCOL = 0x31,
    VIF_MPG = 0x4A,
    VIF_DIRECT = 0x50,
    VIF_DIRECTHL = 0x51,
};

namespace
{
    class ScopedVifRoundingMode
    {
    public:
        ScopedVifRoundingMode()
            : m_previous(std::fegetround()),
              m_changed(m_previous != -1 && m_previous != FE_TOWARDZERO &&
                        std::fesetround(FE_TOWARDZERO) == 0)
        {
        }

        ~ScopedVifRoundingMode()
        {
            if (m_changed)
                std::fesetround(m_previous);
        }

    private:
        int m_previous;
        bool m_changed;
    };

    constexpr uint8_t kGifFmtImage = 2u;

    uint32_t gifPendingImageQwc(const uint8_t *data, uint32_t sizeBytes)
    {
        if (!data || sizeBytes < 16u)
            return 0u;

        uint32_t offset = 0u;
        while (offset + 16u <= sizeBytes)
        {
            uint64_t tagLo = 0u;
            std::memcpy(&tagLo, data + offset, sizeof(tagLo));
            const uint32_t nloop = static_cast<uint32_t>(tagLo & 0x7FFFu);
            const uint8_t flg = static_cast<uint8_t>((tagLo >> 58) & 0x3u);
            uint32_t nreg = static_cast<uint32_t>((tagLo >> 60) & 0xFu);
            if (nreg == 0u)
                nreg = 16u;

            uint64_t payloadQw = 0u;
            if (flg == 0u)
                payloadQw = static_cast<uint64_t>(nloop) * nreg;
            else if (flg == 1u)
                payloadQw = (static_cast<uint64_t>(nloop) * nreg + 1u) / 2u;
            else if (flg == kGifFmtImage)
                payloadQw = nloop;
            else
                return 0u;

            const uint32_t availableQw = (sizeBytes - offset - 16u) / 16u;
            if (flg == kGifFmtImage && payloadQw > availableQw)
                return static_cast<uint32_t>(payloadQw - availableQw);
            if (payloadQw > availableQw)
                return 0u;

            offset += 16u + static_cast<uint32_t>(payloadQw) * 16u;
        }
        return 0u;
    }
}

void PS2Memory::processVIF0Data(uint32_t srcPhys, uint32_t sizeBytes)
{
    if (sizeBytes == 0u || srcPhys >= PS2_RAM_SIZE)
        return;

    const uint64_t requestedEnd = static_cast<uint64_t>(srcPhys) + static_cast<uint64_t>(sizeBytes);
    if (requestedEnd > static_cast<uint64_t>(PS2_RAM_SIZE))
        sizeBytes = PS2_RAM_SIZE - srcPhys;

    processVIF0Data(m_rdram + srcPhys, sizeBytes);
}

void PS2Memory::processVIF0Data(const uint8_t *data, uint32_t sizeBytes)
{
    if (sizeBytes == 0u)
        return;

    uint32_t pos = 0;
    while (pos + 4 <= sizeBytes)
    {
        uint32_t cmd = 0u;
        std::memcpy(&cmd, data + pos, sizeof(cmd));
        pos += 4u;

        const uint8_t opcode = static_cast<uint8_t>((cmd >> 24) & 0x7Fu);
        const uint16_t imm = static_cast<uint16_t>(cmd & 0xFFFFu);
        const uint8_t num = static_cast<uint8_t>((cmd >> 16) & 0xFFu);
        const bool irq = (cmd & 0x80000000u) != 0u;

        vif0_regs.code = cmd;
        vif0_regs.num = num;
        if (irq)
            vif0_regs.stat |= (1u << 11);

        if (opcode == VIF_NOP)
        {
            continue;
        }
        else if (opcode == VIF_STCYCL)
        {
            vif0_regs.cycle = imm;
            continue;
        }
        else if (opcode == VIF_ITOP)
        {
            vif0_regs.itops = imm & 0x3FFu;
            continue;
        }
        else if (opcode == VIF_STMOD)
        {
            vif0_regs.mode = imm & 3u;
            continue;
        }
        else if (opcode == VIF_MARK)
        {
            vif0_regs.mark = imm;
            vif0_regs.stat |= (1u << 6);
            continue;
        }
        else if (opcode == VIF_FLUSHE || opcode == VIF_FLUSH || opcode == VIF_FLUSHA)
        {
            continue;
        }
        else if (opcode == VIF_STMASK)
        {
            if (pos + 4u > sizeBytes)
                break;
            std::memcpy(&vif0_regs.mask, data + pos, sizeof(vif0_regs.mask));
            pos += 4u;
            continue;
        }
        else if (opcode == VIF_STROW)
        {
            if (pos + 16u > sizeBytes)
                break;
            std::memcpy(vif0_regs.row, data + pos, 16u);
            pos += 16u;
            continue;
        }
        else if (opcode == VIF_STCOL)
        {
            if (pos + 16u > sizeBytes)
                break;
            std::memcpy(vif0_regs.col, data + pos, 16u);
            pos += 16u;
            continue;
        }
        else if (opcode == VIF_MPG)
        {
            const uint32_t destAddr = static_cast<uint32_t>(imm & 0x1FFu) * 8u;
            const uint32_t instructionCount = (num == 0u) ? 256u : static_cast<uint32_t>(num);
            const uint32_t mpgBytes = instructionCount * 8u;
            uint32_t copyBytes = 0u;
            if (m_vu0Code && destAddr < PS2_VU0_CODE_SIZE && mpgBytes > 0u)
            {
                copyBytes = mpgBytes;
                if (destAddr + copyBytes > PS2_VU0_CODE_SIZE)
                    copyBytes = PS2_VU0_CODE_SIZE - destAddr;
                if (pos + copyBytes <= sizeBytes)
                {
                    if (std::memcmp(m_vu0Code + destAddr, data + pos, copyBytes) != 0)
                    {
                        std::memcpy(m_vu0Code + destAddr, data + pos, copyBytes);
                        markVU0CodeModified();
                    }
                }
            }

            pos += mpgBytes;
            if (pos > sizeBytes)
                break;
            continue;
        }
        else if ((opcode & 0x60u) == 0x60u)
        {
            const uint8_t vn = static_cast<uint8_t>((opcode >> 2) & 0x3u);
            const uint8_t vl = static_cast<uint8_t>(opcode & 0x3u);
            const int components = static_cast<int>(vn) + 1;
            int bitsPerComponent = 32;
            switch (vl)
            {
            case 0:
                bitsPerComponent = 32;
                break;
            case 1:
                bitsPerComponent = 16;
                break;
            case 2:
                bitsPerComponent = 8;
                break;
            case 3:
                bitsPerComponent = (vn == 3u) ? 4 : 16;
                break;
            default:
                break;
            }
            const int bitsPerVector = (vl == 3u && vn == 3u) ? 16 : (components * bitsPerComponent);
            uint32_t bytesPerVector = static_cast<uint32_t>((bitsPerVector + 7) / 8);
            const uint32_t writeVectorCount = (num == 0u) ? 256u : static_cast<uint32_t>(num);
            uint32_t cl = vif0_regs.cycle & 0xFFu;
            uint32_t wl = (vif0_regs.cycle >> 8) & 0xFFu;
            if (cl == 0u)
                cl = 1u;
            if (wl == 0u)
                wl = 1u;
            uint32_t sourceVectorCount = writeVectorCount;
            if (cl < wl)
            {
                const uint32_t fullBlocks = writeVectorCount / wl;
                uint32_t remainder = writeVectorCount % wl;
                if (remainder > cl)
                    remainder = cl;
                sourceVectorCount = fullBlocks * cl + remainder;
            }
            uint32_t totalBytes = sourceVectorCount * bytesPerVector;
            totalBytes = (totalBytes + 3u) & ~3u;

            if (m_vu0Data && pos + totalBytes <= sizeBytes && vl == 0u)
            {
                uint32_t vuAddr = static_cast<uint32_t>(imm & 0x3FFu);
                if ((imm & 0x8000u) != 0u)
                    vuAddr = (vuAddr + (vif0_regs.tops & 0x3FFu)) & 0x3FFu;
                const uint8_t *srcBase = data + pos;
                uint32_t srcIndex = 0u;
                for (uint32_t writeIndex = 0; writeIndex < writeVectorCount; ++writeIndex)
                {
                    const uint32_t cyclePos = writeIndex % wl;
                    const bool sourceAvailable = (cl >= wl) || (cyclePos < cl);
                    uint32_t destVec = (cl >= wl) ? ((vuAddr + (writeIndex / wl) * cl + cyclePos) & 0x3FFu)
                                                  : ((vuAddr + writeIndex) & 0x3FFu);
                    const uint32_t destOff = destVec * 16u;
                    if (destOff + 16u > PS2_VU0_DATA_SIZE)
                    {
                        if (sourceAvailable && srcIndex < sourceVectorCount)
                            ++srcIndex;
                        continue;
                    }
                    if (!sourceAvailable || srcIndex >= sourceVectorCount)
                        continue;
                    const uint8_t *srcVec = srcBase + srcIndex * bytesPerVector;
                    ++srcIndex;
                    uint32_t lanes[4] = {0u, 0u, 0u, 0u};
                    std::memcpy(lanes, m_vu0Data + destOff, sizeof(lanes));
                    const uint32_t limit = (components > 4) ? 4u : static_cast<uint32_t>(components);
                    for (uint32_t c = 0; c < limit; ++c)
                    {
                        uint32_t scalar = 0u;
                        std::memcpy(&scalar, srcVec + c * 4u, sizeof(scalar));
                        lanes[c] = scalar;
                    }
                    _mm_storeu_si128(reinterpret_cast<__m128i *>(m_vu0Data + destOff), _mm_loadu_si128(reinterpret_cast<const __m128i *>(lanes)));
                }
            }
            pos += totalBytes;
            if (pos > sizeBytes)
                break;
            continue;
        }
        else
        {
            break;
        }
    }
}

void PS2Memory::processVIF1Data(uint32_t srcPhys, uint32_t sizeBytes)
{
    if (sizeBytes == 0u || srcPhys >= PS2_RAM_SIZE)
        return;

    const uint64_t requestedEnd = static_cast<uint64_t>(srcPhys) + static_cast<uint64_t>(sizeBytes);
    if (requestedEnd > static_cast<uint64_t>(PS2_RAM_SIZE))
        sizeBytes = PS2_RAM_SIZE - srcPhys;

    processVIF1Data(m_rdram + srcPhys, sizeBytes);
}

void PS2Memory::processVIF1Data(const uint8_t *data, uint32_t sizeBytes)
{
    if (sizeBytes == 0u)
        return;

    // A chain may issue tens of thousands of VU continuations. VIF parsing is
    // integer-only, so retain the VU's architectural toward-zero mode across
    // the whole chain instead of changing the host control word twice per
    // MSCAL/MSCNT call.
    ScopedVifRoundingMode vifRoundingMode;

    using VifProfileClock = std::chrono::steady_clock;
    struct VifAggregateProfile
    {
        struct VuStartStat
        {
            uint64_t calls{0u};
            uint64_t nanoseconds{0u};
        };
        bool enabled{false};
        bool printed{false};
        VifProfileClock::time_point start{};
        VifProfileClock::time_point deadline{};
        uint64_t processCalls{0u};
        uint64_t processBytes{0u};
        uint64_t processNanoseconds{0u};
        uint64_t directCalls{0u};
        uint64_t directQwords{0u};
        uint64_t directNanoseconds{0u};
        uint64_t vuCalls{0u};
        uint64_t vuNanoseconds{0u};
        uint64_t unpackCalls{0u};
        uint64_t unpackVectors{0u};
        uint64_t unpackNanoseconds{0u};
        std::unordered_map<uint32_t, VuStartStat> vuStarts;
    };
    static VifAggregateProfile vifProfile = [] {
        VifAggregateProfile result;
        const char *durationText = std::getenv("PS2X_VIF1_PROFILE_SECONDS");
        if (!durationText || durationText[0] == '\0')
            return result;
        char *durationEnd = nullptr;
        const double duration = std::strtod(durationText, &durationEnd);
        if (durationEnd == durationText || *durationEnd != '\0' || duration <= 0.0)
            return result;
        double delay = 0.0;
        if (const char *delayText = std::getenv("PS2X_VIF1_PROFILE_DELAY_SECONDS"))
        {
            char *delayEnd = nullptr;
            const double parsed = std::strtod(delayText, &delayEnd);
            if (delayEnd != delayText && *delayEnd == '\0' && parsed > 0.0)
                delay = parsed;
        }
        result.enabled = true;
        result.start = VifProfileClock::now() +
                       std::chrono::duration_cast<VifProfileClock::duration>(
                           std::chrono::duration<double>(delay));
        result.deadline = result.start +
                          std::chrono::duration_cast<VifProfileClock::duration>(
                              std::chrono::duration<double>(duration));
        return result;
    }();
    const auto vifProfileNow = VifProfileClock::now();
    const bool profileVif = vifProfile.enabled &&
                            vifProfileNow >= vifProfile.start &&
                            vifProfileNow < vifProfile.deadline;
    const auto vifProcessStart = profileVif ? vifProfileNow : VifProfileClock::time_point{};

    auto submitDirectPayload = [&](const uint8_t *payload, uint32_t qwCount, bool directHl)
    {
        const auto directStart = profileVif ? VifProfileClock::now() : VifProfileClock::time_point{};
        // The GIF arbiter retains incomplete packets per path, so feed it the
        // exact PATH2 byte stream.  The synthetic IMAGE-tag continuation below
        // is only needed by the legacy callback path, whose GS parser has no
        // cross-submission state.
        if (m_gifArbiter)
        {
            submitGifPacket(GifPathId::Path2, payload, qwCount * 16u, true, directHl);
            if (profileVif)
            {
                ++vifProfile.directCalls;
                vifProfile.directQwords += qwCount;
                vifProfile.directNanoseconds += static_cast<uint64_t>(
                    std::chrono::duration_cast<std::chrono::nanoseconds>(
                        VifProfileClock::now() - directStart).count());
            }
            return;
        }

        uint32_t consumedQw = 0u;
        if (m_vif1PendingPath2ImageQwc != 0u)
        {
            const uint32_t imageQw = std::min<uint32_t>(m_vif1PendingPath2ImageQwc, qwCount);
            std::vector<uint8_t> imagePacket(16u + static_cast<size_t>(imageQw) * 16u, 0u);
            const uint64_t imageTag =
                static_cast<uint64_t>(imageQw & 0x7FFFu) |
                ((m_vif1PendingPath2ImageQwc == imageQw) ? (1ull << 15) : 0ull) |
                (static_cast<uint64_t>(kGifFmtImage) << 58);
            std::memcpy(imagePacket.data(), &imageTag, sizeof(imageTag));
            std::memcpy(imagePacket.data() + 16u, payload, static_cast<size_t>(imageQw) * 16u);
            submitGifPacket(GifPathId::Path2,
                            imagePacket.data(),
                            static_cast<uint32_t>(imagePacket.size()),
                            true,
                            directHl);

            consumedQw = imageQw;
            m_vif1PendingPath2ImageQwc -= imageQw;
            if (m_vif1PendingPath2ImageQwc == 0u)
                m_vif1PendingPath2DirectHl = false;
        }

        if (consumedQw < qwCount)
        {
            const uint8_t *remaining = payload + static_cast<size_t>(consumedQw) * 16u;
            const uint32_t remainingQw = qwCount - consumedQw;
            submitGifPacket(GifPathId::Path2, remaining, remainingQw * 16u, true, directHl);

            const uint32_t pendingImageQw = gifPendingImageQwc(remaining, remainingQw * 16u);
            if (pendingImageQw != 0u)
            {
                m_vif1PendingPath2ImageQwc = pendingImageQw;
                m_vif1PendingPath2DirectHl = directHl;
            }
        }
        if (profileVif)
        {
            ++vifProfile.directCalls;
            vifProfile.directQwords += qwCount;
            vifProfile.directNanoseconds += static_cast<uint64_t>(
                std::chrono::duration_cast<std::chrono::nanoseconds>(
                    VifProfileClock::now() - directStart).count());
        }
    };

    uint32_t pos = 0;
    while (pos + 4 <= sizeBytes)
    {
        if (m_vif1PendingDirectQwc != 0u)
        {
            const uint32_t availableQw = (sizeBytes - pos) / 16u;
            if (availableQw == 0u)
                break;

            const uint32_t chunkQw = std::min<uint32_t>(m_vif1PendingDirectQwc, availableQw);
            submitDirectPayload(data + pos, chunkQw, m_vif1PendingDirectHl);
            pos += chunkQw * 16u;
            m_vif1PendingDirectQwc -= chunkQw;
            if (m_vif1PendingDirectQwc == 0u)
            {
                m_vif1PendingDirectHl = false;
            }
            continue;
        }

        uint32_t cmd;
        memcpy(&cmd, data + pos, 4);
        pos += 4;

        uint8_t opcode = (cmd >> 24) & 0x7F;
        uint16_t imm = cmd & 0xFFFF;
        uint8_t num = (cmd >> 16) & 0xFF;
        const bool irq = (cmd & 0x80000000u) != 0u;
        // Track most-recent command for VIFn_CODE emulation.
        vif1_regs.code = cmd;
        vif1_regs.num = num;
        if (irq)
            vif1_regs.stat |= (1u << 11); // INT

        if (opcode == VIF_NOP)
        {
            continue;
        }
        else if (opcode == VIF_STCYCL)
        {
            vif1_regs.cycle = imm;
            continue;
        }
        else if (opcode == VIF_OFFSET)
        {
            // VIF double-buffer setup. OFFSET clears DBF and resets TOPS to BASE.
            // Do not rewrite BASE from the previous TOPS value.
            vif1_regs.ofst = imm & 0x3FFu;
            vif1_regs.tops = vif1_regs.base & 0x3FFu;
            vif1_regs.stat &= ~(1u << 7); // clear DBF
            continue;
        }
        else if (opcode == VIF_BASE)
        {
            // BASE only updates the base register. TOPS changes on OFFSET/MSCAL.
            vif1_regs.base = imm & 0x3FFu;
            continue;
        }
        else if (opcode == VIF_ITOP)
        {
            // ITOP VIFcode writes pending ITOPS; VU XITOP observes it after MSCAL/MSCNT.
            vif1_regs.itops = imm & 0x3FFu;
            continue;
        }
        else if (opcode == VIF_STMOD)
        {
            vif1_regs.mode = imm & 3u;
            continue;
        }
        else if (opcode == VIF_MSKPATH3)
        {
            // VIF command docs: MSKPATH3 uses IMMEDIATE bit 15.
            const bool wasMasked = m_path3Masked;
            m_path3Masked = (imm & 0x8000u) != 0u;
            if (wasMasked && !m_path3Masked)
                flushMaskedPath3Packets();
            continue;
        }
        else if (opcode == VIF_MARK)
        {
            vif1_regs.mark = imm;
            vif1_regs.stat |= (1u << 6); // MRK
            continue;
        }
        else if (opcode == VIF_FLUSHE || opcode == VIF_FLUSH || opcode == VIF_FLUSHA)
        {
            continue;
        }
        else if (opcode == VIF_MSCAL || opcode == VIF_MSCALF)
        {
            const auto vuStart = profileVif ? VifProfileClock::now() : VifProfileClock::time_point{};
            uint32_t startPC = (uint32_t)imm * 8u;

            // Values visible to the VU program for this MSCAL.
            // DobieStation semantics: ITOP = ITOPS; TOP = current TOPS;
            // then TOPS/DBF are prepared for the next buffer.
            const uint32_t runTop = vif1_regs.tops & 0x3FFu;
            const uint32_t runItop = vif1_regs.itops & 0x3FFu;
            vif1_regs.top = runTop;
            vif1_regs.itop = runItop;

            const bool dbf = (vif1_regs.stat & (1u << 7)) != 0u;
            if (dbf)
                vif1_regs.tops = vif1_regs.base & 0x3FFu;
            else
                vif1_regs.tops = (vif1_regs.base + vif1_regs.ofst) & 0x3FFu;
            vif1_regs.stat ^= (1u << 7); // toggle DBF

            if (m_vu1MscalCallback)
                m_vu1MscalCallback(startPC, runTop, runItop);
            if (profileVif)
            {
                const uint64_t elapsed = static_cast<uint64_t>(
                    std::chrono::duration_cast<std::chrono::nanoseconds>(
                        VifProfileClock::now() - vuStart).count());
                ++vifProfile.vuCalls;
                vifProfile.vuNanoseconds += elapsed;
                auto &stat = vifProfile.vuStarts[startPC];
                ++stat.calls;
                stat.nanoseconds += elapsed;
            }
            continue;
        }
        else if (opcode == VIF_MSCNT)
        {
            const auto vuStart = profileVif ? VifProfileClock::now() : VifProfileClock::time_point{};
            const uint32_t runTop = vif1_regs.tops & 0x3FFu;
            const uint32_t runItop = vif1_regs.itops & 0x3FFu;
            vif1_regs.top = runTop;
            vif1_regs.itop = runItop;

            const bool dbf = (vif1_regs.stat & (1u << 7)) != 0u;
            if (dbf)
                vif1_regs.tops = vif1_regs.base & 0x3FFu;
            else
                vif1_regs.tops = (vif1_regs.base + vif1_regs.ofst) & 0x3FFu;
            vif1_regs.stat ^= (1u << 7); // toggle DBF

            if (m_vu1MscntCallback)
                m_vu1MscntCallback(runTop, runItop);
            if (profileVif)
            {
                const uint64_t elapsed = static_cast<uint64_t>(
                    std::chrono::duration_cast<std::chrono::nanoseconds>(
                        VifProfileClock::now() - vuStart).count());
                ++vifProfile.vuCalls;
                vifProfile.vuNanoseconds += elapsed;
                auto &stat = vifProfile.vuStarts[0xFFFFFFFFu];
                ++stat.calls;
                stat.nanoseconds += elapsed;
            }
            continue;
        }
        else if (opcode == VIF_STMASK)
        {
            if (pos + 4 > sizeBytes)
                break;
            uint32_t maskValue = 0;
            std::memcpy(&maskValue, data + pos, sizeof(maskValue));
            vif1_regs.mask = maskValue;
            pos += 4;
            continue;
        }
        else if (opcode == VIF_STROW)
        {
            if (pos + 16 > sizeBytes)
                break;
            std::memcpy(vif1_regs.row, data + pos, 16);
            pos += 16;
            continue;
        }
        else if (opcode == VIF_STCOL)
        {
            if (pos + 16 > sizeBytes)
                break;
            std::memcpy(vif1_regs.col, data + pos, 16);
            pos += 16;
            continue;
        }
        else if (opcode == VIF_MPG)
        {
            uint32_t destAddr = (uint32_t)imm * 8u;
            // VIF MPG semantics: NUM==0 means 256 instructions (2048 bytes).
            // MPG payload is instruction-packed and should not be QW-aligned.
            const uint32_t instructionCount = (num == 0u) ? 256u : static_cast<uint32_t>(num);
            const uint32_t mpgBytes = instructionCount * 8u;
            if (m_vu1Code && destAddr < PS2_VU1_CODE_SIZE && mpgBytes > 0)
            {
                uint32_t copyBytes = mpgBytes;
                if (destAddr + copyBytes > PS2_VU1_CODE_SIZE)
                    copyBytes = PS2_VU1_CODE_SIZE - destAddr;
                if (pos + copyBytes <= sizeBytes)
                {
                    if (std::memcmp(m_vu1Code + destAddr, data + pos, copyBytes) != 0)
                    {
                        std::memcpy(m_vu1Code + destAddr, data + pos, copyBytes);
                        markVU1CodeModified();
                    }
                }
            }
            pos += mpgBytes;
            if (pos > sizeBytes)
                break;
            continue;
        }
        else if (opcode == VIF_DIRECT || opcode == VIF_DIRECTHL)
        {
            uint32_t requestedQw = imm;
            if (requestedQw == 0u)
                requestedQw = 65536u;
            const uint32_t availableQw = (sizeBytes - pos) / 16u;
            const uint32_t qwCount = std::min<uint32_t>(requestedQw, availableQw);
            const bool truncated = requestedQw > availableQw;
            const bool directHl = (opcode == VIF_DIRECTHL);

            if (qwCount > 0)
                submitDirectPayload(data + pos, qwCount, directHl);

            pos += qwCount * 16;
            if (truncated)
            {
                m_vif1PendingDirectQwc = requestedQw - qwCount;
                m_vif1PendingDirectHl = directHl;
                pos = sizeBytes;
                break;
            }
            continue;
        }
        else if ((opcode & 0x60) == 0x60)
        {
            const auto unpackStart = profileVif ? VifProfileClock::now() : VifProfileClock::time_point{};
            uint8_t vn = (opcode >> 2) & 0x3;
            uint8_t vl = opcode & 0x3;
            const bool maskEnable = (opcode & 0x10u) != 0u;
            int components = vn + 1;
            int bitsPerComponent = 32;
            switch (vl)
            {
            case 0:
                bitsPerComponent = 32;
                break;
            case 1:
                bitsPerComponent = 16;
                break;
            case 2:
                bitsPerComponent = 8;
                break;
            case 3:
                bitsPerComponent = (vn == 3) ? 4 : 16;
                break;
            default:
                break;
            }
            int bitsPerVector = (vl == 3 && vn == 3) ? 16 : (components * bitsPerComponent);
            uint32_t bytesPerVector = (bitsPerVector + 7) / 8;
            // UNPACK semantics: NUM is 8-bit and NUM==0 means 256 vectors (writes).
            const uint32_t writeVectorCount = (num == 0u) ? 256u : static_cast<uint32_t>(num);

            // STCYCL controls write cycles for UNPACK.
            uint32_t cl = vif1_regs.cycle & 0xFFu;
            uint32_t wl = (vif1_regs.cycle >> 8) & 0xFFu;

            if (std::getenv("PS2X_VIF1_UNPACK_TRACE") != nullptr)
            {
                static const auto traceStart = std::chrono::steady_clock::now();
                static std::atomic<uint32_t> traceCount{0u};
                double delaySeconds = 0.0;
                if (const char *delay = std::getenv("PS2X_VIF1_UNPACK_TRACE_DELAY_SECONDS"))
                    delaySeconds = std::strtod(delay, nullptr);
                const double elapsedSeconds = std::chrono::duration<double>(
                    std::chrono::steady_clock::now() - traceStart).count();
                if (elapsedSeconds >= delaySeconds)
                {
                    const uint32_t index = traceCount.fetch_add(1u, std::memory_order_relaxed);
                    if (index < 256u)
                    {
                        std::cerr << "[vif1-unpack] index=" << index
                                  << " opcode=0x" << std::hex << static_cast<uint32_t>(opcode)
                                  << std::dec << " vn=" << static_cast<uint32_t>(vn)
                                  << " vl=" << static_cast<uint32_t>(vl)
                                  << " components=" << components
                                  << " bits=" << bitsPerComponent
                                  << " num=" << writeVectorCount
                                  << " cl=" << cl
                                  << " wl=" << wl
                                  << " mask=" << (maskEnable ? 1 : 0)
                                  << " mode=" << (vif1_regs.mode & 3u)
                                  << " addr=0x" << std::hex << (imm & 0x3FFu)
                                  << std::dec << std::endl;
                    }
                }
            }
            if (cl == 0u)
                cl = 1u;
            if (wl == 0u)
                wl = 1u;

            uint32_t sourceVectorCount = writeVectorCount;
            if (cl < wl)
            {
                const uint32_t fullBlocks = writeVectorCount / wl;
                uint32_t remainder = writeVectorCount % wl;
                if (remainder > cl)
                    remainder = cl;
                sourceVectorCount = fullBlocks * cl + remainder;
            }

            uint32_t totalBytes = sourceVectorCount * bytesPerVector;
            totalBytes = (totalBytes + 3) & ~3u;

            uint32_t vuAddr = (uint32_t)imm & 0x3FFu;
            if ((imm & 0x8000u) != 0u)
                vuAddr = (vuAddr + (vif1_regs.tops & 0x3FFu)) & 0x3FFu;

            const bool zeroExtend = (imm & 0x4000u) != 0u;

            if (m_vu1Data && totalBytes > 0 && pos + totalBytes <= sizeBytes)
            {
                const uint8_t *srcBase = data + pos;
                uint32_t srcIndex = 0u;
                for (uint32_t writeIndex = 0; writeIndex < writeVectorCount; ++writeIndex)
                {
                    const uint32_t cyclePos = writeIndex % wl;
                    const bool sourceAvailable = (cl >= wl) || (cyclePos < cl);

                    uint32_t destVec = 0;
                    if (cl >= wl)
                    {
                        destVec = (vuAddr + (writeIndex / wl) * cl + cyclePos) & 0x3FFu;
                    }
                    else
                    {
                        destVec = (vuAddr + writeIndex) & 0x3FFu;
                    }

                    uint32_t destOff = destVec * 16u;
                    if (destOff + 16u > PS2_VU1_DATA_SIZE)
                    {
                        if (sourceAvailable && srcIndex < sourceVectorCount)
                            ++srcIndex;
                        continue;
                    }

                    uint32_t lanes[4] = {0u, 0u, 0u, 0u};
                    std::memcpy(lanes, m_vu1Data + destOff, sizeof(lanes));
                    uint32_t decompressed[4] = {lanes[0], lanes[1], lanes[2], lanes[3]};
                    bool decoded = false;

                    const uint8_t *srcVec = nullptr;
                    if (sourceAvailable && srcIndex < sourceVectorCount)
                    {
                        srcVec = srcBase + srcIndex * bytesPerVector;
                        ++srcIndex;
                        decoded = true;
                    }

                    auto extend16 = [&](uint16_t raw) -> uint32_t
                    {
                        if (zeroExtend)
                            return static_cast<uint32_t>(raw);
                        return static_cast<uint32_t>(static_cast<int32_t>(static_cast<int16_t>(raw)));
                    };

                    auto extend8 = [&](uint8_t raw) -> uint32_t
                    {
                        if (zeroExtend)
                            return static_cast<uint32_t>(raw);
                        return static_cast<uint32_t>(static_cast<int32_t>(static_cast<int8_t>(raw)));
                    };

                    bool handledFormat = true;
                    if (!decoded)
                    {
                        handledFormat = false;
                    }
                    else if (vl == 0u)
                    {
                        if (components == 1)
                        {
                            uint32_t scalar = 0;
                            std::memcpy(&scalar, srcVec, sizeof(scalar));
                            decompressed[0] = scalar;
                            decompressed[1] = scalar;
                            decompressed[2] = scalar;
                            decompressed[3] = scalar;
                        }
                        else
                        {
                            const uint32_t limit = (components > 4) ? 4u : static_cast<uint32_t>(components);
                            for (uint32_t c = 0; c < limit; ++c)
                            {
                                uint32_t scalar = 0;
                                std::memcpy(&scalar, srcVec + c * 4u, sizeof(scalar));
                                decompressed[c] = scalar;
                            }
                        }
                    }
                    else if (vl == 1u)
                    {
                        if (components == 1)
                        {
                            uint16_t raw = 0;
                            std::memcpy(&raw, srcVec, sizeof(raw));
                            const uint32_t scalar = extend16(raw);
                            decompressed[0] = scalar;
                            decompressed[1] = scalar;
                            decompressed[2] = scalar;
                            decompressed[3] = scalar;
                        }
                        else
                        {
                            const uint32_t limit = (components > 4) ? 4u : static_cast<uint32_t>(components);
                            for (uint32_t c = 0; c < limit; ++c)
                            {
                                uint16_t raw = 0;
                                std::memcpy(&raw, srcVec + c * 2u, sizeof(raw));
                                decompressed[c] = extend16(raw);
                            }
                        }
                    }
                    else if (vl == 2u)
                    {
                        if (components == 1)
                        {
                            const uint32_t scalar = extend8(srcVec[0]);
                            decompressed[0] = scalar;
                            decompressed[1] = scalar;
                            decompressed[2] = scalar;
                            decompressed[3] = scalar;
                        }
                        else
                        {
                            const uint32_t limit = (components > 4) ? 4u : static_cast<uint32_t>(components);
                            for (uint32_t c = 0; c < limit; ++c)
                            {
                                decompressed[c] = extend8(srcVec[c]);
                            }
                        }
                    }
                    else if (vl == 3u && vn == 3u)
                    {
                        // V4-5: packed color-like format in a single 16-bit value.
                        uint16_t packed = 0;
                        std::memcpy(&packed, srcVec, sizeof(packed));
                        decompressed[0] = packed & 0x1Fu;
                        decompressed[1] = (packed >> 5) & 0x1Fu;
                        decompressed[2] = (packed >> 10) & 0x1Fu;
                        decompressed[3] = (packed >> 15) & 0x01u;
                    }
                    else
                    {
                        handledFormat = false;
                    }

                    // Real VIF hardware writes V2 as X,Y,X,Y. The second pair
                    // is documented as indeterminate, but games depend on this
                    // exact replication and PCSX2's interpreter models it.
                    if (handledFormat && components == 2)
                    {
                        decompressed[2] = decompressed[0];
                        decompressed[3] = decompressed[1];
                    }

                    // Unknown compressed format fallback: preserve legacy raw-copy behavior.
                    if (!handledFormat && decoded && !maskEnable && (vif1_regs.mode == 0u || vif1_regs.mode == 3u))
                    {
                        uint32_t copyBytes = (bytesPerVector < 16u) ? bytesPerVector : 16u;
                        std::memcpy(m_vu1Data + destOff, srcVec, copyBytes);
                        continue;
                    }

                    const bool canAdd = (vl != 3u || vn != 3u);
                    const uint32_t mode = vif1_regs.mode & 3u;
                    const uint32_t colIdx = (cyclePos > 3u) ? 3u : cyclePos;
                    const uint32_t maskCycle = (cyclePos > 3u) ? 3u : cyclePos;

                    for (uint32_t field = 0u; field < 4u; ++field)
                    {
                        uint32_t maskSpec = 0u;
                        if (maskEnable)
                        {
                            const uint32_t shift = ((maskCycle * 4u) + field) * 2u;
                            maskSpec = (vif1_regs.mask >> shift) & 0x3u;
                        }

                        // In fill-write cycles with suspended source reads, treat raw-data selections as row-fill.
                        if (!decoded && maskSpec == 0u)
                            maskSpec = 1u;

                        uint32_t writeVal = lanes[field];
                        if (maskSpec == 0u)
                        {
                            if (handledFormat)
                            {
                                writeVal = decompressed[field];
                                if (canAdd && (mode == 1u || mode == 2u))
                                {
                                    writeVal = writeVal + vif1_regs.row[field];
                                    if (mode == 2u)
                                        vif1_regs.row[field] = writeVal;
                                }
                            }
                        }
                        else if (maskSpec == 1u)
                        {
                            writeVal = vif1_regs.row[field];
                        }
                        else if (maskSpec == 2u)
                        {
                            writeVal = vif1_regs.col[colIdx];
                        }
                        else
                        {
                            continue; // write-protect
                        }

                        lanes[field] = writeVal;
                    }

                    std::memcpy(m_vu1Data + destOff, lanes, sizeof(lanes));
                }
            }
            pos += totalBytes;

            if (pos > sizeBytes)
                break;
            if (profileVif)
            {
                ++vifProfile.unpackCalls;
                vifProfile.unpackVectors += writeVectorCount;
                vifProfile.unpackNanoseconds += static_cast<uint64_t>(
                    std::chrono::duration_cast<std::chrono::nanoseconds>(
                        VifProfileClock::now() - unpackStart).count());
            }
            continue;
        }
        else
        {
            continue;
        }
    }

    const auto vifProfileEnd = VifProfileClock::now();
    if (profileVif)
    {
        ++vifProfile.processCalls;
        vifProfile.processBytes += sizeBytes;
        vifProfile.processNanoseconds += static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(
                vifProfileEnd - vifProcessStart).count());
    }
    if (vifProfile.enabled && !vifProfile.printed && vifProfileEnd >= vifProfile.deadline)
    {
        vifProfile.printed = true;
        const double totalMs = static_cast<double>(vifProfile.processNanoseconds) / 1.0e6;
        const double directMs = static_cast<double>(vifProfile.directNanoseconds) / 1.0e6;
        const double vuMs = static_cast<double>(vifProfile.vuNanoseconds) / 1.0e6;
        const double unpackMs = static_cast<double>(vifProfile.unpackNanoseconds) / 1.0e6;
        std::cerr << "[vif1-profile] calls=" << vifProfile.processCalls
                  << " bytes=" << vifProfile.processBytes
                  << " total_ms=" << totalMs
                  << " direct_calls=" << vifProfile.directCalls
                  << " direct_qw=" << vifProfile.directQwords
                  << " direct_ms=" << directMs
                  << " vu_calls=" << vifProfile.vuCalls
                  << " vu_ms=" << vuMs
                  << " unpack_calls=" << vifProfile.unpackCalls
                  << " unpack_vectors=" << vifProfile.unpackVectors
                  << " unpack_ms=" << unpackMs
                  << " other_ms=" << (totalMs - directMs - vuMs - unpackMs)
                  << std::endl;
        std::vector<std::pair<uint32_t, VifAggregateProfile::VuStartStat>> starts(
            vifProfile.vuStarts.begin(), vifProfile.vuStarts.end());
        std::sort(starts.begin(), starts.end(), [](const auto &left, const auto &right) {
            return left.second.nanoseconds > right.second.nanoseconds;
        });
        for (size_t index = 0u; index < std::min<size_t>(starts.size(), 16u); ++index)
        {
            const auto &[startPc, stat] = starts[index];
            std::cerr << "[vif1-vu-start] start_pc=";
            if (startPc == 0xFFFFFFFFu)
                std::cerr << "MSCNT";
            else
                std::cerr << "0x" << std::hex << startPc << std::dec;
            std::cerr << " calls=" << stat.calls
                      << " total_ms=" << (stat.nanoseconds / 1.0e6)
                      << std::endl;
        }
    }
}
