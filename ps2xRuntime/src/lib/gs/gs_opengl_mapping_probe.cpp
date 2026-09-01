#include "runtime/gs/gs_opengl_compute.h"

#include "runtime/gs/ps2_gs_memory.h"
#include "runtime/gs/ps2_gs_psmct32.h"
#include "runtime/gs/ps2_gs_psmt8.h"

#include "rlgl.h"
#include "external/glad.h"

#include <array>
#include <chrono>
#include <cstdint>
#include <iomanip>
#include <sstream>
#include <vector>

namespace
{
    constexpr uint32_t kCaseCount = 65536u;
    constexpr uint32_t kBenchmarkIterations = 512u;
    constexpr uint32_t kVramMask = static_cast<uint32_t>(GSMem::MEMORY_SIZE) - 1u;

    struct alignas(16) AddressInput
    {
        uint32_t block;
        uint32_t width;
        uint32_t xOrIndex;
        uint32_t packedYAndFormat;
    };
    static_assert(sizeof(AddressInput) == 16u);

    constexpr std::array<std::array<uint8_t, 8>, 4> kBlockTableZ32{{
        {{24, 25, 28, 29, 8, 9, 12, 13}},
        {{26, 27, 30, 31, 10, 11, 14, 15}},
        {{16, 17, 20, 21, 0, 1, 4, 5}},
        {{18, 19, 22, 23, 2, 3, 6, 7}},
    }};

    uint32_t nextRandom(uint32_t &state)
    {
        state = state * 1664525u + 1013904223u;
        return state;
    }

    uint32_t resolveCsm1ClutIndex(uint32_t index, uint32_t csa)
    {
        const uint32_t combined = (((csa & 0x0Fu) << 4u) + (index & 0xFFu)) & 0xFFu;
        return (combined & ~0x18u) | ((combined & 0x08u) << 1u) | ((combined & 0x10u) >> 1u);
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

    constexpr const char *kAddressShader = R"glsl(
#version 430
layout(local_size_x = 64) in;
layout(std430, binding = 0) readonly buffer AddressInputs { uvec4 inputs[]; };
layout(std430, binding = 1) writeonly buffer AddressOutputs { uint outputs[]; };
layout(std430, binding = 2) readonly buffer AddressTables { uint tables[]; };

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

uint resolveCsm1ClutIndex(uint index, uint csa)
{
    uint combined = (((csa & 15u) << 4u) + (index & 255u)) & 255u;
    return (combined & ~0x18u) | ((combined & 8u) << 1u) | ((combined & 16u) >> 1u);
}

void main()
{
    uint invocation = gl_GlobalInvocationID.x;
    uvec4 inputValue = inputs[invocation];
    uint format = inputValue.w >> 28u;
    uint y = inputValue.w & 0x0fffffffu;
    if (format == 0u)
        outputs[invocation] = address32(inputValue.x, inputValue.y, inputValue.z, y, false);
    else if (format == 1u)
        outputs[invocation] = address32(inputValue.x, inputValue.y, inputValue.z, y, true);
    else if (format == 2u)
        outputs[invocation] = addressT8(inputValue.x, inputValue.y, inputValue.z, y);
    else
    {
        uint clutIndex = resolveCsm1ClutIndex(inputValue.z & 255u, (inputValue.z >> 8u) & 15u);
        uint clutX = (y & 0xfffu) + (clutIndex & 15u);
        uint clutY = ((y >> 12u) & 0xfffu) + (clutIndex >> 4u);
        outputs[invocation] = address32(inputValue.x, inputValue.y, clutX, clutY, false);
    }
}
)glsl";
}

