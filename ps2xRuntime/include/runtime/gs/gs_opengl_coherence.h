#pragma once

#include "runtime/gs/gs_types.h"
#include "runtime/gs/ps2_gs_common.h"

#include <algorithm>
#include <bitset>
#include <cstddef>
#include <cstdint>

namespace GSOpenGLCoherence
{
    constexpr uint32_t BlockSize = 256u;
    constexpr uint32_t BlockCount = 16384u;
    using BlockMask = std::bitset<BlockCount>;

    struct DrawBlockUsage
    {
        BlockMask access{};
        BlockMask writes{};
        bool exact = false;
    };

    inline bool MarkRange(BlockMask &mask, uint32_t begin, uint64_t count)
    {
        if (count == 0u || begin >= BlockCount || count > BlockCount - begin)
            return false;
        const uint32_t end = begin + static_cast<uint32_t>(count);
        for (uint32_t block = begin; block < end; ++block)
            mask.set(block);
        return true;
    }

    inline bool SurfaceRange(uint8_t psm, uint32_t base, uint32_t bufferWidth,
                             uint32_t height, BlockMask &mask)
    {
        uint32_t pageHeight = 32u;
        uint32_t pagesPerRow = std::max(bufferWidth, 1u);
        switch (psm)
        {
        case GS_PSM_CT32:
        case GS_PSM_CT24:
        case GS_PSM_T8H:
        case GS_PSM_T4HL:
        case GS_PSM_T4HH:
        case GS_PSM_Z32:
        case GS_PSM_Z24:
            break;
        case GS_PSM_CT16:
        case GS_PSM_CT16S:
        case GS_PSM_Z16:
        case GS_PSM_Z16S:
            pageHeight = 64u;
            break;
        case GS_PSM_T8:
            pageHeight = 64u;
            pagesPerRow = std::max((bufferWidth + 1u) / 2u, 1u);
            break;
        case GS_PSM_T4:
            pageHeight = 128u;
            pagesPerRow = std::max((bufferWidth + 1u) / 2u, 1u);
            break;
        default:
            return false;
        }

        const uint64_t pageRows = (static_cast<uint64_t>(height) + pageHeight - 1u) /
                                  pageHeight;
        return MarkRange(mask, base, pageRows * pagesPerRow * 32u);
    }

    inline DrawBlockUsage DescribeDraw(const GSPrimitiveBatch &batch)
    {
        DrawBlockUsage usage{};
        if (batch.vertexCount == 0u)
        {
            usage.exact = true;
            return usage;
        }

        const GSDrawState &state = batch.state;
        const GSContext &context = state.context;
        const uint32_t frameBase = GSInternal::framePageBaseToBlock(context.frame.fbp);
        const uint32_t drawHeight = static_cast<uint32_t>(context.scissor.y1) + 1u;
        if (!SurfaceRange(context.frame.psm, frameBase, context.frame.fbw,
                          drawHeight, usage.access) ||
            !SurfaceRange(context.frame.psm, frameBase, context.frame.fbw,
                          drawHeight, usage.writes))
            return usage;

        const bool zTestEnabled = ((context.test >> 16u) & 1u) != 0u;
        if (zTestEnabled || !context.zbuf.zmask)
        {
            const uint32_t depthBase = GSInternal::framePageBaseToBlock(context.zbuf.zbp);
            if (!SurfaceRange(context.zbuf.psm, depthBase, context.frame.fbw,
                              drawHeight, usage.access))
                return usage;
            if (!context.zbuf.zmask &&
                !SurfaceRange(context.zbuf.psm, depthBase, context.frame.fbw,
                              drawHeight, usage.writes))
                return usage;
        }

        if (state.prim.tme)
        {
            if (!SurfaceRange(context.tex0.psm, context.tex0.tbp0, context.tex0.tbw,
                              std::max<uint32_t>(state.textureHeight, 1u), usage.access))
                return usage;
            const bool indexed = context.tex0.psm == GS_PSM_T8 ||
                                 context.tex0.psm == GS_PSM_T8H ||
                                 context.tex0.psm == GS_PSM_T4 ||
                                 context.tex0.psm == GS_PSM_T4HL ||
                                 context.tex0.psm == GS_PSM_T4HH;
            if (indexed && !MarkRange(usage.access, context.tex0.cbp, 32u))
                return usage;
        }

        usage.exact = true;
        return usage;
    }

    template <typename Callback>
    inline void ForEachRun(const BlockMask &mask, Callback callback)
    {
        uint32_t block = 0u;
        while (block < BlockCount)
        {
            while (block < BlockCount && !mask.test(block))
                ++block;
            if (block == BlockCount)
                break;
            const uint32_t begin = block;
            while (block < BlockCount && mask.test(block))
                ++block;
            callback(begin * BlockSize, (block - begin) * BlockSize);
        }
    }
}
