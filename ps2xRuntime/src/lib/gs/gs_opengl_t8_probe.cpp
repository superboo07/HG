#include "runtime/gs/gs_opengl_compute.h"

#include "runtime/gs/gs_cpu_backend.h"
#include "runtime/gs/ps2_gs_common.h"
#include "runtime/gs/ps2_gs_memory.h"
#include "runtime/gs/ps2_gs_psmct32.h"
#include "runtime/gs/ps2_gs_psmt8.h"

#include "rlgl.h"
#include "external/glad.h"

#include <array>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <iomanip>
#include <sstream>
#include <vector>

namespace
{
    constexpr uint32_t kWidth = 256u;
    constexpr uint32_t kHeight = 224u;
    constexpr uint32_t kTextureBlock = 256u;
    constexpr uint32_t kClutBlock = 512u;
    constexpr uint32_t kFrameFbp = 32u;
    constexpr uint32_t kDepthZbp = 48u;
    constexpr uint32_t kFrameBlock = kFrameFbp << 5u;
    constexpr uint32_t kDepthBlock = kDepthZbp << 5u;
    constexpr uint32_t kFrameWidth = 4u;
    constexpr uint32_t kTextureWidth = 2u;
    constexpr uint32_t kBenchmarkIterations = 2048u;

    constexpr std::array<std::array<uint8_t, 8>, 4> kBlockTableZ32{{
        {{24, 25, 28, 29, 8, 9, 12, 13}},
        {{26, 27, 30, 31, 10, 11, 14, 15}},
        {{16, 17, 20, 21, 0, 1, 4, 5}},
        {{18, 19, 22, 23, 2, 3, 6, 7}},
    }};

    uint32_t resolveCsm1ClutIndex(uint32_t index)
    {
        return (index & ~0x18u) | ((index & 0x08u) << 1u) | ((index & 0x10u) >> 1u);
    }

    std::array<uint32_t, 416> makeAddressTables()
    {
        std::array<uint32_t, 416> tables{};
        uint32_t cursor = 0u;
        for (const auto &row : GSPSMCT32::blockTable32)
            for (uint8_t value : row)
                tables[cursor++] = value;
        for (const auto &row : kBlockTableZ32)
            for (uint8_t value : row)
                tables[cursor++] = value;
        for (const auto &row : GSPSMCT32::columnTable32)
            for (uint8_t value : row)
                tables[cursor++] = value;
        for (const auto &row : GSPSMT8::blockTable8)
            for (uint8_t value : row)
                tables[cursor++] = value;
        for (const auto &row : GSPSMT8::columnTable8)
            for (uint8_t value : row)
                tables[cursor++] = value;
        return tables;
    }

    GSPrimitiveBatch makeTriangle()
    {
        GSPrimitiveBatch batch{};
        batch.vertexCount = 3u;
        batch.vertices[0] = {16.0625f, 16.1875f, 8388608.0, 0x80, 0x80, 0x80, 0x80,
                             1.0f, 0.0f, 0.0f, 0u, 0u, 0x80};
        batch.vertices[1] = {192.3125f, 32.0625f, 8388608.0, 0x80, 0x80, 0x80, 0x80,
                             1.0f, 0.0f, 0.0f, 1008u, 0u, 0x80};
        batch.vertices[2] = {48.1875f, 200.375f, 8388608.0, 0x80, 0x80, 0x80, 0x80,
                             1.0f, 0.0f, 0.0f, 0u, 1008u, 0x80};
        batch.state.prim.type = GS_PRIM_TRIANGLE;
        batch.state.prim.tme = true;
        batch.state.prim.fst = true;
        batch.state.context.frame = {kFrameFbp, kFrameWidth, GS_PSM_CT32, 0u};
        batch.state.context.zbuf = {kDepthZbp, GS_PSM_Z32, false};
        batch.state.context.scissor = {0u, static_cast<uint16_t>(kWidth - 1u),
                                        0u, static_cast<uint16_t>(kHeight - 1u)};
        batch.state.context.test = 1ull | (7ull << 1u) | (1ull << 16u) | (2ull << 17u);
        batch.state.context.tex0.tbp0 = kTextureBlock;
        batch.state.context.tex0.tbw = kTextureWidth;
        batch.state.context.tex0.psm = GS_PSM_T8;
        batch.state.context.tex0.tw = 6u;
        batch.state.context.tex0.th = 6u;
        batch.state.context.tex0.tcc = 1u;
        batch.state.context.tex0.tfx = 1u;
        batch.state.context.tex0.cbp = kClutBlock;
        batch.state.context.tex0.cpsm = GS_PSM_CT32;
        batch.state.context.tex0.csm = 0u;
        batch.state.texclut.cbw = 1u;
        batch.state.textureWidth = 64u;
        batch.state.textureHeight = 64u;
        batch.state.linearFilter = true;
        return batch;
    }