bool ProbeGSOpenGLAddressMapping(std::string &detail)
{
    const unsigned int shader = rlCompileShader(kAddressShader, RL_COMPUTE_SHADER);
    if (shader == 0u)
    {
        detail = "address-mapping compute shader compilation failed";
        return false;
    }
    const unsigned int program = rlLoadComputeShaderProgram(shader);
    if (program == 0u)
    {
        detail = "address-mapping compute shader link failed";
        return false;
    }

    std::vector<AddressInput> inputs(kCaseCount);
    std::vector<uint32_t> expected(kCaseCount);
    std::vector<uint32_t> outputs(kCaseCount, 0xFFFFFFFFu);
    uint32_t randomState = 0x48475553u;
    for (uint32_t caseIndex = 0u; caseIndex < kCaseCount; ++caseIndex)
    {
        const uint32_t format = caseIndex & 3u;
        AddressInput input{};
        input.block = nextRandom(randomState) & 0x3FFFu;
        input.width = 1u + (nextRandom(randomState) & 0x3Fu);
        if (format < 3u)
        {
            const uint32_t coordinateMask = format == 2u ? 0x7FFu : 0x3FFu;
            input.xOrIndex = nextRandom(randomState) & coordinateMask;
            const uint32_t y = nextRandom(randomState) & coordinateMask;
            input.packedYAndFormat = (format << 28u) | y;
            if (format == 0u)
                expected[caseIndex] = GSMem::AddressCT32(input.block, input.width, input.xOrIndex, y);
            else if (format == 1u)
                expected[caseIndex] = GSMem::AddressZ32(input.block, input.width, input.xOrIndex, y);
            else
                expected[caseIndex] = GSPSMT8::addrPSMT8(input.block, input.width, input.xOrIndex, y) & kVramMask;
        }
        else
        {
            const uint32_t index = nextRandom(randomState) & 0xFFu;
            const uint32_t csa = nextRandom(randomState) & 0x0Fu;
            const uint32_t cou = nextRandom(randomState) & 0x3Fu;
            const uint32_t cov = nextRandom(randomState) & 0x3Fu;
            input.xOrIndex = index | (csa << 8u);
            input.packedYAndFormat = (format << 28u) | cou | (cov << 12u);
            const uint32_t clutIndex = resolveCsm1ClutIndex(index, csa);
            expected[caseIndex] = GSMem::AddressCT32(
                input.block, input.width, cou + (clutIndex & 0x0Fu), cov + (clutIndex >> 4u));
        }
        inputs[caseIndex] = input;
    }

    const auto tables = makeAddressTables();
    const unsigned int inputBuffer = rlLoadShaderBuffer(
        static_cast<unsigned int>(inputs.size() * sizeof(AddressInput)), inputs.data(), RL_STATIC_DRAW);
    const unsigned int outputBuffer = rlLoadShaderBuffer(
        static_cast<unsigned int>(outputs.size() * sizeof(uint32_t)), outputs.data(), RL_DYNAMIC_COPY);
    const unsigned int tableBuffer = rlLoadShaderBuffer(
        static_cast<unsigned int>(tables.size() * sizeof(uint32_t)), tables.data(), RL_STATIC_DRAW);

    const auto cleanup = [&]()
    {
        if (inputBuffer != 0u)
            rlUnloadShaderBuffer(inputBuffer);
        if (outputBuffer != 0u)
            rlUnloadShaderBuffer(outputBuffer);
        if (tableBuffer != 0u)
            rlUnloadShaderBuffer(tableBuffer);
        rlUnloadShaderProgram(program);
    };
    if (inputBuffer == 0u || outputBuffer == 0u || tableBuffer == 0u)
    {
        cleanup();
        detail = "address-mapping SSBO allocation failed";
        return false;
    }

    rlEnableShader(program);
    rlBindShaderBuffer(inputBuffer, 0u);
    rlBindShaderBuffer(outputBuffer, 1u);
    rlBindShaderBuffer(tableBuffer, 2u);
    rlComputeShaderDispatch(kCaseCount / 64u, 1u, 1u);
    glMemoryBarrier(GL_SHADER_STORAGE_BARRIER_BIT | GL_BUFFER_UPDATE_BARRIER_BIT);
    rlReadShaderBuffer(outputBuffer, outputs.data(),
                       static_cast<unsigned int>(outputs.size() * sizeof(uint32_t)), 0u);

    for (uint32_t caseIndex = 0u; caseIndex < kCaseCount; ++caseIndex)
    {
        if (outputs[caseIndex] != expected[caseIndex])
        {
            rlDisableShader();
            std::ostringstream message;
            message << "address mismatch at case " << caseIndex
                    << ": expected 0x" << std::hex << expected[caseIndex]
                    << ", got 0x" << outputs[caseIndex];
            detail = message.str();
            cleanup();
            return false;
        }
    }

    const auto start = std::chrono::steady_clock::now();
    for (uint32_t iteration = 0u; iteration < kBenchmarkIterations; ++iteration)
        rlComputeShaderDispatch(kCaseCount / 64u, 1u, 1u);
    glMemoryBarrier(GL_SHADER_STORAGE_BARRIER_BIT | GL_BUFFER_UPDATE_BARRIER_BIT);
    glFinish();
    const auto end = std::chrono::steady_clock::now();
    rlDisableShader();
    cleanup();

    const double elapsedSeconds = std::chrono::duration<double>(end - start).count();
    const double mappingCount = static_cast<double>(kCaseCount) * kBenchmarkIterations;
    std::ostringstream message;
    message << kCaseCount << " mixed CT32/Z32-Z24/T8/CLUT addresses matched exactly; "
            << std::fixed << std::setprecision(1)
            << (mappingCount / elapsedSeconds / 1000000.0)
            << " million address mappings/s (" << mappingCount / 1000000.0
            << " million mappings, GPU completion included)";
    detail = message.str();
    return true;
}
