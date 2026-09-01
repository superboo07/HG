#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

struct Ps2AdpcmDecoderState
{
    int16_t previous = 0;
    int16_t previousPrevious = 0;
};

namespace ps2_adpcm
{
    // Decodes complete 16-byte Sony ADPCM blocks while preserving predictor
    // history across calls. Each block produces 28 signed PCM samples.
    bool decodeBlocks(const uint8_t *data,
                      size_t sizeBytes,
                      Ps2AdpcmDecoderState &state,
                      std::vector<int16_t> &outPcm);
}