    void initializeVram(std::vector<uint8_t> &vram)
    {
        for (uint32_t y = 0u; y < 64u; ++y)
        {
            for (uint32_t x = 0u; x < 64u; ++x)
            {
                const uint32_t index = (x * 13u + y * 29u + (x ^ y) * 7u) & 0xFFu;
                GSMem::WriteP8(vram.data(), kTextureBlock, kTextureWidth, x, y, index);
            }
        }
        for (uint32_t index = 0u; index < 256u; ++index)
        {
            const uint32_t clutIndex = resolveCsm1ClutIndex(index);
            const uint32_t color = ((index * 17u) & 0xFFu) |
                                   (((index * 43u + 11u) & 0xFFu) << 8u) |
                                   (((index * 71u + 23u) & 0xFFu) << 16u) |
                                   (((index * 31u + 0x40u) & 0xFFu) << 24u);
            GSMem::WriteCT32(vram.data(), kClutBlock, 1u,
                              clutIndex & 0x0Fu, clutIndex >> 4u, color);
        }
    }

    constexpr const char *kShaderSource = R"glsl(
#version 430
layout(local_size_x = 8, local_size_y = 8) in;
layout(std430, binding = 0) buffer RawVram { uint vram[]; };
layout(std430, binding = 1) readonly buffer AddressTables { uint tables[]; };

uint address32(uint block, uint width, uint x, uint y, bool depth)
{
    uint page = (block >> 5u) + (y >> 5u) * max(width, 1u) + (x >> 6u);
    uint tableBase = depth ? 32u : 0u;
    uint blockId = (block & 31u) + tables[tableBase + (((y >> 3u) & 3u) * 8u) + ((x >> 3u) & 7u)];
    uint column = tables[64u + (y & 7u) * 8u + (x & 7u)];
    return ((page << 13u) + ((blockId >> 5u) << 13u) + (blockId & 31u) * 256u + column * 4u) & 0x003ffffcu;
}

uint addressT8(uint block, uint width, uint x, uint y)
{
    uint page = (block >> 5u) + (y >> 6u) * max(width >> 1u, 1u) + (x >> 7u);
    uint blockId = (block & 31u) + tables[128u + (((y >> 4u) & 3u) * 8u) + ((x >> 4u) & 7u)];
    uint column = tables[160u + (y & 15u) * 16u + (x & 15u)];
    return ((page << 13u) + ((blockId >> 5u) << 13u) + (blockId & 31u) * 256u + column) & 0x003fffffu;
}

uint readByte(uint address)
{
    return (vram[address >> 2u] >> ((address & 3u) * 8u)) & 255u;
}

uint resolveClut(uint index)
{
    return (index & ~0x18u) | ((index & 8u) << 1u) | ((index & 16u) >> 1u);
}

uint samplePoint(int inputU, int inputV)
{
    uint u = uint(inputU & 63);
    uint v = uint(inputV & 63);
    uint index = readByte(addressT8(256u, 2u, u, v));
    uint clutIndex = resolveClut(index);
    return vram[address32(512u, 1u, clutIndex & 15u, clutIndex >> 4u, false) >> 2u];
}

uint channel(uint value, uint shift) { return (value >> shift) & 255u; }
uint lerpChannel(uint c00, uint c10, uint c01, uint c11, float fx, float fy)
{
    precise float top = float(c00) + (float(c10) - float(c00)) * fx;
    precise float bottom = float(c01) + (float(c11) - float(c01)) * fx;
    precise float filtered = top + (bottom - top) * fy;
    return uint(clamp(floor(filtered + 0.5), 0.0, 255.0));
}

uint sampleLinear(float texU, float texV)
{
    float sampleU = texU - 0.5;
    float sampleV = texV - 0.5;
    int u0 = int(floor(sampleU));
    int v0 = int(floor(sampleV));
    float fx = sampleU - float(u0);
    float fy = sampleV - float(v0);
    uint c00 = samplePoint(u0, v0);
    uint c10 = samplePoint(u0 + 1, v0);
    uint c01 = samplePoint(u0, v0 + 1);
    uint c11 = samplePoint(u0 + 1, v0 + 1);
    return lerpChannel(channel(c00, 0u), channel(c10, 0u), channel(c01, 0u), channel(c11, 0u), fx, fy) |
           (lerpChannel(channel(c00, 8u), channel(c10, 8u), channel(c01, 8u), channel(c11, 8u), fx, fy) << 8u) |
           (lerpChannel(channel(c00, 16u), channel(c10, 16u), channel(c01, 16u), channel(c11, 16u), fx, fy) << 16u) |
           (lerpChannel(channel(c00, 24u), channel(c10, 24u), channel(c01, 24u), channel(c11, 24u), fx, fy) << 24u);
}

void main()
{
    uint x = gl_GlobalInvocationID.x;
    uint y = gl_GlobalInvocationID.y;
    if (x >= 256u || y >= 224u)
        return;

    const vec2 p0 = vec2(16.0625, 16.1875);
    const vec2 p1 = vec2(192.3125, 32.0625);
    const vec2 p2 = vec2(48.1875, 200.375);
    vec2 p = vec2(float(x) + 0.5, float(y) + 0.5);
    precise float denom = (p1.y - p2.y) * (p0.x - p2.x) + (p2.x - p1.x) * (p0.y - p2.y);
    precise float winding = denom < 0.0 ? -1.0 : 1.0;
    precise float invAbsDenom = 1.0 / abs(denom);
    precise float numerator0 = (p1.y - p2.y) * (p.x - p2.x) + (p2.x - p1.x) * (p.y - p2.y);
    precise float numerator1 = (p2.y - p0.y) * (p.x - p2.x) + (p0.x - p2.x) * (p.y - p2.y);
    precise float w0 = (numerator0 * winding) * invAbsDenom;
    precise float w1 = (numerator1 * winding) * invAbsDenom;
    precise float w2 = 1.0 - w0 - w1;
    if (w0 < -0.0001 || w1 < -0.0001 || w2 < -0.0001)
        return;

    precise float interpolatedU = 1008.0 * w1;
    precise float interpolatedV = 1008.0 * w2;
    uint iu = uint(interpolatedU);
    uint iv = uint(interpolatedV);
    uint outputColor = sampleLinear(float(iu) / 16.0, float(iv) / 16.0);
    uint depthAddress = address32(1536u, 4u, x, y, true);
    precise double depth0 = double(8388608.0) * double(w0);
    precise double depth1 = double(8388608.0) * double(w1);
    precise double depth2 = double(8388608.0) * double(w2);
    precise double interpolatedDepth = depth0 + depth1 + depth2;
    uint writeDepth = uint(interpolatedDepth + double(0.5));
    if (writeDepth < vram[depthAddress >> 2u])
        return;
    vram[address32(1024u, 4u, x, y, false) >> 2u] = outputColor;
    vram[depthAddress >> 2u] = writeDepth;
}
)glsl";
}

bool ProbeGSOpenGLT8Triangle(std::string &detail)
{
    std::vector<uint8_t> initialVram(static_cast<size_t>(GSMem::MEMORY_SIZE), 0u);
    initializeVram(initialVram);
    std::vector<uint8_t> cpuVram = initialVram;
    GSCpuBackend cpu;
    cpu.Initialize(cpuVram.data(), static_cast<uint32_t>(cpuVram.size()));
    cpu.Submit(makeTriangle());
    cpu.Sync(GSSyncReason::DebugReadback);

    const unsigned int shader = rlCompileShader(kShaderSource, RL_COMPUTE_SHADER);
    if (shader == 0u)
    {
        detail = "linear-T8 triangle shader compilation failed";
        return false;
    }
    const unsigned int program = rlLoadComputeShaderProgram(shader);
    if (program == 0u)
    {
        detail = "linear-T8 triangle shader link failed";
        return false;
    }
    const auto tables = makeAddressTables();
    const unsigned int vramBuffer = rlLoadShaderBuffer(
        static_cast<unsigned int>(initialVram.size()), initialVram.data(), RL_DYNAMIC_COPY);
    const unsigned int tableBuffer = rlLoadShaderBuffer(
        static_cast<unsigned int>(tables.size() * sizeof(uint32_t)), tables.data(), RL_STATIC_DRAW);
    const auto cleanup = [&]()
    {
        if (vramBuffer != 0u)
            rlUnloadShaderBuffer(vramBuffer);
        if (tableBuffer != 0u)
            rlUnloadShaderBuffer(tableBuffer);
        rlUnloadShaderProgram(program);
    };
    if (vramBuffer == 0u || tableBuffer == 0u)
    {
        cleanup();
        detail = "linear-T8 triangle SSBO allocation failed";
        return false;
    }

    rlEnableShader(program);
    rlBindShaderBuffer(vramBuffer, 0u);
    rlBindShaderBuffer(tableBuffer, 1u);
    rlComputeShaderDispatch(kWidth / 8u, kHeight / 8u, 1u);
    glMemoryBarrier(GL_SHADER_STORAGE_BARRIER_BIT | GL_BUFFER_UPDATE_BARRIER_BIT);
    std::vector<uint8_t> gpuVram(initialVram.size());
    rlReadShaderBuffer(vramBuffer, gpuVram.data(), static_cast<unsigned int>(gpuVram.size()), 0u);

    size_t mismatchCount = 0u;
    size_t firstMismatch = 0u;
    for (size_t offset = 0u; offset < cpuVram.size(); ++offset)
    {
        if (cpuVram[offset] != gpuVram[offset])
        {
            if (mismatchCount == 0u)
                firstMismatch = offset;
            ++mismatchCount;
        }
    }
    if (mismatchCount != 0u)
    {
        rlDisableShader();
        std::ostringstream message;
        message << "linear-T8 triangle VRAM mismatch: " << mismatchCount
                << " bytes differ, first at 0x" << std::hex << firstMismatch
                << " CPU=0x" << static_cast<uint32_t>(cpuVram[firstMismatch])
                << " GPU=0x" << static_cast<uint32_t>(gpuVram[firstMismatch]);
        detail = message.str();
        cleanup();
        return false;
    }

    uint32_t coveredPixels = 0u;
    for (uint32_t y = 0u; y < kHeight; ++y)
        for (uint32_t x = 0u; x < kWidth; ++x)
            if (GSMem::ReadZ32(cpuVram.data(), kDepthBlock, kFrameWidth, x, y) != 0u)
                ++coveredPixels;

    const auto start = std::chrono::steady_clock::now();
    for (uint32_t iteration = 0u; iteration < kBenchmarkIterations; ++iteration)
        rlComputeShaderDispatch(kWidth / 8u, kHeight / 8u, 1u);
    glMemoryBarrier(GL_SHADER_STORAGE_BARRIER_BIT | GL_BUFFER_UPDATE_BARRIER_BIT);
    glFinish();
    const auto end = std::chrono::steady_clock::now();
    rlDisableShader();
    cleanup();

    const double elapsedSeconds = std::chrono::duration<double>(end - start).count();
    const double candidatePixels = static_cast<double>(kWidth) * kHeight * kBenchmarkIterations;
    const double writtenPixels = static_cast<double>(coveredPixels) * kBenchmarkIterations;
    std::ostringstream message;
    message << "linear-T8 triangle matched all 4 MiB exactly (" << coveredPixels
            << " covered pixels); " << std::fixed << std::setprecision(1)
            << (candidatePixels / elapsedSeconds / 1000000.0)
            << " million candidate pixels/s, "
            << (writtenPixels / elapsedSeconds / 1000000.0)
            << " million covered pixels/s (GPU completion included)";
    detail = message.str();
    return true;
}
