#include "runtime/ps2_vu1.h"
#include "runtime/gs/ps2_gif_arbiter.h"
#include "runtime/gs/gs_frontend.h"
#include "runtime/ps2_memory.h"
#include "ps2_vu1_detail.h"

#include <algorithm>
#include <array>
#include <bit>
#include <chrono>
#include <cfenv>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <limits>
#include <memory>
#include <mutex>
#include <ps2_log.h>
#include <sstream>
#include <type_traits>
#include <unordered_map>
#include <unordered_set>

namespace
{
    thread_local uint64_t g_vuRunProfileXgkickNanoseconds = 0u;
    thread_local uint32_t g_vuRunProfileXgkickPackets = 0u;
    thread_local uint64_t g_vuRunProfileDecodeNanoseconds = 0u;
    thread_local uint32_t g_vuRunProfileDecodeRebuilds = 0u;
    thread_local uint64_t g_vuPhaseSampleSequence = 0u;
    thread_local bool g_disableCapturedHotBlock = false;
    thread_local bool g_suppressVuExternalGif = false;
    thread_local bool g_capturedHotBlockRejected = false;
    constexpr uint8_t laneForComponent(uint32_t component)
    {
        return static_cast<uint8_t>(1u << (3u - component));
    }

    void captureVuProgramIfRequested(const uint8_t *code, uint32_t codeSize,
                                     uint32_t startPc, uint64_t generation)
    {
        static const std::string captureDirectory = []
        {
            const char *value = std::getenv("PS2X_VU1_CAPTURE_PROGRAMS_DIR");
            return value != nullptr ? std::string(value) : std::string{};
        }();
        if (captureDirectory.empty() || !code || codeSize == 0u)
            return;

        uint64_t hash = 1469598103934665603ull;
        for (uint32_t index = 0u; index < codeSize; ++index)
        {
            hash ^= code[index];
            hash *= 1099511628211ull;
        }
        const uint64_t captureKey = hash ^ (static_cast<uint64_t>(startPc) << 32u);
        static std::mutex captureMutex;
        static std::unordered_set<uint64_t> captured;
        std::lock_guard lock(captureMutex);
        if (!captured.insert(captureKey).second)
            return;

        std::error_code error;
        const std::filesystem::path directory(captureDirectory);
        std::filesystem::create_directories(directory, error);
        if (error)
        {
            std::cerr << "[vu1-capture] create directory failed: "
                      << error.message() << '\n';
            return;
        }

        std::ostringstream stem;
        stem << "vu1-h" << std::hex << std::setw(16) << std::setfill('0') << hash
             << "-pc" << std::setw(4) << startPc;
        const std::filesystem::path binaryPath = directory / (stem.str() + ".bin");
        std::ofstream binary(binaryPath, std::ios::binary | std::ios::trunc);
        binary.write(reinterpret_cast<const char *>(code), codeSize);
        binary.close();
        if (!binary)
        {
            std::cerr << "[vu1-capture] write failed: " << binaryPath << '\n';
            return;
        }

        const std::filesystem::path metadataPath = directory / (stem.str() + ".txt");
        std::ofstream metadata(metadataPath, std::ios::trunc);
        metadata << "hash=0x" << std::hex << hash << '\n'
                 << "start_pc=0x" << startPc << '\n'
                 << "generation=0x" << generation << '\n'
                 << "code_size=0x" << codeSize << '\n';
        std::cerr << "[vu1-capture] hash=0x" << std::hex << hash
                  << " start_pc=0x" << startPc
                  << " generation=0x" << generation
                  << " bytes=0x" << codeSize << std::dec << '\n';
    }

}

void VU1Interpreter::addVfRead(InstructionUsage &usage, uint8_t reg, uint8_t lanes)
{
    if (lanes == 0u)
        return;
    for (uint32_t index = 0; index < usage.vfReadCount; ++index)
    {
        if (usage.vfRead[index].reg == reg)
        {
            usage.vfRead[index].lanes |= lanes;
            return;
        }
    }
    if (usage.vfReadCount < usage.vfRead.size())
        usage.vfRead[usage.vfReadCount++] = {reg, lanes};
}

void VU1Interpreter::addVfWrite(InstructionUsage &usage, uint8_t reg, uint8_t lanes)
{
    if (reg == 0u || lanes == 0u)
        return;
    if (usage.vfWrite.reg == 0u)
        usage.vfWrite = {reg, lanes};
    else if (usage.vfWrite.reg == reg)
        usage.vfWrite.lanes |= lanes;
}

uint8_t VU1Interpreter::vfReadLanes(const InstructionUsage &usage, uint8_t reg)
{
    for (uint32_t index = 0; index < usage.vfReadCount; ++index)
    {
        if (usage.vfRead[index].reg == reg)
            return usage.vfRead[index].lanes;
    }
    return 0u;
}

VU1Interpreter::VU1Interpreter(Unit unit)
    : m_unit(unit)
{
    reset();
}

void VU1Interpreter::resetScheduler()
{
    m_flagPipeline = {};
    m_fdiv = {};
    m_efu = {};
    m_storePipeline = {};
    m_vfWritePipeline = {};
    m_viWritePipeline = {};
    m_accWritePipeline = {};
    m_flagPipelineFreeMask = 0xFFu;
    m_efuFreeMask = 0x03u;
    m_storePipelineFreeMask = 0xFFu;
    m_vfWritePipelineFreeMask = 0xFFFFu;
    m_viWritePipelineFreeMask = 0xFFu;
    m_accWritePipelineFreeMask = 0xFFu;
    m_xgkick = {};
    m_vfReady = {};
    m_viReady = {};
    m_accReady = {};
    m_vfLatestWrite = {};
    m_viLatestWrite = {};
    m_accLatestWrite = {};
    m_nextWriteSequence = 0;
    m_efuResourceReady = 0;
    m_workingClip = m_state.clip;
    m_viBranchBackupValue = 0;
    m_viBranchBackupReg = 0;
    m_viBranchBackupValid = false;
    m_stopRequested = false;
    m_pendingHaltD = false;
    m_pendingHaltT = false;
}

void VU1Interpreter::reset()
{
    std::memset(&m_state, 0, sizeof(m_state));
    m_state.vf[0][3] = 1.0f;
    m_state.q = 1.0f;
    m_state.r = 0x3F800000u;
    m_cycle = 0;
    resetScheduler();
}

float VU1Interpreter::broadcast(const float *vf, uint8_t bc)
{
    // execUpper supplies its already-normalized VT snapshot. Normalizing the
    // selected lane again is redundant and sits on every broadcast FMAC path.
    return vf[bc & 3u];
}

float VU1Interpreter::normalizeOperand(float value) const
{
    uint32_t bits = 0;
    std::memcpy(&bits, &value, sizeof(bits));
    const uint32_t exponent = (bits >> 23) & 0xFFu;
    if (exponent == 0u)
    {
        bits &= 0x80000000u;
    }
    else if (exponent == 0xFFu)
    {
        bits = (bits & 0x80000000u) | 0x7F7FFFFFu;
    }
    std::memcpy(&value, &bits, sizeof(value));
    return value;
}

float VU1Interpreter::normalizeResult(float value, uint32_t &laneFlags) const
{
    uint32_t bits = 0;
    std::memcpy(&bits, &value, sizeof(bits));
    const uint32_t sign = bits & 0x80000000u;
    const uint32_t magnitude = bits & 0x7FFFFFFFu;
    const uint32_t exponent = (bits >> 23) & 0xFFu;

    laneFlags = sign != 0u ? 0x2u : 0u;
    if (magnitude == 0u)
    {
        laneFlags |= 0x1u;
    }
    else if (exponent == 0u)
    {
        laneFlags |= 0x5u;
        bits = sign;
    }
    else if (exponent == 0xFFu)
    {
        laneFlags |= 0x8u;
        bits = sign | 0x7F7FFFFFu;
    }

    std::memcpy(&value, &bits, sizeof(value));
    return value;
}

uint32_t VU1Interpreter::microAddressMask() const
{
    return m_unit == Unit::VU1 ? 0x3FFFu : 0x0FFFu;
}

int32_t VU1Interpreter::readBranchVi(uint8_t reg) const
{
    if (reg == 0u)
        return 0;
    if (m_viBranchBackupValid &&
        m_viBranchBackupReg == reg)
    {
        return m_viBranchBackupValue;
    }
    return m_state.vi[reg];
}

void VU1Interpreter::recordViWriteForBranch(uint8_t reg, int32_t oldValue)
{
    if (reg == 0u)
        return;
    m_viBranchBackupValue = oldValue;
    m_viBranchBackupReg = reg;
    m_viBranchBackupValid = true;
}

void VU1Interpreter::applyDest(float *dst, const float *result, uint8_t dest)
{
    if (dest & 0x8u)
        dst[0] = result[0];
    if (dest & 0x4u)
        dst[1] = result[1];
    if (dest & 0x2u)
        dst[2] = result[2];
    if (dest & 0x1u)
        dst[3] = result[3];
}

void VU1Interpreter::applyDestAcc(const float *result, uint8_t dest)
{
    applyDest(m_state.acc, result, dest);
}

uint32_t VU1Interpreter::normalizeFmacResult(float *result, uint8_t dest,
                                             uint8_t laneFlags[4])
{
    uint32_t extraSticky = 0u;
    for (uint32_t component = 0; component < 4u; ++component)
    {
        laneFlags[component] = 0u;
        if ((dest & laneForComponent(component)) == 0u)
            continue;

        long double exactResult = 0.0L;
        uint8_t productFlags = 0u;
        if (calculateFmacExactResult(component, exactResult, productFlags))
        {
            laneFlags[component] = normalizeFmacExactResult(result[component], exactResult);
            extraSticky |= productFlags & 0xFu;
            continue;
        }

        uint32_t flags = 0u;
        result[component] = normalizeResult(result[component], flags);
        laneFlags[component] = static_cast<uint8_t>(flags);
    }
    return extraSticky;
}

bool VU1Interpreter::calculateFmacExactResult(uint32_t component,
                                               long double &result,
                                               uint8_t &productFlags) const
{
    productFlags = 0u;
    const uint32_t upper = m_currentUpperInstruction;
    const uint8_t op = static_cast<uint8_t>(upper & 0x3Fu);
    const uint8_t special = op >= 0x3Cu
                                ? static_cast<uint8_t>((upper & 3u) | ((upper >> 4) & 0x7Cu))
                                : 0xFFu;
    const uint8_t fs = FS(upper);
    const uint8_t ft = FT(upper);

    const auto operand = [this](float value)
    {
        return static_cast<long double>(normalizeOperand(value));
    };
    const auto vs = [&](uint32_t lane)
    {
        return operand(m_state.vf[fs][lane]);
    };
    const auto vt = [&](uint32_t lane)
    {
        return operand(m_state.vf[ft][lane]);
    };
    const auto acc = [&](uint32_t lane)
    {
        return operand(m_state.acc[lane]);
    };

    const auto q = [&]()
    {
        return operand(m_state.q);
    };
    const auto i = [&]()
    {
        return operand(m_state.i);
    };
    const auto product = [&](long double left, long double right,
                             bool contributesSticky)
    {
        const long double exactProduct = left * right;
        if (contributesSticky)
        {
            float roundedProduct = static_cast<float>(left) * static_cast<float>(right);
            productFlags = normalizeFmacExactResult(roundedProduct, exactProduct);
        }
        return exactProduct;
    };
    if (op < 0x3Cu)
    {
        if (op <= 0x03u)
            result = vs(component) + vt(op & 3u);
        else if (op <= 0x07u)
            result = vs(component) - vt(op & 3u);
        else if (op <= 0x0Bu)
            result = acc(component) + product(vs(component), vt(op & 3u), true);
        else if (op <= 0x0Fu)
            result = acc(component) - product(vs(component), vt(op & 3u), true);
        else if (op >= 0x18u && op <= 0x1Bu)
            result = product(vs(component), vt(op & 3u), false);
        else
        {
            switch (op)
            {
            case 0x1Cu:
                result = product(vs(component), q(), false);
                break;
            case 0x1Eu:
                result = product(vs(component), i(), false);
                break;
            case 0x20u:
                result = vs(component) + q();
                break;
            case 0x21u:
                result = acc(component) + product(vs(component), q(), true);
                break;
            case 0x22u:
                result = vs(component) + i();
                break;
            case 0x23u:
                result = acc(component) + product(vs(component), i(), true);
                break;
            case 0x24u:
                result = vs(component) - q();
                break;
            case 0x25u:
                result = acc(component) - product(vs(component), q(), true);
                break;
            case 0x26u:
                result = vs(component) - i();
                break;
            case 0x27u:
                result = acc(component) - product(vs(component), i(), true);
                break;
            case 0x28u:
                result = vs(component) + vt(component);
                break;
            case 0x29u:
                result = acc(component) + product(vs(component), vt(component), true);
                break;
            case 0x2Au:
                result = product(vs(component), vt(component), false);
                break;
            case 0x2Cu:
                result = vs(component) - vt(component);
                break;
            case 0x2Du:
                result = acc(component) - product(vs(component), vt(component), true);
                break;
            case 0x2Eu:
            {
                static constexpr uint8_t left[4] = {1u, 2u, 0u, 3u};
                static constexpr uint8_t right[4] = {2u, 0u, 1u, 3u};
                result = component == 3u
                             ? 0.0L
                             : acc(component) - product(vs(left[component]), vt(right[component]), true);
                break;
            }
            default:
                return false;
            }
        }
        return true;
    }

    if (special <= 0x03u)
        result = vs(component) + vt(special & 3u);
    else if (special <= 0x07u)
        result = vs(component) - vt(special & 3u);
    else if (special <= 0x0Bu)
        result = acc(component) + product(vs(component), vt(special & 3u), true);
    else if (special <= 0x0Fu)
        result = acc(component) - product(vs(component), vt(special & 3u), true);
    else if (special >= 0x18u && special <= 0x1Bu)
        result = product(vs(component), vt(special & 3u), false);
    else
    {
        switch (special)
        {
        case 0x1Cu:
            result = product(vs(component), q(), false);
            break;
        case 0x1Eu:
            result = product(vs(component), i(), false);
            break;
        case 0x20u:
            result = vs(component) + q();
            break;
        case 0x21u:
            result = acc(component) + product(vs(component), q(), true);
            break;
        case 0x22u:
            result = vs(component) + i();
            break;
        case 0x23u:
            result = acc(component) + product(vs(component), i(), true);
            break;
        case 0x24u:
            result = vs(component) - q();
            break;
        case 0x25u:
            result = acc(component) - product(vs(component), q(), true);
            break;
        case 0x26u:
            result = vs(component) - i();
            break;
        case 0x27u:
            result = acc(component) - product(vs(component), i(), true);
            break;
        case 0x28u:
            result = vs(component) + vt(component);
            break;
        case 0x29u:
            result = acc(component) + product(vs(component), vt(component), true);
            break;
        case 0x2Au:
            result = product(vs(component), vt(component), false);
            break;
        case 0x2Cu:
            result = vs(component) - vt(component);
            break;
        case 0x2Du:
            result = acc(component) - product(vs(component), vt(component), true);
            break;
        case 0x2Eu:
        {
            static constexpr uint8_t left[4] = {1u, 2u, 0u, 3u};
            static constexpr uint8_t right[4] = {2u, 0u, 1u, 3u};
            result = component == 3u
                         ? 0.0L
                         : vs(left[component]) * vt(right[component]);
            break;
        }
        default:
            return false;
        }
    }
    return true;
}

uint8_t VU1Interpreter::normalizeFmacExactResult(float &value,
                                                  long double exactResult) const
{
    const bool negative = std::signbit(exactResult);
    const long double magnitude = std::fabs(exactResult);
    const long double maximum = static_cast<long double>(std::numeric_limits<float>::max());
    const long double minimum = static_cast<long double>(std::numeric_limits<float>::min());
    uint8_t flags = negative ? 0x2u : 0u;

    uint32_t bits = negative ? 0x80000000u : 0u;
    if (magnitude == 0.0L)
    {
        flags |= 0x1u;
        std::memcpy(&value, &bits, sizeof(value));
    }
    else if (magnitude > maximum)
    {
        flags |= 0x8u;
        bits |= 0x7F7FFFFFu;
        std::memcpy(&value, &bits, sizeof(value));
    }
    else if (magnitude < minimum)
    {
        flags |= 0x5u;
        std::memcpy(&value, &bits, sizeof(value));
    }

    return flags;
}

void VU1Interpreter::updateFmacFlags(const uint8_t laneFlags[4], uint8_t dest,
                                     uint32_t extraSticky)
{
    if (dest == 0u)
        return;

    uint32_t mac = 0u;
    uint32_t status = 0u;
    for (uint32_t component = 0; component < 4u; ++component)
    {
        const uint8_t lane = laneForComponent(component);
        if ((dest & lane) == 0u)
            continue;

        const uint32_t flags = laneFlags[component];
        if ((flags & 0x1u) != 0u)
            mac |= lane;
        if ((flags & 0x2u) != 0u)
            mac |= static_cast<uint32_t>(lane) << 4;
        if ((flags & 0x4u) != 0u)
            mac |= static_cast<uint32_t>(lane) << 8;
        if ((flags & 0x8u) != 0u)
            mac |= static_cast<uint32_t>(lane) << 12;
        status |= flags;
    }

    if (m_flagPipelineFreeMask == 0u)
    {
        reportReservedInstruction(true, 0xFFFFFFFFu);
        return;
    }
    const uint32_t entryIndex = static_cast<uint32_t>(std::countr_zero(m_flagPipelineFreeMask));
    m_flagPipelineFreeMask = static_cast<uint8_t>(
        m_flagPipelineFreeMask & ~(1u << entryIndex));
    FlagPipelineEntry *entry = &m_flagPipeline[entryIndex];

    *entry = {};
    entry->valid = true;
    entry->issueCycle = m_cycle;
    entry->readyCycle = m_cycle + kFmacLatency;
    entry->mac = mac;
    entry->status = status;
    entry->extraSticky = extraSticky;
    entry->writesMac = true;
    entry->writesStatus = true;
}

void VU1Interpreter::applyFmacDest(float *dst, float *result, uint8_t dest)
{
    uint8_t laneFlags[4]{};
    const uint32_t extraSticky = normalizeFmacResult(result, dest, laneFlags);
    updateFmacFlags(laneFlags, dest, extraSticky);
    applyDest(dst, result, dest);
}

void VU1Interpreter::applyFmacDestAcc(float *result, uint8_t dest)
{
    uint8_t laneFlags[4]{};
    const uint32_t extraSticky = normalizeFmacResult(result, dest, laneFlags);
    updateFmacFlags(laneFlags, dest, extraSticky);
    applyDestAcc(result, dest);
}

void VU1Interpreter::queueFsset(uint16_t immediate)
{
    uint8_t active = static_cast<uint8_t>(~m_flagPipelineFreeMask);
    while (active != 0u)
    {
        const uint32_t index = static_cast<uint32_t>(std::countr_zero(active));
        FlagPipelineEntry &entry = m_flagPipeline[index];
        if (entry.valid && entry.issueCycle == m_cycle)
            entry.writesStatus = false;
        active = static_cast<uint8_t>(active & (active - 1u));
    }

    if (m_flagPipelineFreeMask == 0u)
    {
        reportReservedInstruction(false, 0xFFFFFFFEu);
        return;
    }
    const uint32_t index = static_cast<uint32_t>(std::countr_zero(m_flagPipelineFreeMask));
    m_flagPipelineFreeMask = static_cast<uint8_t>(m_flagPipelineFreeMask & ~(1u << index));
    FlagPipelineEntry &entry = m_flagPipeline[index];
    entry = {};
    entry.valid = true;
    entry.issueCycle = m_cycle;
    entry.readyCycle = m_cycle + kFmacLatency;
    entry.status = static_cast<uint32_t>(immediate) & 0xFC0u;
    entry.writesSticky = true;
}

void VU1Interpreter::queueClip(uint32_t clip)
{
    m_workingClip = ((m_workingClip << 6) | (clip & 0x3Fu)) & 0xFFFFFFu;
    if (m_flagPipelineFreeMask == 0u)
    {
        reportReservedInstruction(true, 0xFFFFFFFDu);
        return;
    }
    const uint32_t index = static_cast<uint32_t>(std::countr_zero(m_flagPipelineFreeMask));
    m_flagPipelineFreeMask = static_cast<uint8_t>(m_flagPipelineFreeMask & ~(1u << index));
    FlagPipelineEntry &entry = m_flagPipeline[index];
    entry = {};
    entry.valid = true;
    entry.issueCycle = m_cycle;
    entry.readyCycle = m_cycle + kFmacLatency;
    entry.clip = m_workingClip;
    entry.writesClip = true;
}

void VU1Interpreter::queueFcset(uint32_t clip)
{
    m_workingClip = clip & 0xFFFFFFu;
    uint8_t active = static_cast<uint8_t>(~m_flagPipelineFreeMask);
    while (active != 0u)
    {
        const uint32_t index = static_cast<uint32_t>(std::countr_zero(active));
        FlagPipelineEntry &entry = m_flagPipeline[index];
        if (entry.valid && entry.issueCycle == m_cycle)
            entry.writesClip = false;
        active = static_cast<uint8_t>(active & (active - 1u));
    }
    if (m_flagPipelineFreeMask == 0u)
    {
        reportReservedInstruction(false, 0xFFFFFFFAu);
        return;
    }
    const uint32_t index = static_cast<uint32_t>(std::countr_zero(m_flagPipelineFreeMask));
    m_flagPipelineFreeMask = static_cast<uint8_t>(m_flagPipelineFreeMask & ~(1u << index));
    FlagPipelineEntry &entry = m_flagPipeline[index];
    entry = {};
    entry.valid = true;
    entry.issueCycle = m_cycle;
    entry.readyCycle = m_cycle + kFmacLatency;
    entry.clip = m_workingClip;
    entry.writesClip = true;
}

void VU1Interpreter::queueQ(float value, uint32_t latency, uint32_t statusDi)
{
    uint32_t ignoredFlags = 0u;
    value = normalizeResult(value, ignoredFlags);
    m_fdiv.valid = true;
    m_fdiv.readyCycle = m_cycle + latency;
    m_fdiv.value = value;
    m_fdiv.statusDi = statusDi & 0x30u;
}

void VU1Interpreter::queueP(float value, uint32_t latency)
{
    uint32_t ignoredFlags = 0u;
    value = normalizeResult(value, ignoredFlags);
    if (m_efuFreeMask != 0u)
    {
        const uint32_t index = static_cast<uint32_t>(std::countr_zero(m_efuFreeMask));
        m_efuFreeMask = static_cast<uint8_t>(m_efuFreeMask & ~(1u << index));
        ScalarPipelineEntry &entry = m_efu[index];
        entry.valid = true;
        entry.readyCycle = m_cycle + latency;
        entry.value = value;
        // EFU throughput is one cycle shorter than result visibility.
        m_efuResourceReady = m_cycle + (latency > 0u ? latency - 1u : 0u);
        return;
    }
    reportReservedInstruction(false, 0xFFFFFFF9u);
}

void VU1Interpreter::queueStore(uint32_t address, const uint32_t words[4], uint8_t laneMask)
{
    if (m_storePipelineFreeMask != 0u)
    {
        const uint32_t index = static_cast<uint32_t>(std::countr_zero(m_storePipelineFreeMask));
        m_storePipelineFreeMask = static_cast<uint8_t>(m_storePipelineFreeMask & ~(1u << index));
        PendingStore &store = m_storePipeline[index];
        store.valid = true;
        store.readyCycle = m_cycle + 1u;
        store.address = address;
        store.laneMask = laneMask;
        std::copy(words, words + 4, store.words.begin());
        return;
    }
    reportReservedInstruction(false, 0xFFFFFFFCu);
}

void VU1Interpreter::queueVfWrite(uint8_t reg, uint8_t laneMask,
                                  const float value[4], uint32_t latency)
{
    if (reg == 0u || laneMask == 0u)
        return;
    if (m_vfWritePipelineFreeMask != 0u)
    {
        const uint32_t index = static_cast<uint32_t>(std::countr_zero(m_vfWritePipelineFreeMask));
        m_vfWritePipelineFreeMask = static_cast<uint16_t>(m_vfWritePipelineFreeMask & ~(1u << index));
        PendingVfWrite &write = m_vfWritePipeline[index];
        write = {};
        write.valid = true;
        write.readyCycle = m_cycle + latency;
        write.sequence = ++m_nextWriteSequence;
        write.reg = reg;
        write.laneMask = laneMask;
        std::copy(value, value + 4, write.value.begin());
        for (uint32_t component = 0; component < 4u; ++component)
        {
            if ((laneMask & laneForComponent(component)) != 0u)
                m_vfLatestWrite[reg][component] = write.sequence;
        }
        return;
    }
    reportReservedInstruction(false, 0xFFFFFFF7u);
}

void VU1Interpreter::queueViWrite(uint8_t reg, int32_t value, uint32_t latency)
{
    if (reg == 0u)
        return;
    if (m_viWritePipelineFreeMask != 0u)
    {
        const uint32_t index = static_cast<uint32_t>(std::countr_zero(m_viWritePipelineFreeMask));
        m_viWritePipelineFreeMask = static_cast<uint8_t>(m_viWritePipelineFreeMask & ~(1u << index));
        PendingViWrite &write = m_viWritePipeline[index];
        write = {};
        write.valid = true;
        write.readyCycle = m_cycle + latency;
        write.sequence = ++m_nextWriteSequence;
        write.reg = reg;
        write.value = value;
        m_viLatestWrite[reg] = write.sequence;
        return;
    }
    reportReservedInstruction(false, 0xFFFFFFF6u);
}

void VU1Interpreter::queueAccWrite(uint8_t laneMask, const float value[4], uint32_t latency)
{
    if (laneMask == 0u)
        return;
    if (m_accWritePipelineFreeMask != 0u)
    {
        const uint32_t index = static_cast<uint32_t>(std::countr_zero(m_accWritePipelineFreeMask));
        m_accWritePipelineFreeMask = static_cast<uint8_t>(m_accWritePipelineFreeMask & ~(1u << index));
        PendingAccWrite &write = m_accWritePipeline[index];
        write = {};
        write.valid = true;
        write.readyCycle = m_cycle + latency;
        write.sequence = ++m_nextWriteSequence;
        write.laneMask = laneMask;
        std::copy(value, value + 4, write.value.begin());
        for (uint32_t component = 0; component < 4u; ++component)
        {
            if ((laneMask & laneForComponent(component)) != 0u)
                m_accLatestWrite[component] = write.sequence;
        }
        return;
    }
    reportReservedInstruction(true, 0xFFFFFFF5u);
}

void VU1Interpreter::commitReadyPipelines()
{
    uint8_t activeFlags = static_cast<uint8_t>(~m_flagPipelineFreeMask);
    while (activeFlags != 0u)
    {
        const uint32_t index = static_cast<uint32_t>(std::countr_zero(activeFlags));
        const uint8_t bit = static_cast<uint8_t>(1u << index);
        activeFlags = static_cast<uint8_t>(activeFlags & (activeFlags - 1u));
        FlagPipelineEntry &entry = m_flagPipeline[index];
        if (!entry.valid || entry.readyCycle > m_cycle)
            continue;

        if (entry.writesMac)
            m_state.mac = entry.mac;
        if (entry.writesStatus)
        {
            const uint32_t current = entry.status & 0xFu;
            m_state.status = (m_state.status & 0xFF0u) | current | ((current | entry.extraSticky) << 6);
        }
        if (entry.writesSticky)
        {
            m_state.status = (m_state.status & 0x03Fu) | (entry.status & 0xFC0u);
        }
        if (entry.writesClip)
            m_state.clip = entry.clip;
        entry = {};
        m_flagPipelineFreeMask = static_cast<uint8_t>(m_flagPipelineFreeMask | bit);
    }

    if (m_fdiv.valid && m_fdiv.readyCycle <= m_cycle)
    {
        m_state.q = m_fdiv.value;
        const uint32_t currentDi = m_fdiv.statusDi & 0x30u;
        m_state.status = (m_state.status & 0xFCFu) | currentDi | (currentDi << 6);
        m_fdiv = {};
    }

    uint8_t activeEfu = static_cast<uint8_t>((~m_efuFreeMask) & 0x03u);
    while (activeEfu != 0u)
    {
        const uint32_t index = static_cast<uint32_t>(std::countr_zero(activeEfu));
        const uint8_t bit = static_cast<uint8_t>(1u << index);
        activeEfu = static_cast<uint8_t>(activeEfu & (activeEfu - 1u));
        ScalarPipelineEntry &entry = m_efu[index];
        if (entry.valid && entry.readyCycle <= m_cycle)
        {
            m_state.p = entry.value;
            entry = {};
            m_efuFreeMask = static_cast<uint8_t>(m_efuFreeMask | bit);
        }
    }

    uint8_t activeStores = static_cast<uint8_t>(~m_storePipelineFreeMask);
    while (activeStores != 0u)
    {
        const uint32_t index = static_cast<uint32_t>(std::countr_zero(activeStores));
        const uint8_t bit = static_cast<uint8_t>(1u << index);
        activeStores = static_cast<uint8_t>(activeStores & (activeStores - 1u));
        PendingStore &store = m_storePipeline[index];
        if (!store.valid || store.readyCycle > m_cycle)
            continue;
        if (m_activeVuData && store.address + 16u <= m_activeVuDataSize)
        {
            uint32_t oldWords[4]{};
            std::memcpy(oldWords, m_activeVuData + store.address, sizeof(oldWords));
            for (uint32_t component = 0; component < 4u; ++component)
            {
                if ((store.laneMask & laneForComponent(component)) != 0u)
                    oldWords[component] = store.words[component];
            }
            std::memcpy(m_activeVuData + store.address, oldWords, sizeof(oldWords));
        }
        store = {};
        m_storePipelineFreeMask = static_cast<uint8_t>(m_storePipelineFreeMask | bit);
    }

    uint16_t activeVfWrites = static_cast<uint16_t>(~m_vfWritePipelineFreeMask);
    while (activeVfWrites != 0u)
    {
        const uint32_t index = static_cast<uint32_t>(std::countr_zero(activeVfWrites));
        const uint16_t bit = static_cast<uint16_t>(1u << index);
        activeVfWrites = static_cast<uint16_t>(activeVfWrites & (activeVfWrites - 1u));
        PendingVfWrite &write = m_vfWritePipeline[index];
        if (!write.valid || write.readyCycle > m_cycle)
            continue;
        for (uint32_t component = 0; component < 4u; ++component)
        {
            if ((write.laneMask & laneForComponent(component)) != 0u &&
                m_vfLatestWrite[write.reg][component] == write.sequence)
            {
                m_state.vf[write.reg][component] = write.value[component];
            }
        }
        write = {};
        m_vfWritePipelineFreeMask = static_cast<uint16_t>(m_vfWritePipelineFreeMask | bit);
    }

    uint8_t activeViWrites = static_cast<uint8_t>(~m_viWritePipelineFreeMask);
    while (activeViWrites != 0u)
    {
        const uint32_t index = static_cast<uint32_t>(std::countr_zero(activeViWrites));
        const uint8_t bit = static_cast<uint8_t>(1u << index);
        activeViWrites = static_cast<uint8_t>(activeViWrites & (activeViWrites - 1u));
        PendingViWrite &write = m_viWritePipeline[index];
        if (!write.valid || write.readyCycle > m_cycle)
            continue;
        if (m_viLatestWrite[write.reg] == write.sequence)
            m_state.vi[write.reg] = static_cast<int16_t>(write.value);
        write = {};
        m_viWritePipelineFreeMask = static_cast<uint8_t>(m_viWritePipelineFreeMask | bit);
    }

    uint8_t activeAccWrites = static_cast<uint8_t>(~m_accWritePipelineFreeMask);
    while (activeAccWrites != 0u)
    {
        const uint32_t index = static_cast<uint32_t>(std::countr_zero(activeAccWrites));
        const uint8_t bit = static_cast<uint8_t>(1u << index);
        activeAccWrites = static_cast<uint8_t>(activeAccWrites & (activeAccWrites - 1u));
        PendingAccWrite &write = m_accWritePipeline[index];
        if (!write.valid || write.readyCycle > m_cycle)
            continue;
        for (uint32_t component = 0; component < 4u; ++component)
        {
            if ((write.laneMask & laneForComponent(component)) != 0u &&
                m_accLatestWrite[component] == write.sequence)
            {
                m_state.acc[component] = write.value[component];
            }
        }
        write = {};
        m_accWritePipelineFreeMask = static_cast<uint8_t>(m_accWritePipelineFreeMask | bit);
    }
}

void VU1Interpreter::progressXgkick()
{
    if (!m_xgkick.active || !m_activeVuData || m_activeVuDataSize == 0u)
        return;

    ++m_xgkick.cycleCredit;
    while (m_xgkick.active && m_xgkick.cycleCredit >= 2u)
    {
        m_xgkick.cycleCredit -= 2u;
        if (m_xgkick.copiedBytes > XgkickPipeline::kBufferSize - 16u)
        {
            reportReservedInstruction(false, 0xFFFFFFFBu);
            m_xgkick.active = false;
            return;
        }

        const uint32_t qwordOffset = m_xgkick.copiedBytes;
        const uint32_t source =
            (m_xgkick.sourceAddress + m_xgkick.copiedBytes) % m_activeVuDataSize;
        const uint32_t firstBytes =
            std::min<uint32_t>(16u, m_activeVuDataSize - source);
        std::memcpy(m_xgkick.packet.data() + m_xgkick.copiedBytes,
                    m_activeVuData + source, firstBytes);
        if (firstBytes < 16u)
        {
            std::memcpy(m_xgkick.packet.data() + m_xgkick.copiedBytes + firstBytes,
                        m_activeVuData, 16u - firstBytes);
        }
        m_xgkick.copiedBytes += 16u;

        if (m_xgkick.currentTagEnd == 0u)
        {
            uint64_t tagLo = 0;
            std::memcpy(&tagLo, m_xgkick.packet.data() + qwordOffset, sizeof(tagLo));
            const uint32_t nloop = static_cast<uint32_t>(tagLo & 0x7FFFu);
            const uint32_t format = static_cast<uint32_t>((tagLo >> 58) & 0x3u);
            uint32_t nreg = static_cast<uint32_t>((tagLo >> 60) & 0xFu);
            if (nreg == 0u)
                nreg = 16u;

            uint64_t tagBytes = 16u;
            if (format == 0u)
                tagBytes += static_cast<uint64_t>(nloop) * nreg * 16u;
            else if (format == 1u)
                tagBytes += ((static_cast<uint64_t>(nloop) * nreg + 1u) & ~1ull) * 8u;
            else if (format == GIF_FMT_IMAGE || format == GIF_FMT_IMAGE2)
                tagBytes += static_cast<uint64_t>(nloop) * 16u;

            // PATH1 addresses wrap inside 16 KiB VU1 data memory. A single
            // GIF tag which claims that entire space (or more) cannot form a
            // packet before wrapping back onto itself, so hardware-compatible
            // implementations cancel the kick instead of decoding arbitrary
            // following VU data as another instruction stream.
            if (tagBytes >= m_activeVuDataSize)
            {
                m_xgkick.active = false;
                return;
            }

            if (tagBytes > XgkickPipeline::kBufferSize - qwordOffset)
            {
                reportReservedInstruction(false, 0xFFFFFFFBu);
                m_xgkick.active = false;
                return;
            }
            m_xgkick.currentTagEnd = qwordOffset + static_cast<uint32_t>(tagBytes);
            m_xgkick.currentTagEop = ((tagLo >> 15) & 1u) != 0u;
            if (m_xgkick.currentTagEop)
                m_xgkick.totalBytes = m_xgkick.currentTagEnd;
        }

        if (m_xgkick.copiedBytes >= m_xgkick.currentTagEnd)
        {
            if (m_xgkick.currentTagEop)
                finishXgkick();
            else
            {
                // The next transferred qword is another GIFtag.
                m_xgkick.currentTagEnd = 0u;
                m_xgkick.currentTagEop = false;
            }
        }
    }
}

void VU1Interpreter::finishXgkick()
{
    if (!m_xgkick.active)
        return;

    static const auto badStqTraceEpoch = std::chrono::steady_clock::now();
    static const double badStqTraceDelaySeconds = []()
    {
        const char *value = std::getenv("PS2X_VU1_BAD_STQ_TRACE_DELAY_SECONDS");
        return value && *value ? std::max(0.0, std::strtod(value, nullptr)) : 0.0;
    }();
    if (std::getenv("PS2X_VU1_BAD_STQ_TRACE") != nullptr && m_xgkick.totalBytes >= 64u &&
        std::chrono::duration<double>(std::chrono::steady_clock::now() - badStqTraceEpoch).count() >=
            badStqTraceDelaySeconds)
    {
        uint64_t tagLo = 0u, tagHi = 0u;
        std::memcpy(&tagLo, m_xgkick.packet.data(), sizeof(tagLo));
        std::memcpy(&tagHi, m_xgkick.packet.data() + 8u, sizeof(tagHi));
        const uint32_t nloop = static_cast<uint32_t>(tagLo & 0x7FFFu);
        const uint32_t format = static_cast<uint32_t>((tagLo >> 58u) & 0x3u);
        uint32_t nreg = static_cast<uint32_t>((tagLo >> 60u) & 0xFu);
        if (nreg == 0u)
            nreg = 16u;
        if (format == 0u && nreg == 3u && (tagHi & 0xFFFu) == 0x512u)
        {
            float maxAbsStq = 0.0f;
            uint32_t maxLoop = 0u;
            uint32_t maxComponent = 0u;
            const uint32_t availableLoops = std::min<uint32_t>(
                nloop, (m_xgkick.totalBytes - 16u) / 48u);
            for (uint32_t loop = 0u; loop < availableLoops; ++loop)
            {
                for (uint32_t component = 0u; component < 3u; ++component)
                {
                    float value = 0.0f;
                    std::memcpy(&value,
                                m_xgkick.packet.data() + 16u + loop * 48u + component * 4u,
                                sizeof(value));
                    const float magnitude = std::fabs(value);
                    if (magnitude > maxAbsStq)
                    {
                        maxAbsStq = magnitude;
                        maxLoop = loop;
                        maxComponent = component;
                    }
                }
            }
            if (maxAbsStq > 100.0f)
            {
                static std::atomic<uint32_t> badPacketCount{0u};
                const uint32_t ordinal = badPacketCount.fetch_add(1u, std::memory_order_relaxed);
                if (ordinal < 64u)
                {
                    std::cerr << "[vu1-bad-stq] ordinal=" << ordinal
                              << " issue_pc=0x" << std::hex << m_xgkick.issuePc
                              << " finish_pc=0x" << m_state.pc
                              << " qword=0x" << m_xgkick.qwordAddress
                              << std::dec << " bytes=" << m_xgkick.totalBytes
                              << " loop=" << maxLoop
                              << " component=" << maxComponent
                              << " magnitude=" << maxAbsStq << std::endl;
                }

                static std::atomic<bool> codeDumped{false};
                const char *dumpPath = std::getenv("PS2X_VU1_BAD_STQ_CODE_DUMP");
                bool expected = false;
                if (dumpPath && *dumpPath && m_activeMemory &&
                    codeDumped.compare_exchange_strong(expected, true, std::memory_order_relaxed))
                {
                    if (std::FILE *file = std::fopen(dumpPath, "wb"))
                    {
                        std::fwrite(m_activeMemory->getVU1Code(), 1u, PS2_VU1_CODE_SIZE, file);
                        std::fclose(file);
                        std::cerr << "[vu1-bad-stq] dumped micro memory to " << dumpPath << std::endl;
                    }
                }

                static std::atomic<bool> dataDumped{false};
                const char *dataDumpPath = std::getenv("PS2X_VU1_BAD_STQ_DATA_DUMP");
                expected = false;
                if (dataDumpPath && *dataDumpPath && m_activeVuData &&
                    dataDumped.compare_exchange_strong(expected, true, std::memory_order_relaxed))
                {
                    if (std::FILE *file = std::fopen(dataDumpPath, "wb"))
                    {
                        std::fwrite(m_activeVuData, 1u, m_activeVuDataSize, file);
                        std::fclose(file);
                        std::cerr << "[vu1-bad-stq] dumped VU data memory to " << dataDumpPath << std::endl;
                    }
                }

                static std::atomic<bool> stateDumped{false};
                const char *stateDumpPath = std::getenv("PS2X_VU1_BAD_STQ_STATE_DUMP");
                expected = false;
                if (stateDumpPath && *stateDumpPath &&
                    stateDumped.compare_exchange_strong(expected, true, std::memory_order_relaxed))
                {
                    if (std::FILE *file = std::fopen(stateDumpPath, "wb"))
                    {
                        std::fwrite(&m_state, 1u, sizeof(m_state), file);
                        std::fclose(file);
                        std::cerr << "[vu1-bad-stq] dumped VU state to " << stateDumpPath << std::endl;
                    }
                }
            }
        }
    }

    const bool profileRun = std::getenv("PS2X_VU1_RUN_PROFILE") != nullptr ||
                            std::getenv("PS2X_VU1_AGGREGATE_PROFILE_SECONDS") != nullptr;
    const auto profileStart = profileRun ? std::chrono::steady_clock::now()
                                         : std::chrono::steady_clock::time_point{};
    if (!g_suppressVuExternalGif)
    {
        if (m_activeMemory)
            m_activeMemory->submitGifPacket(GifPathId::Path1, m_xgkick.packet.data(), m_xgkick.totalBytes);
        else if (m_activeGs)
            m_activeGs->processGIFPacket(m_xgkick.packet.data(), m_xgkick.totalBytes);
    }
    if (profileRun)
    {
        g_vuRunProfileXgkickNanoseconds += static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(
                std::chrono::steady_clock::now() - profileStart)
                .count());
        ++g_vuRunProfileXgkickPackets;
    }
    m_xgkick.active = false;
}

void VU1Interpreter::startXgkick(uint32_t qwordAddress)
{
    if (m_unit != Unit::VU1 || !m_activeVuData || m_activeVuDataSize < 16u)
        return;

    const uint32_t sourceAddress = (qwordAddress * 16u) % m_activeVuDataSize;
    m_xgkick = {};
    m_xgkick.active = true;
    m_xgkick.qwordAddress = qwordAddress;
    m_xgkick.sourceAddress = sourceAddress;
    m_xgkick.cycleCredit = 1u; // XGKICK's issue cycle counts toward PATH1.
    m_xgkick.issueCycle = m_cycle;
    m_xgkick.issuePc = m_state.pc;
}

void VU1Interpreter::advanceOneCycle()
{
    ++m_cycle;
    // LSU commits become visible at the cycle boundary before PATH1 consumes
    // its next qword from VU memory.
    commitReadyPipelines();
    progressXgkick();
}

void VU1Interpreter::advanceTo(uint64_t targetCycle)
{
    while (m_cycle < targetCycle)
        advanceOneCycle();
}

bool VU1Interpreter::pipelinesPending() const
{
    if (m_fdiv.valid || m_xgkick.active)
        return true;
    return m_efuFreeMask != 0x03u ||
           m_flagPipelineFreeMask != 0xFFu ||
           m_storePipelineFreeMask != 0xFFu ||
           m_vfWritePipelineFreeMask != 0xFFFFu ||
           m_viWritePipelineFreeMask != 0xFFu ||
           m_accWritePipelineFreeMask != 0xFFu;
}

void VU1Interpreter::flushPipelines()
{
    while (pipelinesPending())
        advanceOneCycle();
}

uint64_t VU1Interpreter::calculatePairReadyCycle(const DecodedInstructionPair &decoded) const
{
    uint64_t ready = m_cycle;
    const InstructionUsage *usages[2] = {
        &decoded.upperUsage,
        &decoded.lowerUsage};
    for (const InstructionUsage *usage : usages)
    {
        if (!usage)
            continue;
        for (uint32_t index = 0; index < usage->vfReadCount; ++index)
        {
            const VfAccess &access = usage->vfRead[index];
            for (uint32_t component = 0; component < 4u; ++component)
            {
                if ((access.lanes & laneForComponent(component)) != 0u)
                    ready = std::max(ready, m_vfReady[access.reg][component]);
            }
        }
        uint16_t viReads = static_cast<uint16_t>(usage->viRead & 0xFFFEu);
        while (viReads != 0u)
        {
            const uint32_t reg = static_cast<uint32_t>(std::countr_zero(viReads));
            ready = std::max(ready, m_viReady[reg]);
            viReads = static_cast<uint16_t>(viReads & (viReads - 1u));
        }
        for (uint32_t component = 0; component < 4u; ++component)
        {
            if ((usage->accRead & laneForComponent(component)) != 0u)
                ready = std::max(ready, m_accReady[component]);
        }
    }

    if (decoded.lowerUsage.pipeline == PipelineFdiv && m_fdiv.valid)
        ready = std::max(ready, m_fdiv.readyCycle);
    if (decoded.lowerUsage.pipeline == PipelineEfu)
        ready = std::max(ready, m_efuResourceReady);
    if (decoded.lowerUsage.waitQ && m_fdiv.valid)
        ready = std::max(ready, m_fdiv.readyCycle);
    if (decoded.lowerUsage.waitP)
    {
        for (const ScalarPipelineEntry &entry : m_efu)
            if (entry.valid)
                ready = std::max(ready, entry.readyCycle);
    }
    if (decoded.lowerUsage.pipeline == PipelineXgkick && m_xgkick.active)
        ready = std::max(ready, m_cycle + 1u);
    return ready;
}

void VU1Interpreter::markPairWrites(const DecodedInstructionPair &decoded)
{
    const VfAccess lowerWrite = decoded.lowerUsage.vfWrite;
    if (lowerWrite.reg != 0u &&
        decoded.suppressedLowerVf != lowerWrite.reg)
    {
        const uint32_t latency = decoded.lowerUsage.vfLatency != 0u
                                     ? decoded.lowerUsage.vfLatency
                                     : decoded.lowerUsage.latency;
        for (uint32_t component = 0; component < 4u; ++component)
        {
            if ((lowerWrite.lanes & laneForComponent(component)) != 0u)
                m_vfReady[lowerWrite.reg][component] = m_cycle + latency;
        }
    }

    const VfAccess upperWrite = decoded.upperUsage.vfWrite;
    if (upperWrite.reg != 0u)
    {
        const uint32_t latency = decoded.upperUsage.vfLatency != 0u
                                     ? decoded.upperUsage.vfLatency
                                     : decoded.upperUsage.latency;
        for (uint32_t component = 0; component < 4u; ++component)
        {
            if ((upperWrite.lanes & laneForComponent(component)) != 0u)
                m_vfReady[upperWrite.reg][component] = m_cycle + latency;
        }
    }

    uint16_t viWrites = static_cast<uint16_t>(decoded.lowerUsage.viWrite & 0xFFFEu);
    while (viWrites != 0u)
    {
        const uint32_t reg = static_cast<uint32_t>(std::countr_zero(viWrites));
        m_viReady[reg] = m_cycle + (decoded.lowerUsage.viLatency != 0u ? decoded.lowerUsage.viLatency : decoded.lowerUsage.latency);
        viWrites = static_cast<uint16_t>(viWrites & (viWrites - 1u));
    }
    for (uint32_t component = 0; component < 4u; ++component)
    {
        if ((decoded.upperUsage.accWrite & laneForComponent(component)) != 0u)
            m_accReady[component] = m_cycle + kAccForwardLatency;
    }
}

VU1Interpreter::InstructionUsage VU1Interpreter::decodeUpperUsage(uint32_t upper) const
{
    InstructionUsage usage;
    usage.pipeline = PipelineFmac;
    usage.latency = kFmacLatency;

    const uint8_t op = static_cast<uint8_t>(upper & 0x3Fu);
    const uint8_t dest = DEST(upper);
    const uint8_t fs = FS(upper);
    const uint8_t ft = FT(upper);
    const uint8_t fd = FD(upper);

    if (op <= 0x2Fu)
    {
        addVfRead(usage, fs, dest);
        addVfWrite(usage, fd, dest);
        if (op <= 0x1Bu)
            addVfRead(usage, ft, laneForComponent(op & 3u));
        else if (op >= 0x28u)
            addVfRead(usage, ft, op == 0x2Eu ? 0xEu : dest);
        if (op == 0x08u || op == 0x09u || op == 0x0Au || op == 0x0Bu ||
            op == 0x0Cu || op == 0x0Du || op == 0x0Eu || op == 0x0Fu ||
            op == 0x21u || op == 0x23u || op == 0x25u || op == 0x27u ||
            op == 0x29u || op == 0x2Du || op == 0x2Eu)
        {
            usage.accRead = dest;
        }
        return usage;
    }

    if (op >= 0x3Cu)
    {
        const uint8_t special = static_cast<uint8_t>((upper & 3u) | ((upper >> 4) & 0x7Cu));
        const bool writesAcc =
            special <= 0x0Fu ||
            (special >= 0x18u && special <= 0x1Cu) ||
            special == 0x1Eu ||
            (special >= 0x20u && special <= 0x2Au) ||
            (special >= 0x2Cu && special <= 0x2Eu);
        if (writesAcc)
        {
            addVfRead(usage, fs, dest);
            if (special <= 0x1Bu)
                addVfRead(usage, ft, laneForComponent(special & 3u));
            else if ((special >= 0x28u && special <= 0x2Eu))
                addVfRead(usage, ft, special == 0x2Eu ? 0xEu : dest);
            usage.accWrite = dest;
            if ((special >= 0x08u && special <= 0x0Fu) ||
                special == 0x21u || special == 0x23u || special == 0x25u ||
                special == 0x27u || special == 0x29u || special == 0x2Du)
            {
                usage.accRead = dest;
            }
        }
        else if (special >= 0x10u && special <= 0x17u)
        {
            addVfRead(usage, fs, dest);
            addVfWrite(usage, ft, dest);
        }
        else if (special == 0x1Du)
        {
            addVfRead(usage, fs, dest);
            addVfWrite(usage, ft, dest);
        }
        else if (special == 0x1Fu)
        {
            addVfRead(usage, fs, 0xEu);
            addVfRead(usage, ft, 0x1u);
            usage.writesClip = true;
        }
        else if (special != 0x2Fu && special != 0x30u)
        {
            usage.reserved = true;
        }
        return usage;
    }

    usage.reserved = true;
    return usage;
}

VU1Interpreter::InstructionUsage VU1Interpreter::decodeLowerUsage(uint32_t lower) const
{
    InstructionUsage usage;
    if (lower == 0u || lower == 0x8000033Cu)
        return usage;

    const uint8_t opHi = static_cast<uint8_t>((lower >> 25) & 0x7Fu);
    const uint8_t vfT = FT(lower);
    const uint8_t vfS = FS(lower);
    const uint8_t viT = VIT(lower);
    const uint8_t viS = VIS(lower);
    const uint8_t viD = VID(lower);
    const uint8_t dest = DEST(lower);
    auto readVi = [&](uint8_t reg)
    {
        if (reg != 0u)
            usage.viRead |= static_cast<uint16_t>(1u << reg);
    };
    auto writeVi = [&](uint8_t reg)
    {
        if (reg != 0u)
            usage.viWrite |= static_cast<uint16_t>(1u << reg);
    };

    switch (opHi)
    {
    case 0x00:
        usage.pipeline = PipelineLsu;
        usage.latency = 4u;
        readVi(viS);
        addVfWrite(usage, vfT, dest);
        return usage;
    case 0x01:
        usage.pipeline = PipelineLsu;
        usage.latency = 1u;
        readVi(viT);
        addVfRead(usage, vfS, dest);
        return usage;
    case 0x04:
        usage.pipeline = PipelineLsu;
        usage.latency = 4u;
        readVi(viS);
        writeVi(viT);
        return usage;
    case 0x05:
        usage.pipeline = PipelineLsu;
        usage.latency = 1u;
        readVi(viS);
        readVi(viT);
        return usage;
    case 0x08:
    case 0x09:
        usage.pipeline = PipelineIalu;
        usage.latency = 1u;
        usage.delaysNextBranchRead = true;
        readVi(viS);
        writeVi(viT);
        return usage;
    case 0x10:
    case 0x12:
    case 0x13:
        usage.pipeline = PipelineIalu;
        usage.latency = 1u;
        usage.readsClip = true;
        writeVi(1u);
        return usage;
    case 0x11:
        usage.pipeline = PipelineFmac;
        usage.latency = kFmacLatency;
        usage.writesClip = true;
        return usage;
    case 0x14:
    case 0x16:
    case 0x17:
        usage.pipeline = PipelineIalu;
        usage.latency = 1u;
        writeVi(viT);
        return usage;
    case 0x15:
        usage.pipeline = PipelineFmac;
        usage.latency = kFmacLatency;
        return usage;
    case 0x18:
    case 0x1A:
    case 0x1B:
        usage.pipeline = PipelineIalu;
        usage.latency = 1u;
        readVi(viS);
        writeVi(viT);
        return usage;
    case 0x1C:
        usage.pipeline = PipelineIalu;
        usage.latency = 1u;
        usage.readsClip = true;
        writeVi(viT);
        return usage;
    case 0x20:
        usage.pipeline = PipelineBranch;
        return usage;
    case 0x21:
        usage.pipeline = PipelineBranch;
        usage.latency = 1u;
        writeVi(viT);
        return usage;
    case 0x24:
        usage.pipeline = PipelineBranch;
        readVi(viS);
        return usage;
    case 0x25:
        usage.pipeline = PipelineBranch;
        usage.latency = 1u;
        readVi(viS);
        writeVi(viT);
        return usage;
    case 0x28:
    case 0x29:
        usage.pipeline = PipelineBranch;
        readVi(viS);
        readVi(viT);
        return usage;
    case 0x2C:
    case 0x2D:
    case 0x2E:
    case 0x2F:
        usage.pipeline = PipelineBranch;
        readVi(viS);
        return usage;
    case 0x40:
        break;
    default:
        usage.reserved = true;
        return usage;
    }

    const uint8_t direct = static_cast<uint8_t>(lower & 0x3Fu);
    if (direct == 0x30u || direct == 0x31u || direct == 0x34u || direct == 0x35u)
    {
        usage.pipeline = PipelineIalu;
        usage.latency = 1u;
        usage.delaysNextBranchRead = true;
        readVi(viS);
        readVi(viT);
        writeVi(viD);
        return usage;
    }
    if (direct == 0x32u)
    {
        usage.pipeline = PipelineIalu;
        usage.latency = 1u;
        usage.delaysNextBranchRead = true;
        readVi(viS);
        writeVi(viT);
        return usage;
    }
    if (direct < 0x3Cu)
    {
        usage.reserved = true;
        return usage;
    }

    const uint8_t special = static_cast<uint8_t>((lower & 3u) | ((lower >> 4) & 0x7Cu));
    switch (special)
    {
    case 0x30:
    case 0x31:
        usage.pipeline = PipelineFmac;
        usage.latency = 4u;
        addVfRead(usage, vfS, special == 0x31u ? 0xFu : dest);
        addVfWrite(usage, vfT, dest);
        break;
    case 0x34:
    case 0x36:
        usage.pipeline = PipelineLsu;
        usage.latency = 4u;
        usage.viLatency = 1u;
        usage.delaysNextBranchRead = true;
        readVi(viS);
        writeVi(viS);
        addVfWrite(usage, vfT, dest);
        break;
    case 0x35:
    case 0x37:
        usage.pipeline = PipelineLsu;
        usage.latency = 1u;
        usage.delaysNextBranchRead = true;
        readVi(viT);
        writeVi(viT);
        addVfRead(usage, vfS, dest);
        break;
    case 0x38:
        usage.pipeline = PipelineFdiv;
        usage.latency = 7u;
        addVfRead(usage, vfS, laneForComponent((lower >> 21) & 3u));
        addVfRead(usage, vfT, laneForComponent((lower >> 23) & 3u));
        break;
    case 0x39:
        usage.pipeline = PipelineFdiv;
        usage.latency = 7u;
        addVfRead(usage, vfT, laneForComponent((lower >> 23) & 3u));
        break;
    case 0x3A:
        usage.pipeline = PipelineFdiv;
        usage.latency = 13u;
        addVfRead(usage, vfS, laneForComponent((lower >> 21) & 3u));
        addVfRead(usage, vfT, laneForComponent((lower >> 23) & 3u));
        break;
    case 0x3B:
        usage.pipeline = PipelineFdiv;
        usage.waitQ = true;
        break;
    case 0x3C:
        usage.pipeline = PipelineIalu;
        usage.latency = 1u;
        usage.delaysNextBranchRead = true;
        addVfRead(usage, vfS, laneForComponent((lower >> 21) & 3u));
        writeVi(viT);
        break;
    case 0x3D:
        usage.pipeline = PipelineFmac;
        usage.latency = 4u;
        readVi(viS);
        addVfWrite(usage, vfT, dest);
        break;
    case 0x3E:
        usage.pipeline = PipelineLsu;
        usage.latency = 4u;
        readVi(viS);
        writeVi(viT);
        break;
    case 0x3F:
        usage.pipeline = PipelineLsu;
        usage.latency = 1u;
        readVi(viS);
        readVi(viT);
        break;
    case 0x40:
    case 0x41:
        usage.pipeline = PipelineFmac;
        usage.latency = 4u;
        addVfWrite(usage, vfT, dest);
        break;
    case 0x42:
    case 0x43:
        usage.pipeline = PipelineIalu;
        usage.latency = 1u;
        addVfRead(usage, vfS, laneForComponent((lower >> 21) & 3u));
        break;
    case 0x64:
        if (m_unit == Unit::VU0)
        {
            usage.reserved = true;
            break;
        }
        usage.pipeline = PipelineFmac;
        usage.latency = 4u;
        addVfWrite(usage, vfT, dest);
        break;
    case 0x68:
    case 0x69:
        usage.pipeline = PipelineIalu;
        usage.latency = 1u;
        writeVi(viT);
        break;
    case 0x6C:
        if (m_unit == Unit::VU0)
        {
            usage.reserved = true;
            break;
        }
        usage.pipeline = PipelineXgkick;
        usage.latency = 2u;
        readVi(viS);
        break;
    case 0x70:
    case 0x71:
    case 0x72:
    case 0x73:
    case 0x74:
    case 0x75:
    case 0x76:
    case 0x77:
    case 0x78:
    case 0x79:
    case 0x7A:
    case 0x7C:
    case 0x7D:
        if (m_unit == Unit::VU0)
        {
            usage.reserved = true;
            break;
        }
        usage.pipeline = PipelineEfu;
        switch (special)
        {
        case 0x70:
            usage.latency = 11u;
            break;
        case 0x71:
        case 0x72:
        case 0x77:
            usage.latency = 18u;
            break;
        case 0x73:
            usage.latency = 24u;
            break;
        case 0x74:
        case 0x75:
        case 0x7C:
            usage.latency = 54u;
            break;
        case 0x76:
        case 0x78:
        case 0x7A:
            usage.latency = 12u;
            break;
        case 0x79:
            usage.latency = 29u;
            break;
        case 0x7D:
            usage.latency = 44u;
            break;
        default:
            break;
        }
        if (special >= 0x70u && special <= 0x73u)
            addVfRead(usage, vfS, 0xEu);
        else if (special == 0x74u)
            addVfRead(usage, vfS, 0xCu);
        else if (special == 0x75u)
            addVfRead(usage, vfS, 0xAu);
        else if (special == 0x76u)
            addVfRead(usage, vfS, 0xFu);
        else
            addVfRead(usage, vfS, laneForComponent((lower >> 21) & 3u));
        break;
    case 0x7B:
        if (m_unit == Unit::VU0)
        {
            usage.reserved = true;
            break;
        }
        usage.pipeline = PipelineEfu;
        usage.waitP = true;
        break;
    default:
        usage.reserved = true;
        break;
    }
    return usage;
}

VU1Interpreter::DecodedInstructionPair VU1Interpreter::decodeInstructionPair(const uint8_t *vuCode, uint32_t pc) const
{
    DecodedInstructionPair decoded;
    std::memcpy(&decoded.lower, vuCode + pc, sizeof(decoded.lower));
    std::memcpy(&decoded.upper, vuCode + pc + sizeof(decoded.lower), sizeof(decoded.upper));
    decoded.iBit = (decoded.upper & 0x80000000u) != 0u;
    decoded.eBit = (decoded.upper & 0x40000000u) != 0u;
    decoded.mBit = (decoded.upper & 0x20000000u) != 0u;
    decoded.dBit = (decoded.upper & 0x10000000u) != 0u;
    decoded.tBit = (decoded.upper & 0x08000000u) != 0u;
    decoded.upperUsage = decodeUpperUsage(decoded.upper);
    if (!decoded.iBit)
        decoded.lowerUsage = decodeLowerUsage(decoded.lower);

    const uint8_t upperWriteReg = decoded.upperUsage.vfWrite.reg;
    if (upperWriteReg != 0u && (vfReadLanes(decoded.lowerUsage, upperWriteReg) != 0u || decoded.lowerUsage.vfWrite.reg == upperWriteReg))
    {
        decoded.upperVfShadowReg = upperWriteReg;
        if (decoded.lowerUsage.vfWrite.reg == upperWriteReg)
            decoded.suppressedLowerVf = upperWriteReg;
    }
    return decoded;
}

void VU1Interpreter::rebuildDecodedCodeCache(const uint8_t *vuCode, uint32_t codeSize,
                                             const PS2Memory *memory, uint64_t generation)
{
    const bool profileDecode = std::getenv("PS2X_VU1_AGGREGATE_PROFILE_SECONDS") != nullptr;
    const auto decodeStart = profileDecode ? std::chrono::steady_clock::now()
                                           : std::chrono::steady_clock::time_point{};
    const uint32_t pairCount = std::min<uint32_t>(codeSize / 8u, kMaxDecodedPairs);
    for (uint32_t i = 0; i < pairCount; ++i)
        m_decodedCodeCache[i] = decodeInstructionPair(vuCode, i * 8u);

    m_cachedVuCode = vuCode;
    m_cachedMemory = memory;
    m_cachedCodeSize = codeSize;
    m_cachedCodeGeneration = generation;
    m_decodedCodeCacheValid = true;
    if (profileDecode)
    {
        g_vuRunProfileDecodeNanoseconds += static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(
                std::chrono::steady_clock::now() - decodeStart).count());
        ++g_vuRunProfileDecodeRebuilds;
    }
}

const VU1Interpreter::DecodedInstructionPair &VU1Interpreter::getDecodedInstructionPairForPc(
    const uint8_t *vuCode, uint32_t codeSize, PS2Memory *memory, uint32_t pc)
{
    if ((pc & 7u) != 0u)
    {
        m_decodedScratch = decodeInstructionPair(vuCode, pc);
        return m_decodedScratch;
    }

    const bool trackedVu1Code = memory != nullptr &&
                                ((m_unit == Unit::VU1 && vuCode == memory->getVU1Code()) ||
                                 (m_unit == Unit::VU0 && vuCode == memory->getVU0Code()));
    if (!trackedVu1Code)
    {
        m_decodedScratch = decodeInstructionPair(vuCode, pc);
        return m_decodedScratch;
    }

    const uint64_t generation = m_unit == Unit::VU1 ? memory->getVU1CodeGeneration() : memory->getVU0CodeGeneration();
    if (!m_decodedCodeCacheValid ||
        m_cachedVuCode != vuCode ||
        m_cachedMemory != memory ||
        m_cachedCodeSize != codeSize ||
        m_cachedCodeGeneration != generation)
    {
        rebuildDecodedCodeCache(vuCode, codeSize, memory, generation);
    }
    const uint32_t pairIndex = pc / 8u;
    if (pairIndex >= kMaxDecodedPairs)
    {
        m_decodedScratch = decodeInstructionPair(vuCode, pc);
        return m_decodedScratch;
    }
    return m_decodedCodeCache[pairIndex];
}

void VU1Interpreter::reportReservedInstruction(bool upper, uint32_t instruction)
{
    RUNTIME_ERROR(
        "[VU" << (m_unit == Unit::VU1 ? "1" : "0")
              << " reserved " << (upper ? "upper" : "lower")
              << "] cycle=" << m_cycle
              << " pc=0x" << std::hex << m_state.pc
              << " instruction=0x" << instruction
              << std::dec << '\n');
    m_stopRequested = true;
}

void VU1Interpreter::execute(uint8_t *vuCode, uint32_t codeSize,
                             uint8_t *vuData, uint32_t dataSize,
                             GS &gs, PS2Memory *memory,
                             uint32_t startPC, uint32_t top, uint32_t itop,
                             uint32_t maxCycles)
{
    resetScheduler();
    m_state.pc = startPC & microAddressMask();
    m_state.ebit = false;
    m_state.haltAfterDelaySlot = false;
    m_state.stoppedByD = false;
    m_state.stoppedByT = false;
    m_state.top = top;
    m_state.itop = itop;
    m_state.branchPending = false;
    m_state.branchTarget = 0;
    m_state.branchDelay = 0;
    m_state.vf[0][0] = 0.0f;
    m_state.vf[0][1] = 0.0f;
    m_state.vf[0][2] = 0.0f;
    m_state.vf[0][3] = 1.0f;
    run(vuCode, codeSize, vuData, dataSize, gs, memory, maxCycles);
}

void VU1Interpreter::resume(uint8_t *vuCode, uint32_t codeSize,
                            uint8_t *vuData, uint32_t dataSize,
                            GS &gs, PS2Memory *memory,
                            uint32_t top, uint32_t itop, uint32_t maxCycles)
{
    m_state.top = top;
    m_state.itop = itop;
    m_state.stoppedByD = false;
    m_state.stoppedByT = false;
    run(vuCode, codeSize, vuData, dataSize, gs, memory, maxCycles);
}

void VU1Interpreter::run(uint8_t *vuCode, uint32_t codeSize,
                         uint8_t *vuData, uint32_t dataSize,
                         GS &gs, PS2Memory *memory, uint32_t maxCycles)
{
    using VuProfileClock = std::chrono::steady_clock;
    struct VuAggregateStat
    {
        uint64_t calls{0u};
        uint64_t cycles{0u};
        uint64_t nanoseconds{0u};
        uint64_t xgkickNanoseconds{0u};
        uint64_t xgkickPackets{0u};
        uint64_t decodeNanoseconds{0u};
        uint64_t decodeRebuilds{0u};
        uint64_t phaseSamples{0u};
        uint64_t phasePreNanoseconds{0u};
        uint64_t phaseExecuteNanoseconds{0u};
        uint64_t phasePostNanoseconds{0u};
        std::array<uint64_t, 256> upperOpcodeCounts{};
        std::array<uint64_t, 5> upperDestinationLaneCounts{};
        std::unordered_map<uint64_t, uint64_t> entryStateCounts;
    };
    struct VuAggregateProfile
    {
        bool enabled{false};
        bool printed{false};
        VuProfileClock::time_point start{};
        VuProfileClock::time_point deadline{};
        std::unordered_map<uint32_t, VuAggregateStat> stats;
    };
    static VuAggregateProfile aggregateProfile = [] {
        VuAggregateProfile result;
        const char *durationText = std::getenv("PS2X_VU1_AGGREGATE_PROFILE_SECONDS");
        if (!durationText || durationText[0] == '\0')
            return result;
        char *durationEnd = nullptr;
        const double duration = std::strtod(durationText, &durationEnd);
        if (durationEnd == durationText || *durationEnd != '\0' || duration <= 0.0)
            return result;
        double delay = 0.0;
        if (const char *delayText = std::getenv("PS2X_VU1_AGGREGATE_PROFILE_DELAY_SECONDS"))
        {
            char *delayEnd = nullptr;
            const double parsed = std::strtod(delayText, &delayEnd);
            if (delayEnd != delayText && *delayEnd == '\0' && parsed > 0.0)
                delay = parsed;
        }
        result.enabled = true;
        result.start = VuProfileClock::now() +
                       std::chrono::duration_cast<VuProfileClock::duration>(
                           std::chrono::duration<double>(delay));
        result.deadline = result.start +
                          std::chrono::duration_cast<VuProfileClock::duration>(
                              std::chrono::duration<double>(duration));
        result.stats.reserve(64u);
        return result;
    }();
    const auto aggregateNow = VuProfileClock::now();
    const bool aggregateRun = aggregateProfile.enabled &&
                              aggregateNow >= aggregateProfile.start &&
                              aggregateNow < aggregateProfile.deadline;
    const bool legacyProfileRun = std::getenv("PS2X_VU1_RUN_PROFILE") != nullptr;
    const bool profileRun = legacyProfileRun || aggregateRun;
    const auto profileStart = profileRun ? std::chrono::steady_clock::now()
                                         : std::chrono::steady_clock::time_point{};
    const uint64_t profileStartCycle = m_cycle;
    const uint32_t profileStartPc = m_state.pc;
    std::array<uint64_t, 256> profileUpperOpcodeCounts{};
    std::array<uint64_t, 5> profileUpperDestinationLaneCounts{};
    uint64_t profilePhaseSamples = 0u;
    uint64_t profilePhasePreNanoseconds = 0u;
    uint64_t profilePhaseExecuteNanoseconds = 0u;
    uint64_t profilePhasePostNanoseconds = 0u;
    uint64_t profileEntryStateHash = 0u;
    if (profileRun)
    {
        g_vuRunProfileXgkickNanoseconds = 0u;
        g_vuRunProfileXgkickPackets = 0u;
        g_vuRunProfileDecodeNanoseconds = 0u;
        g_vuRunProfileDecodeRebuilds = 0u;
    }
    m_activeVuData = vuData;
    m_activeVuDataSize = dataSize;
    m_activeGs = &gs;
    m_activeMemory = memory;

    if (aggregateRun && (profileStartPc == 0x288u || profileStartPc == 0x60u))
    {
        uint64_t hash = 1469598103934665603ull;
        const auto hashValue = [&hash](uint64_t value)
        {
            for (uint32_t byte = 0u; byte < 8u; ++byte)
            {
                hash ^= static_cast<uint8_t>(value >> (byte * 8u));
                hash *= 1099511628211ull;
            }
        };
        const auto relativeCycle = [this](uint64_t cycle)
        {
            return cycle > m_cycle ? cycle - m_cycle : 0u;
        };
        hashValue(m_flagPipelineFreeMask);
        hashValue(m_efuFreeMask);
        hashValue(m_storePipelineFreeMask);
        hashValue(m_vfWritePipelineFreeMask);
        hashValue(m_viWritePipelineFreeMask);
        hashValue(m_accWritePipelineFreeMask);
        hashValue(m_fdiv.valid ? relativeCycle(m_fdiv.readyCycle) : 0u);
        hashValue(m_xgkick.active ? 1u : 0u);
        hashValue(m_xgkick.cycleCredit);
        hashValue(m_xgkick.copiedBytes);
        hashValue(m_xgkick.currentTagEnd);
        for (const auto &ready : m_vfReady)
            for (uint64_t cycle : ready)
                hashValue(relativeCycle(cycle));
        for (uint64_t cycle : m_viReady)
            hashValue(relativeCycle(cycle));
        for (uint64_t cycle : m_accReady)
            hashValue(relativeCycle(cycle));
        profileEntryStateHash = hash;
    }

    if (m_unit == Unit::VU1)
    {
        const uint64_t generation = memory != nullptr
                                        ? memory->getVU1CodeGeneration()
                                        : 0u;
        captureVuProgramIfRequested(vuCode, codeSize, m_state.pc, generation);
    }

    const bool trackedRunCode = memory != nullptr &&
                                ((m_unit == Unit::VU1 && vuCode == memory->getVU1Code()) ||
                                 (m_unit == Unit::VU0 && vuCode == memory->getVU0Code()));
    const DecodedInstructionPair *runDecodedCode = nullptr;
    uint32_t runDecodedPairCount = 0u;
    if (trackedRunCode)
    {
        const uint64_t generation = m_unit == Unit::VU1
                                        ? memory->getVU1CodeGeneration()
                                        : memory->getVU0CodeGeneration();
        if (!m_decodedCodeCacheValid ||
            m_cachedVuCode != vuCode ||
            m_cachedMemory != memory ||
            m_cachedCodeSize != codeSize ||
            m_cachedCodeGeneration != generation)
        {
            rebuildDecodedCodeCache(vuCode, codeSize, memory, generation);
        }
        runDecodedCode = m_decodedCodeCache.data();
        runDecodedPairCount = std::min<uint32_t>(codeSize / 8u, kMaxDecodedPairs);
    }

    static const bool enableCapturedHotBlock =
        std::getenv("PS2X_VU1_CAPTURED_HOT_BLOCK") != nullptr;
    static const bool capturedScheduleProfileEnabled =
        std::getenv("PS2X_VU1_CAPTURED_SCHEDULE_PROFILE") != nullptr;
    uint64_t capturedCodeHash = 0u;
    if ((enableCapturedHotBlock || capturedScheduleProfileEnabled) &&
        trackedRunCode && m_unit == Unit::VU1)
    {
        thread_local const uint8_t *hashedCode = nullptr;
        thread_local uint32_t hashedCodeSize = 0u;
        thread_local uint64_t hashedGeneration = ~0ull;
        thread_local uint64_t codeHash = 0u;
        const uint64_t generation = memory->getVU1CodeGeneration();
        if (hashedCode != vuCode || hashedCodeSize != codeSize ||
            hashedGeneration != generation)
        {
            codeHash = 1469598103934665603ull;
            for (uint32_t index = 0u; index < codeSize; ++index)
            {
                codeHash ^= vuCode[index];
                codeHash *= 1099511628211ull;
            }
            hashedCode = vuCode;
            hashedCodeSize = codeSize;
            hashedGeneration = generation;
        }
        capturedCodeHash = !g_disableCapturedHotBlock && !g_capturedHotBlockRejected
                               ? codeHash
                               : 0u;
    }

    const int previousRoundingMode = std::fegetround();
    const bool changeVuRounding = previousRoundingMode != FE_TOWARDZERO;
    const bool useVuRounding = !changeVuRounding ||
                               std::fesetround(FE_TOWARDZERO) == 0;
    const uint64_t budgetEnd = m_cycle + maxCycles;
    bool programEnded = false;
    const auto executeCapturedUpper = [&]<typename UpperTag>(UpperTag)
    {
        constexpr uint32_t instr = UpperTag::value;
        m_currentUpperInstruction = instr;
        if constexpr (instr == 0x000002ffu)
        {
            return;
        }
        else if constexpr (instr == 0x01cb59ffu)
        {
            constexpr uint8_t fs = FS(instr);
            constexpr uint8_t ft = FT(instr);
            uint32_t wBits = 0u;
            std::memcpy(&wBits, &m_state.vf[ft][3], sizeof(wBits));
            const int32_t limit = (wBits & 0x7F800000u) != 0u
                                      ? static_cast<int32_t>(wBits & 0x7FFFFFFFu)
                                      : 0x007FFFFF;
            const auto exceeds = [limit](float value, uint32_t signMask)
            {
                uint32_t bits = 0u;
                std::memcpy(&bits, &value, sizeof(bits));
                bits ^= signMask;
                int32_t ordered = 0;
                std::memcpy(&ordered, &bits, sizeof(ordered));
                return ordered > limit;
            };
            uint32_t flags = 0u;
            flags |= exceeds(m_state.vf[fs][0], 0u) ? 0x01u : 0u;
            flags |= exceeds(m_state.vf[fs][0], 0x80000000u) ? 0x02u : 0u;
            flags |= exceeds(m_state.vf[fs][1], 0u) ? 0x04u : 0u;
            flags |= exceeds(m_state.vf[fs][1], 0x80000000u) ? 0x08u : 0u;
            flags |= exceeds(m_state.vf[fs][2], 0u) ? 0x10u : 0u;
            flags |= exceeds(m_state.vf[fs][2], 0x80000000u) ? 0x20u : 0u;
            queueClip(flags);
        }
        else if constexpr (instr == 0x01c04a9cu || instr == 0x01c06b5cu)
        {
            constexpr uint8_t dest = DEST(instr);
            constexpr uint8_t fs = FS(instr);
            constexpr uint8_t fd = FD(instr);
            const float q = normalizeOperand(m_state.q);
            float result[4]{};
            for (uint32_t c = 0u; c < 4u; ++c)
                result[c] = normalizeOperand(m_state.vf[fs][c]) * q;
            applyFmacDest(m_state.vf[fd], result, dest);
        }
        else if constexpr (instr == 0x01eb09bcu || instr == 0x01eb29bcu)
        {
            constexpr uint8_t dest = DEST(instr);
            constexpr uint8_t fs = FS(instr);
            constexpr uint8_t ft = FT(instr);
            const float bc = normalizeOperand(m_state.vf[ft][0]);
            float result[4]{};
            for (uint32_t c = 0u; c < 4u; ++c)
                result[c] = normalizeOperand(m_state.vf[fs][c]) * bc;
            applyFmacDestAcc(result, dest);
        }
        else if constexpr (instr == 0x01eb10bdu || instr == 0x01eb30bdu ||
                           instr == 0x01eb18beu || instr == 0x01eb38beu)
        {
            constexpr uint8_t dest = DEST(instr);
            constexpr uint8_t fs = FS(instr);
            constexpr uint8_t ft = FT(instr);
            constexpr uint8_t bcLane =
                instr == 0x01eb10bdu || instr == 0x01eb30bdu ? 1u : 2u;
            const float bc = normalizeOperand(m_state.vf[ft][bcLane]);
            float result[4]{};
            for (uint32_t c = 0u; c < 4u; ++c)
                result[c] = normalizeOperand(m_state.acc[c]) +
                            normalizeOperand(m_state.vf[fs][c]) * bc;
            applyFmacDestAcc(result, dest);
        }
        else if constexpr (instr == 0x01e0224bu || instr == 0x01e042cbu)
        {
            constexpr uint8_t dest = DEST(instr);
            constexpr uint8_t fs = FS(instr);
            constexpr uint8_t ft = FT(instr);
            constexpr uint8_t fd = FD(instr);
            const float bc = normalizeOperand(m_state.vf[ft][3]);
            float result[4]{};
            for (uint32_t c = 0u; c < 4u; ++c)
                result[c] = normalizeOperand(m_state.acc[c]) +
                            normalizeOperand(m_state.vf[fs][c]) * bc;
            applyFmacDest(m_state.vf[fd], result, dest);
        }
        else if constexpr (instr == 0x018a517du || instr == 0x004a517cu)
        {
            constexpr uint8_t dest = DEST(instr);
            constexpr uint8_t fs = FS(instr);
            constexpr uint8_t ft = FT(instr);
            constexpr float scale = instr == 0x018a517du ? 16.0f : 1.0f;
            float result[4]{};
            for (uint32_t c = 0u; c < 4u; ++c)
            {
                const double scaled = static_cast<double>(normalizeOperand(m_state.vf[fs][c])) * scale;
                int32_t value = 0;
                if (scaled >= static_cast<double>(std::numeric_limits<int32_t>::max()))
                    value = std::numeric_limits<int32_t>::max();
                else if (scaled <= static_cast<double>(std::numeric_limits<int32_t>::min()))
                    value = std::numeric_limits<int32_t>::min();
                else
                    value = static_cast<int32_t>(scaled);
                std::memcpy(&result[c], &value, sizeof(value));
            }
            applyDest(m_state.vf[ft], result, dest);
        }
        else
        {
            execUpper(instr);
        }
    };
    const auto executeCapturedLower = [&]<typename LowerTag, typename UpperTag>(
        LowerTag, UpperTag)
    {
        constexpr uint32_t instr = LowerTag::value;
        if constexpr (instr == 0x8000033cu || instr == 0u)
        {
            return;
        }
        else if constexpr (((instr >> 25) & 0x7Fu) == 0x00u)
        {
            constexpr uint8_t it = FT(instr);
            constexpr uint8_t is = VIS(instr);
            constexpr uint8_t dest = (instr >> 21) & 0xFu;
            constexpr int16_t imm = IMM11(instr);
            uint32_t address = static_cast<uint32_t>(m_state.vi[is] + imm) * 16u;
            address &= dataSize - 1u;
            if (address + 16u <= dataSize)
            {
                float value[4]{};
                std::memcpy(value, vuData + address, sizeof(value));
                applyDest(m_state.vf[it], value, dest);
            }
        }
        else if constexpr (((instr >> 25) & 0x7Fu) == 0x01u)
        {
            constexpr uint8_t is = FS(instr);
            constexpr uint8_t it = VIT(instr);
            constexpr uint8_t dest = (instr >> 21) & 0xFu;
            constexpr int16_t imm = IMM11(instr);
            uint32_t address = static_cast<uint32_t>(m_state.vi[it] + imm) * 16u;
            address &= dataSize - 1u;
            if (address + 16u <= dataSize)
            {
                uint32_t words[4]{};
                std::memcpy(words, m_state.vf[is], sizeof(words));
                queueStore(address, words, dest);
            }
        }
        else if constexpr (((instr >> 25) & 0x7Fu) == 0x05u)
        {
            constexpr uint8_t it = VIT(instr);
            constexpr uint8_t is = VIS(instr);
            constexpr uint8_t dest = (instr >> 21) & 0xFu;
            constexpr int16_t imm = IMM11(instr);
            uint32_t address = static_cast<uint32_t>(m_state.vi[is] + imm) * 16u;
            address &= dataSize - 1u;
            if (address + 16u <= dataSize)
            {
                const uint32_t value = static_cast<uint16_t>(m_state.vi[it]);
                const uint32_t words[4] = {value, value, value, value};
                queueStore(address, words, dest);
            }
        }
        else if constexpr (((instr >> 25) & 0x7Fu) == 0x09u)
        {
            constexpr uint8_t it = VIT(instr);
            constexpr uint8_t is = VIS(instr);
            constexpr int16_t imm = static_cast<int16_t>(instr & 0x7FFu) |
                                    static_cast<int16_t>((instr >> 10) & 0x7800u);
            if constexpr (it != 0u)
                m_state.vi[it] = static_cast<int16_t>(m_state.vi[is] - imm);
        }
        else if constexpr (((instr >> 25) & 0x7Fu) == 0x12u)
        {
            m_state.vi[1] = (m_state.clip & (instr & 0xFFFFFFu)) != 0u ? 1 : 0;
        }
        else if constexpr (((instr >> 25) & 0x7Fu) == 0x29u)
        {
            constexpr uint8_t it = VIT(instr);
            constexpr uint8_t is = VIS(instr);
            constexpr int16_t imm = IMM11(instr);
            if (static_cast<int16_t>(readBranchVi(is)) !=
                static_cast<int16_t>(readBranchVi(it)))
            {
                m_state.branchPending = true;
                m_state.branchTarget =
                    (m_state.pc + 8u + static_cast<int32_t>(imm) * 8u) & microAddressMask();
                m_state.branchDelay = 1u;
            }
        }
        else if constexpr (((instr >> 25) & 0x7Fu) == 0x40u &&
                           (instr & 0x3Fu) == 0x30u)
        {
            constexpr uint8_t it = VIT(instr);
            constexpr uint8_t is = VIS(instr);
            constexpr uint8_t id = VID(instr);
            if constexpr (id != 0u)
                m_state.vi[id] = static_cast<int16_t>(m_state.vi[is] + m_state.vi[it]);
        }
        else if constexpr (((instr >> 25) & 0x7Fu) == 0x40u &&
                           (instr & 0x3Fu) == 0x32u)
        {
            constexpr uint8_t it = VIT(instr);
            constexpr uint8_t is = VIS(instr);
            constexpr int16_t imm = static_cast<int16_t>(
                static_cast<int32_t>((instr >> 6) & 0x1Fu) << 27 >> 27);
            if constexpr (it != 0u)
                m_state.vi[it] = static_cast<int16_t>(m_state.vi[is] + imm);
        }
        else if constexpr (((instr >> 25) & 0x7Fu) == 0x40u &&
                           ((instr & 0x3u) | ((instr >> 4) & 0x7Cu)) == 0x3Cu)
        {
            constexpr uint8_t vfS = FS(instr);
            constexpr uint8_t viT = VIT(instr);
            constexpr uint32_t component = (instr >> 21) & 0x3u;
            uint32_t bits = 0u;
            std::memcpy(&bits, &m_state.vf[vfS][component], sizeof(bits));
            if constexpr (viT != 0u)
                m_state.vi[viT] = static_cast<int16_t>(bits & 0xFFFFu);
        }
        else if constexpr (((instr >> 25) & 0x7Fu) == 0x40u &&
                           ((instr & 0x3u) | ((instr >> 4) & 0x7Cu)) == 0x38u)
        {
            constexpr uint8_t vfS = FS(instr);
            constexpr uint8_t vfT = FT(instr);
            constexpr uint32_t fsf = (instr >> 21) & 0x3u;
            constexpr uint32_t ftf = (instr >> 23) & 0x3u;
            const float numerator = normalizeOperand(m_state.vf[vfS][fsf]);
            const float denominator = normalizeOperand(m_state.vf[vfT][ftf]);
            uint32_t statusDi = 0u;
            float value = 0.0f;
            if (denominator == 0.0f)
            {
                statusDi = numerator == 0.0f ? 0x10u : 0x20u;
                value = std::signbit(numerator) != std::signbit(denominator)
                            ? -std::numeric_limits<float>::max()
                            : std::numeric_limits<float>::max();
            }
            else
            {
                value = numerator / denominator;
            }
            uint32_t ignoredFlags = 0u;
            value = normalizeResult(value, ignoredFlags);
            queueQ(value, 7u, statusDi);
        }
        else
        {
            execLower(instr, vuData, dataSize, gs, memory, UpperTag::value);
        }
    };
    const auto executeCapturedPair = [&]<typename LowerTag, typename UpperTag,
                                         typename ScheduleDeltaTag>(
        uint32_t pairIndex, LowerTag, UpperTag, ScheduleDeltaTag)
    {
        constexpr uint32_t lower = LowerTag::value;
        constexpr uint32_t upper = UpperTag::value;
        const DecodedInstructionPair &decoded = runDecodedCode[pairIndex];
        if constexpr (ScheduleDeltaTag::value != 0u)
        {
            constexpr uint64_t scheduledDelta = ScheduleDeltaTag::value;
            static_assert(scheduledDelta >= 1u);
            if (m_cycle + scheduledDelta > budgetEnd)
            {
                advanceTo(budgetEnd);
                return false;
            }
            if constexpr (scheduledDelta > 1u)
                advanceTo(m_cycle + scheduledDelta - 1u);
        }
        else
        {
            uint64_t readyCycle = calculatePairReadyCycle(decoded);
            while (readyCycle > m_cycle)
            {
                if (readyCycle >= budgetEnd)
                {
                    advanceTo(budgetEnd);
                    return false;
                }
                advanceTo(readyCycle);
                readyCycle = calculatePairReadyCycle(decoded);
            }
        }
        if (m_cycle >= budgetEnd)
            return false;

        if (aggregateRun)
        {
            const uint8_t op = static_cast<uint8_t>(upper & 0x3Fu);
            const uint8_t key = op >= 0x3Cu
                                    ? static_cast<uint8_t>(128u +
                                        ((upper & 0x3u) | ((upper >> 4) & 0x7Cu)))
                                    : op;
            ++profileUpperOpcodeCounts[key];
            ++profileUpperDestinationLaneCounts[std::popcount(
                static_cast<unsigned int>(DEST(upper)))];
        }

        constexpr uint8_t lowerOp = static_cast<uint8_t>((lower >> 25) & 0x7Fu);
        constexpr uint8_t lowerDirect = static_cast<uint8_t>(lower & 0x3Fu);
        constexpr uint8_t lowerSpecial = static_cast<uint8_t>(
            (lower & 0x3u) | ((lower >> 4) & 0x7Cu));
        constexpr uint8_t upperOp = static_cast<uint8_t>(upper & 0x3Fu);
        constexpr uint8_t upperSpecial = static_cast<uint8_t>(
            (upper & 0x3u) | ((upper >> 4) & 0x7Cu));
        constexpr bool upperWritesAcc = upperOp >= 0x3Cu &&
            (upperSpecial <= 0x0Fu ||
             (upperSpecial >= 0x18u && upperSpecial <= 0x1Cu) ||
             upperSpecial == 0x1Eu ||
             (upperSpecial >= 0x20u && upperSpecial <= 0x2Au) ||
             (upperSpecial >= 0x2Cu && upperSpecial <= 0x2Eu));
        constexpr bool upperWritesVf = upperOp <= 0x2Fu ||
            (upperOp >= 0x3Cu &&
             ((upperSpecial >= 0x10u && upperSpecial <= 0x17u) ||
              upperSpecial == 0x1Du));
        constexpr uint8_t upperWriteReg = upperWritesVf
                                               ? (upperOp <= 0x2Fu
                                                      ? FD(upper)
                                                      : FT(upper))
                                               : 0u;
        constexpr uint8_t upperWriteLanes = upperWritesVf ? DEST(upper) : 0u;
        constexpr uint8_t upperAccLanes = upperWritesAcc ? DEST(upper) : 0u;
        constexpr uint8_t lowerWriteReg = lowerOp == 0x00u ? FT(lower) : 0u;
        constexpr uint8_t lowerWriteLanes = lowerOp == 0x00u ? DEST(lower) : 0u;
        constexpr uint8_t writtenVi = (lowerOp == 0x08u || lowerOp == 0x09u)
                                          ? VIT(lower)
                                          : lowerOp == 0x12u
                                                ? 1u
                                                : lowerOp == 0x40u && lowerDirect == 0x30u
                                                      ? VID(lower)
                                                      : lowerOp == 0x40u && lowerDirect == 0x32u
                                                            ? VIT(lower)
                                                            : lowerOp == 0x40u && lowerSpecial == 0x3Cu
                                                                  ? VIT(lower)
                                                                  : lowerOp == 0x40u &&
                                                                            (lowerSpecial == 0x35u ||
                                                                             lowerSpecial == 0x37u ||
                                                                             lowerSpecial == 0x68u ||
                                                                             lowerSpecial == 0x69u)
                                                                        ? VIT(lower)
                                                                  : 0u;
        constexpr bool delaysNextBranchRead =
            lowerOp == 0x08u || lowerOp == 0x09u ||
            (lowerOp == 0x40u &&
             (lowerDirect == 0x30u || lowerDirect == 0x32u ||
              lowerSpecial == 0x3Cu || lowerSpecial == 0x35u ||
              lowerSpecial == 0x37u));
        constexpr bool lowerReadsUpperWrite = upperWriteReg != 0u &&
            ((lowerOp == 0x01u && FS(lower) == upperWriteReg) ||
             (lowerOp == 0x40u && lowerSpecial == 0x3Cu && FS(lower) == upperWriteReg) ||
             (lowerOp == 0x40u && lowerSpecial == 0x38u &&
              (FS(lower) == upperWriteReg || FT(lower) == upperWriteReg)));
        static_assert(upperWriteReg == 0u || lowerWriteReg == 0u ||
                      upperWriteReg != lowerWriteReg);
        const int32_t oldVi = writtenVi != 0u ? m_state.vi[writtenVi] : 0;
        float oldUpperVf[4]{};
        float newUpperVf[4]{};
        float oldLowerVf[4]{};
        float newLowerVf[4]{};
        float oldAcc[4]{};
        float newAcc[4]{};
        if constexpr (upperWriteReg != 0u)
            std::memcpy(oldUpperVf, m_state.vf[upperWriteReg], sizeof(oldUpperVf));
        if constexpr (lowerWriteReg != 0u)
            std::memcpy(oldLowerVf, m_state.vf[lowerWriteReg], sizeof(oldLowerVf));
        if constexpr (upperAccLanes != 0u)
            std::memcpy(oldAcc, m_state.acc, sizeof(oldAcc));

        if constexpr (lowerReadsUpperWrite)
        {
            float upperResult[4]{};
            executeCapturedUpper(UpperTag{});
            std::memcpy(upperResult, m_state.vf[upperWriteReg], sizeof(upperResult));
            std::memcpy(m_state.vf[upperWriteReg], oldUpperVf, sizeof(oldUpperVf));
            executeCapturedLower(LowerTag{}, UpperTag{});
            std::memcpy(m_state.vf[upperWriteReg], upperResult, sizeof(upperResult));
        }
        else
        {
            executeCapturedUpper(UpperTag{});
            executeCapturedLower(LowerTag{}, UpperTag{});
        }

        m_viBranchBackupValid = false;
        if constexpr (upperWriteReg != 0u)
        {
            std::memcpy(newUpperVf, m_state.vf[upperWriteReg], sizeof(newUpperVf));
            std::memcpy(m_state.vf[upperWriteReg], oldUpperVf, sizeof(oldUpperVf));
            queueVfWrite(upperWriteReg, upperWriteLanes, newUpperVf, kFmacLatency);
        }
        if constexpr (lowerWriteReg != 0u)
        {
            std::memcpy(newLowerVf, m_state.vf[lowerWriteReg], sizeof(newLowerVf));
            std::memcpy(m_state.vf[lowerWriteReg], oldLowerVf, sizeof(oldLowerVf));
            queueVfWrite(lowerWriteReg, lowerWriteLanes, newLowerVf, 4u);
        }
        if constexpr (upperAccLanes != 0u)
        {
            std::memcpy(newAcc, m_state.acc, sizeof(newAcc));
            std::memcpy(m_state.acc, oldAcc, sizeof(oldAcc));
            queueAccWrite(upperAccLanes, newAcc, kAccForwardLatency);
        }
        if constexpr (writtenVi != 0u)
        {
            const int32_t newVi = m_state.vi[writtenVi];
            m_state.vi[writtenVi] = oldVi;
            queueViWrite(writtenVi, newVi, 1u);
        }

        if constexpr (lowerWriteReg != 0u)
        {
            for (uint32_t component = 0u; component < 4u; ++component)
                if ((lowerWriteLanes & laneForComponent(component)) != 0u)
                    m_vfReady[lowerWriteReg][component] = m_cycle + 4u;
        }
        if constexpr (upperWriteReg != 0u)
        {
            for (uint32_t component = 0u; component < 4u; ++component)
                if ((upperWriteLanes & laneForComponent(component)) != 0u)
                    m_vfReady[upperWriteReg][component] = m_cycle + kFmacLatency;
        }
        if constexpr (writtenVi != 0u)
            m_viReady[writtenVi] = m_cycle + 1u;
        if constexpr (upperAccLanes != 0u)
        {
            for (uint32_t component = 0u; component < 4u; ++component)
                if ((upperAccLanes & laneForComponent(component)) != 0u)
                    m_accReady[component] = m_cycle + kAccForwardLatency;
        }
        if constexpr (writtenVi != 0u && delaysNextBranchRead)
            recordViWriteForBranch(writtenVi, oldVi);
        m_state.vf[0][0] = 0.0f;
        m_state.vf[0][1] = 0.0f;
        m_state.vf[0][2] = 0.0f;
        m_state.vf[0][3] = 1.0f;
        m_state.vi[0] = 0;

        uint32_t nextPc = m_state.pc + 8u;
        if (nextPc >= codeSize)
            nextPc = 0u;
        m_state.pc = nextPc;
        if (m_state.branchPending)
        {
            if (m_state.branchDelay == 0u)
            {
                m_state.pc = m_state.branchTarget & microAddressMask();
                m_state.branchPending = false;
            }
            else
            {
                --m_state.branchDelay;
            }
        }
        advanceOneCycle();
        return !m_stopRequested;
    };
    while (m_cycle < budgetEnd && !m_stopRequested)
    {
        static const bool generatedDifferentialEnabled =
            std::getenv("PS2X_VU1_AOT_DIFFERENTIAL") != nullptr;
        const bool checkGeneratedBlock = generatedDifferentialEnabled &&
                                         !g_capturedHotBlockRejected &&
                                         enableCapturedHotBlock &&
                                         capturedCodeHash == 0xa198bebb3e5909d9ull &&
                                         (m_state.pc == 0x060u ||
                                          m_state.pc == 0x0e8u ||
                                          m_state.pc == 0x158u ||
                                          m_state.pc == 0x1d0u ||
                                          m_state.pc == 0x220u ||
                                          m_state.pc == 0x288u);
        std::unique_ptr<VU1Interpreter> generatedOracleStorage;
        if (checkGeneratedBlock)
            generatedOracleStorage = std::make_unique<VU1Interpreter>(*this);
        VU1Interpreter &generatedOracle = checkGeneratedBlock
                                              ? *generatedOracleStorage
                                              : *this;
        std::vector<uint8_t> generatedOracleData;
        if (checkGeneratedBlock)
        {
            generatedOracleData.assign(vuData, vuData + dataSize);
        }
        bool executedGeneratedBlock = false;
#if __has_include("ps2_vu1_aot_blocks.inc")
#include "ps2_vu1_aot_blocks.inc"
#endif
        if (executedGeneratedBlock && checkGeneratedBlock)
        {
            const uint64_t generatedCycles = m_cycle - generatedOracle.m_cycle;
            const bool previousDisable = g_disableCapturedHotBlock;
            const bool previousSuppress = g_suppressVuExternalGif;
            g_disableCapturedHotBlock = true;
            g_suppressVuExternalGif = true;
            generatedOracle.run(vuCode, codeSize, generatedOracleData.data(), dataSize,
                                gs, memory, static_cast<uint32_t>(generatedCycles));
            g_suppressVuExternalGif = previousSuppress;
            g_disableCapturedHotBlock = previousDisable;
        }
        if (executedGeneratedBlock && checkGeneratedBlock)
        {
            const bool architecturalMatch =
                std::memcmp(m_state.vf, generatedOracle.m_state.vf, sizeof(m_state.vf)) == 0 &&
                std::memcmp(m_state.vi, generatedOracle.m_state.vi, sizeof(m_state.vi)) == 0 &&
                std::memcmp(m_state.acc, generatedOracle.m_state.acc, sizeof(m_state.acc)) == 0 &&
                std::memcmp(&m_state.q, &generatedOracle.m_state.q, sizeof(m_state.q)) == 0 &&
                std::memcmp(&m_state.p, &generatedOracle.m_state.p, sizeof(m_state.p)) == 0 &&
                m_state.i == generatedOracle.m_state.i &&
                m_state.r == generatedOracle.m_state.r &&
                m_state.pc == generatedOracle.m_state.pc &&
                m_state.mac == generatedOracle.m_state.mac &&
                m_state.clip == generatedOracle.m_state.clip &&
                m_state.status == generatedOracle.m_state.status &&
                m_state.ebit == generatedOracle.m_state.ebit &&
                m_state.haltAfterDelaySlot == generatedOracle.m_state.haltAfterDelaySlot &&
                m_state.dBitEnabled == generatedOracle.m_state.dBitEnabled &&
                m_state.tBitEnabled == generatedOracle.m_state.tBitEnabled &&
                m_state.stoppedByD == generatedOracle.m_state.stoppedByD &&
                m_state.stoppedByT == generatedOracle.m_state.stoppedByT &&
                m_state.top == generatedOracle.m_state.top &&
                m_state.itop == generatedOracle.m_state.itop &&
                m_state.branchPending == generatedOracle.m_state.branchPending &&
                m_state.branchTarget == generatedOracle.m_state.branchTarget &&
                m_state.branchDelay == generatedOracle.m_state.branchDelay;
            bool flagEntriesMatch = true;
            for (uint32_t index = 0u; index < m_flagPipeline.size(); ++index)
            {
                if ((m_flagPipelineFreeMask & (1u << index)) != 0u)
                    continue;
                const auto &left = m_flagPipeline[index];
                const auto &right = generatedOracle.m_flagPipeline[index];
                flagEntriesMatch &= left.readyCycle == right.readyCycle &&
                                    left.issueCycle == right.issueCycle &&
                                    left.mac == right.mac && left.status == right.status &&
                                    left.extraSticky == right.extraSticky &&
                                    left.clip == right.clip && left.valid == right.valid &&
                                    left.writesMac == right.writesMac &&
                                    left.writesStatus == right.writesStatus &&
                                    left.writesSticky == right.writesSticky &&
                                    left.writesClip == right.writesClip;
            }
            const auto scalarEntryMatch = [](const ScalarPipelineEntry &left,
                                             const ScalarPipelineEntry &right)
            {
                return left.valid == right.valid &&
                       (!left.valid ||
                        (left.readyCycle == right.readyCycle &&
                         std::bit_cast<uint32_t>(left.value) ==
                             std::bit_cast<uint32_t>(right.value) &&
                         left.statusDi == right.statusDi));
            };
            const bool fdivMatch = scalarEntryMatch(m_fdiv, generatedOracle.m_fdiv);
            bool efuEntriesMatch = true;
            for (uint32_t index = 0u; index < m_efu.size(); ++index)
                if ((m_efuFreeMask & (1u << index)) == 0u)
                    efuEntriesMatch &= scalarEntryMatch(m_efu[index], generatedOracle.m_efu[index]);
            bool storeEntriesMatch = true;
            for (uint32_t index = 0u; index < m_storePipeline.size(); ++index)
            {
                if ((m_storePipelineFreeMask & (1u << index)) != 0u)
                    continue;
                const auto &left = m_storePipeline[index];
                const auto &right = generatedOracle.m_storePipeline[index];
                storeEntriesMatch &= left.readyCycle == right.readyCycle &&
                                     left.address == right.address &&
                                     left.words == right.words &&
                                     left.laneMask == right.laneMask &&
                                     left.valid == right.valid;
            }
            bool vfEntriesMatch = true;
            for (uint32_t index = 0u; index < m_vfWritePipeline.size(); ++index)
            {
                if ((m_vfWritePipelineFreeMask & (1u << index)) != 0u)
                    continue;
                const auto &left = m_vfWritePipeline[index];
                const auto &right = generatedOracle.m_vfWritePipeline[index];
                vfEntriesMatch &= left.readyCycle == right.readyCycle &&
                                  left.sequence == right.sequence &&
                                  std::memcmp(left.value.data(), right.value.data(),
                                              sizeof(left.value)) == 0 &&
                                  left.reg == right.reg &&
                                  left.laneMask == right.laneMask &&
                                  left.valid == right.valid;
            }
            bool viEntriesMatch = true;
            for (uint32_t index = 0u; index < m_viWritePipeline.size(); ++index)
            {
                if ((m_viWritePipelineFreeMask & (1u << index)) != 0u)
                    continue;
                const auto &left = m_viWritePipeline[index];
                const auto &right = generatedOracle.m_viWritePipeline[index];
                viEntriesMatch &= left.readyCycle == right.readyCycle &&
                                  left.sequence == right.sequence &&
                                  left.value == right.value && left.reg == right.reg &&
                                  left.valid == right.valid;
            }
            bool accEntriesMatch = true;
            for (uint32_t index = 0u; index < m_accWritePipeline.size(); ++index)
            {
                if ((m_accWritePipelineFreeMask & (1u << index)) != 0u)
                    continue;
                const auto &left = m_accWritePipeline[index];
                const auto &right = generatedOracle.m_accWritePipeline[index];
                accEntriesMatch &= left.readyCycle == right.readyCycle &&
                                   left.sequence == right.sequence &&
                                   std::memcmp(left.value.data(), right.value.data(),
                                               sizeof(left.value)) == 0 &&
                                   left.laneMask == right.laneMask &&
                                   left.valid == right.valid;
            }
            const bool schedulerMatch =
                flagEntriesMatch && fdivMatch && efuEntriesMatch &&
                storeEntriesMatch && vfEntriesMatch && viEntriesMatch &&
                accEntriesMatch &&
                m_flagPipelineFreeMask == generatedOracle.m_flagPipelineFreeMask &&
                m_efuFreeMask == generatedOracle.m_efuFreeMask &&
                m_storePipelineFreeMask == generatedOracle.m_storePipelineFreeMask &&
                m_vfWritePipelineFreeMask == generatedOracle.m_vfWritePipelineFreeMask &&
                m_viWritePipelineFreeMask == generatedOracle.m_viWritePipelineFreeMask &&
                m_accWritePipelineFreeMask == generatedOracle.m_accWritePipelineFreeMask &&
                m_vfReady == generatedOracle.m_vfReady &&
                m_viReady == generatedOracle.m_viReady &&
                m_accReady == generatedOracle.m_accReady &&
                m_vfLatestWrite == generatedOracle.m_vfLatestWrite &&
                m_viLatestWrite == generatedOracle.m_viLatestWrite &&
                m_accLatestWrite == generatedOracle.m_accLatestWrite &&
                m_cycle == generatedOracle.m_cycle &&
                m_nextWriteSequence == generatedOracle.m_nextWriteSequence &&
                m_efuResourceReady == generatedOracle.m_efuResourceReady &&
                m_workingClip == generatedOracle.m_workingClip &&
                m_viBranchBackupValue == generatedOracle.m_viBranchBackupValue &&
                m_viBranchBackupReg == generatedOracle.m_viBranchBackupReg &&
                m_viBranchBackupValid == generatedOracle.m_viBranchBackupValid &&
                m_pendingHaltD == generatedOracle.m_pendingHaltD &&
                m_pendingHaltT == generatedOracle.m_pendingHaltT;
            const bool xgkickMatch =
                m_xgkick.active == generatedOracle.m_xgkick.active &&
                (!m_xgkick.active ||
                 std::memcmp(&m_xgkick, &generatedOracle.m_xgkick,
                             sizeof(m_xgkick)) == 0);
            const bool dataMatch =
                std::memcmp(vuData, generatedOracleData.data(), dataSize) == 0;
            static thread_local uint64_t generatedDifferentialBlocks = 0u;
            ++generatedDifferentialBlocks;
            if (!architecturalMatch || !schedulerMatch || !xgkickMatch || !dataMatch)
            {
                std::cerr << "[vu1-aot-diff] first_mismatch block="
                          << generatedDifferentialBlocks
                          << " architectural=" << architecturalMatch
                          << " scheduler=" << schedulerMatch
                          << " xgkick=" << xgkickMatch
                          << " data=" << dataMatch
                          << " active_parts=" << flagEntriesMatch << fdivMatch
                          << efuEntriesMatch << storeEntriesMatch << vfEntriesMatch
                          << viEntriesMatch << accEntriesMatch
                          << " pc=0x" << std::hex << m_state.pc
                          << "/0x" << generatedOracle.m_state.pc << std::dec
                          << " cycle=" << m_cycle << '/' << generatedOracle.m_cycle
                          << " free=" << static_cast<uint32_t>(m_flagPipelineFreeMask)
                          << '/' << static_cast<uint32_t>(generatedOracle.m_flagPipelineFreeMask)
                          << ',' << m_vfWritePipelineFreeMask
                          << '/' << generatedOracle.m_vfWritePipelineFreeMask
                          << ',' << static_cast<uint32_t>(m_viWritePipelineFreeMask)
                          << '/' << static_cast<uint32_t>(generatedOracle.m_viWritePipelineFreeMask)
                          << ',' << static_cast<uint32_t>(m_accWritePipelineFreeMask)
                          << '/' << static_cast<uint32_t>(generatedOracle.m_accWritePipelineFreeMask)
                          << std::endl;
                for (uint32_t index = 0u; index < m_flagPipeline.size(); ++index)
                {
                    if ((m_flagPipelineFreeMask & (1u << index)) != 0u ||
                        std::memcmp(&m_flagPipeline[index],
                                    &generatedOracle.m_flagPipeline[index],
                                    sizeof(m_flagPipeline[index])) == 0)
                        continue;
                    const auto &left = m_flagPipeline[index];
                    const auto &right = generatedOracle.m_flagPipeline[index];
                    std::cerr << "[vu1-aot-diff-flag] index=" << index
                              << " ready=" << left.readyCycle << '/' << right.readyCycle
                              << " issue=" << left.issueCycle << '/' << right.issueCycle
                              << " mac=0x" << std::hex << left.mac << "/0x" << right.mac
                              << " status=0x" << left.status << "/0x" << right.status
                              << " sticky=0x" << left.extraSticky << "/0x" << right.extraSticky
                              << " clip=0x" << left.clip << "/0x" << right.clip << std::dec
                              << " valid=" << left.valid << '/' << right.valid
                              << " writes=" << left.writesMac << left.writesStatus
                              << left.writesSticky << left.writesClip << '/'
                              << right.writesMac << right.writesStatus
                              << right.writesSticky << right.writesClip << std::endl;
                }
                for (uint32_t index = 0u; index < m_vfWritePipeline.size(); ++index)
                {
                    if ((m_vfWritePipelineFreeMask & (1u << index)) != 0u ||
                        std::memcmp(&m_vfWritePipeline[index],
                                    &generatedOracle.m_vfWritePipeline[index],
                                    sizeof(m_vfWritePipeline[index])) == 0)
                        continue;
                    const auto &left = m_vfWritePipeline[index];
                    const auto &right = generatedOracle.m_vfWritePipeline[index];
                    std::cerr << "[vu1-aot-diff-vf] index=" << index
                              << " ready=" << left.readyCycle << '/' << right.readyCycle
                              << " sequence=" << left.sequence << '/' << right.sequence
                              << " reg=" << static_cast<uint32_t>(left.reg) << '/'
                              << static_cast<uint32_t>(right.reg)
                              << " lanes=" << static_cast<uint32_t>(left.laneMask) << '/'
                              << static_cast<uint32_t>(right.laneMask)
                              << " valid=" << left.valid << '/' << right.valid
                              << " values=0x" << std::hex
                              << std::bit_cast<uint32_t>(left.value[0]) << "/0x"
                              << std::bit_cast<uint32_t>(right.value[0]) << ",0x"
                              << std::bit_cast<uint32_t>(left.value[1]) << "/0x"
                              << std::bit_cast<uint32_t>(right.value[1]) << ",0x"
                              << std::bit_cast<uint32_t>(left.value[2]) << "/0x"
                              << std::bit_cast<uint32_t>(right.value[2]) << ",0x"
                              << std::bit_cast<uint32_t>(left.value[3]) << "/0x"
                              << std::bit_cast<uint32_t>(right.value[3]) << std::dec
                              << std::endl;
                }
                *this = generatedOracle;
                std::memcpy(vuData, generatedOracleData.data(), dataSize);
                m_activeVuData = vuData;
                m_activeVuDataSize = dataSize;
                m_activeGs = &gs;
                m_activeMemory = memory;
                g_capturedHotBlockRejected = true;
            }
        }
        if (executedGeneratedBlock)
        {
            static const bool traceGeneratedProgress =
                std::getenv("PS2X_VU1_AOT_PROGRESS") != nullptr;
            static thread_local uint64_t generatedBlockCount = 0u;
            ++generatedBlockCount;
            if (traceGeneratedProgress &&
                (generatedBlockCount == 1u ||
                 (generatedBlockCount & (generatedBlockCount - 1u)) == 0u))
            {
                std::cerr << "[vu1-aot-progress] blocks=" << generatedBlockCount
                          << " cycle=" << m_cycle
                          << " budget_end=" << budgetEnd
                          << " remaining=" << (budgetEnd > m_cycle ? budgetEnd - m_cycle : 0u)
                          << " pc=0x" << std::hex << m_state.pc << std::dec
                          << " vi5=" << m_state.vi[5]
                          << std::endl;
            }
            continue;
        }
#if 0 // Superseded by the side-owned generated include seam above.
        if (useCapturedHotBlock && m_state.pc == 0x158u &&
            budgetEnd - m_cycle >= 15u)
        {
#if 0 // Differential proof completed; keep the production hot path out of this cold diagnostic block.
            static const bool differentialEnabled =
                std::getenv("PS2X_VU1_CAPTURED_DIFFERENTIAL") != nullptr;
            static bool differentialComplete = false;
            static uint64_t differentialBlocks = 0u;
            VU1Interpreter oracle = *this;
            std::vector<uint8_t> oracleData;
            if (differentialEnabled && !differentialComplete)
            {
                oracleData.assign(vuData, vuData + dataSize);
                g_disableCapturedHotBlock = true;
                oracle.run(vuCode, codeSize, oracleData.data(), dataSize,
                           gs, memory, 15u);
                g_disableCapturedHotBlock = false;
            }
#endif
#define PS2X_CAPTURED_PAIR(PC, LOWER, UPPER) \
            if (!executeCapturedPair((PC) / 8u, \
                    std::integral_constant<uint32_t, LOWER>{}, \
                    std::integral_constant<uint32_t, UPPER>{})) \
                break
            PS2X_CAPTURED_PAIR(0x0158u, 0x01eb1802u, 0x01cb59ffu);
            PS2X_CAPTURED_PAIR(0x0160u, 0x01ec1801u, 0x000002ffu);
            PS2X_CAPTURED_PAIR(0x0168u, 0x12052801u, 0x000002ffu);
            PS2X_CAPTURED_PAIR(0x0170u, 0x03c457fcu, 0x01c04a9cu);
            PS2X_CAPTURED_PAIR(0x0178u, 0x03c46ffau, 0x01eb09bcu);
            PS2X_CAPTURED_PAIR(0x0180u, 0x01cd1ffdu, 0x01eb10bdu);
            PS2X_CAPTURED_PAIR(0x0188u, 0x2403ffffu, 0x01eb18beu);
            PS2X_CAPTURED_PAIR(0x0190u, 0x80013070u, 0x01e0224bu);
            PS2X_CAPTURED_PAIR(0x0198u, 0x80665bfcu, 0x01eb29bcu);
            PS2X_CAPTURED_PAIR(0x01a0u, 0x03e46001u, 0x01eb30bdu);
            PS2X_CAPTURED_PAIR(0x01a8u, 0x0a2127ffu, 0x01eb38beu);
            PS2X_CAPTURED_PAIR(0x01b0u, 0x81e903bcu, 0x01e042cbu);
            PS2X_CAPTURED_PAIR(0x01b8u, 0x800318f2u, 0x01c06b5cu);
            PS2X_CAPTURED_PAIR(0x01c0u, 0x520507f2u, 0x018a517du);
            PS2X_CAPTURED_PAIR(0x01c8u, 0x800420f2u, 0x004a517cu);
#undef PS2X_CAPTURED_PAIR
#if 0 // See the matching disabled oracle setup above.
            if (differentialEnabled && !differentialComplete)
            {
                const bool vfArchitecturalMatch =
                    std::memcmp(m_state.vf, oracle.m_state.vf, sizeof(m_state.vf)) == 0;
                const bool viArchitecturalMatch =
                    std::memcmp(m_state.vi, oracle.m_state.vi, sizeof(m_state.vi)) == 0;
                const bool accArchitecturalMatch =
                    std::memcmp(m_state.acc, oracle.m_state.acc, sizeof(m_state.acc)) == 0;
                const bool scalarArchitecturalMatch =
                    std::memcmp(&m_state.q, &oracle.m_state.q, sizeof(m_state.q)) == 0 &&
                    std::memcmp(&m_state.p, &oracle.m_state.p, sizeof(m_state.p)) == 0 &&
                    m_state.i == oracle.m_state.i && m_state.r == oracle.m_state.r;
                const bool controlArchitecturalMatch =
                    m_state.pc == oracle.m_state.pc && m_state.mac == oracle.m_state.mac &&
                    m_state.clip == oracle.m_state.clip && m_state.status == oracle.m_state.status &&
                    m_cycle == oracle.m_cycle && m_state.ebit == oracle.m_state.ebit &&
                    m_state.haltAfterDelaySlot == oracle.m_state.haltAfterDelaySlot &&
                    m_state.top == oracle.m_state.top && m_state.itop == oracle.m_state.itop &&
                    m_state.branchPending == oracle.m_state.branchPending &&
                    m_state.branchTarget == oracle.m_state.branchTarget &&
                    m_state.branchDelay == oracle.m_state.branchDelay;
                const bool architecturalMatch =
                    vfArchitecturalMatch && viArchitecturalMatch && accArchitecturalMatch &&
                    scalarArchitecturalMatch && controlArchitecturalMatch;
                bool flagActiveMatch = true, vfActiveMatch = true;
                bool viActiveMatch = true, accActiveMatch = true;
                for (uint32_t index = 0u; index < kMaxFlagEntries; ++index)
                    if ((m_flagPipelineFreeMask & (1u << index)) == 0u)
                    {
                        const auto &a = m_flagPipeline[index], &b = oracle.m_flagPipeline[index];
                        flagActiveMatch &= a.readyCycle == b.readyCycle && a.issueCycle == b.issueCycle &&
                            a.mac == b.mac && a.status == b.status && a.extraSticky == b.extraSticky &&
                            a.clip == b.clip && a.valid == b.valid && a.writesMac == b.writesMac &&
                            a.writesStatus == b.writesStatus && a.writesSticky == b.writesSticky &&
                            a.writesClip == b.writesClip;
                    }
                for (uint32_t index = 0u; index < kMaxPendingVfWrites; ++index)
                    if ((m_vfWritePipelineFreeMask & (1u << index)) == 0u)
                    {
                        const auto &a = m_vfWritePipeline[index], &b = oracle.m_vfWritePipeline[index];
                        vfActiveMatch &= a.readyCycle == b.readyCycle && a.sequence == b.sequence &&
                            std::memcmp(a.value.data(), b.value.data(), sizeof(a.value)) == 0 &&
                            a.reg == b.reg && a.laneMask == b.laneMask && a.valid == b.valid;
                    }
                for (uint32_t index = 0u; index < kMaxPendingViWrites; ++index)
                    if ((m_viWritePipelineFreeMask & (1u << index)) == 0u)
                    {
                        const auto &a = m_viWritePipeline[index], &b = oracle.m_viWritePipeline[index];
                        viActiveMatch &= a.readyCycle == b.readyCycle && a.sequence == b.sequence &&
                            a.value == b.value && a.reg == b.reg && a.valid == b.valid;
                    }
                for (uint32_t index = 0u; index < kMaxPendingAccWrites; ++index)
                    if ((m_accWritePipelineFreeMask & (1u << index)) == 0u)
                    {
                        const auto &a = m_accWritePipeline[index], &b = oracle.m_accWritePipeline[index];
                        accActiveMatch &= a.readyCycle == b.readyCycle && a.sequence == b.sequence &&
                            std::memcmp(a.value.data(), b.value.data(), sizeof(a.value)) == 0 &&
                            a.laneMask == b.laneMask && a.valid == b.valid;
                    }
                const bool schedulerMatch =
                    m_flagPipelineFreeMask == oracle.m_flagPipelineFreeMask &&
                    m_vfWritePipelineFreeMask == oracle.m_vfWritePipelineFreeMask &&
                    m_viWritePipelineFreeMask == oracle.m_viWritePipelineFreeMask &&
                    m_accWritePipelineFreeMask == oracle.m_accWritePipelineFreeMask &&
                    flagActiveMatch && vfActiveMatch && viActiveMatch && accActiveMatch &&
                    m_vfReady == oracle.m_vfReady && m_viReady == oracle.m_viReady &&
                    m_accReady == oracle.m_accReady && m_vfLatestWrite == oracle.m_vfLatestWrite &&
                    m_viLatestWrite == oracle.m_viLatestWrite && m_accLatestWrite == oracle.m_accLatestWrite &&
                    m_nextWriteSequence == oracle.m_nextWriteSequence &&
                    m_workingClip == oracle.m_workingClip &&
                    m_viBranchBackupValue == oracle.m_viBranchBackupValue &&
                    m_viBranchBackupReg == oracle.m_viBranchBackupReg &&
                    m_viBranchBackupValid == oracle.m_viBranchBackupValid;
                const bool semanticMatch = architecturalMatch && schedulerMatch &&
                    std::memcmp(vuData, oracleData.data(), dataSize) == 0;
                ++differentialBlocks;
                if (!semanticMatch || differentialBlocks >= 50000u)
                {
                    differentialComplete = true;
                std::cerr << "[vu1-captured-diff] state="
                          << (std::memcmp(&m_state, &oracle.m_state, sizeof(m_state)) == 0)
                          << " flags="
                          << (std::memcmp(&m_flagPipeline, &oracle.m_flagPipeline,
                                          sizeof(m_flagPipeline)) == 0)
                          << " scalar=" << (std::memcmp(&m_fdiv, &oracle.m_fdiv, sizeof(m_fdiv)) == 0)
                          << " stores=" << (std::memcmp(&m_storePipeline, &oracle.m_storePipeline,
                                                         sizeof(m_storePipeline)) == 0)
                          << " vf_queue=" << (std::memcmp(&m_vfWritePipeline, &oracle.m_vfWritePipeline,
                                                           sizeof(m_vfWritePipeline)) == 0)
                          << " vi_queue=" << (std::memcmp(&m_viWritePipeline, &oracle.m_viWritePipeline,
                                                           sizeof(m_viWritePipeline)) == 0)
                          << " acc_queue=" << (std::memcmp(&m_accWritePipeline, &oracle.m_accWritePipeline,
                                                            sizeof(m_accWritePipeline)) == 0)
                          << " ready="
                          << (m_vfReady == oracle.m_vfReady && m_viReady == oracle.m_viReady &&
                              m_accReady == oracle.m_accReady)
                          << " xgkick=" << (std::memcmp(&m_xgkick, &oracle.m_xgkick,
                                                        sizeof(m_xgkick)) == 0)
                          << " data=" << (std::memcmp(vuData, oracleData.data(), dataSize) == 0)
                          << '\n';
                std::cerr << "[vu1-captured-diff-detail] pc=0x" << std::hex
                          << m_state.pc << "/0x" << oracle.m_state.pc << std::dec
                          << " cycle=" << m_cycle << '/' << oracle.m_cycle
                          << " mac=0x" << std::hex << m_state.mac << "/0x" << oracle.m_state.mac
                          << " status=0x" << m_state.status << "/0x" << oracle.m_state.status
                          << " clip=0x" << m_state.clip << "/0x" << oracle.m_state.clip << std::dec
                          << " free=" << static_cast<uint32_t>(m_flagPipelineFreeMask) << '/'
                          << static_cast<uint32_t>(oracle.m_flagPipelineFreeMask) << ','
                          << m_vfWritePipelineFreeMask << '/' << oracle.m_vfWritePipelineFreeMask << ','
                          << static_cast<uint32_t>(m_accWritePipelineFreeMask) << '/'
                          << static_cast<uint32_t>(oracle.m_accWritePipelineFreeMask) << '\n';
                std::cerr << "[vu1-captured-diff-state] q=" << m_state.q << '/' << oracle.m_state.q
                          << " p=" << m_state.p << '/' << oracle.m_state.p
                          << " i=" << m_state.i << '/' << oracle.m_state.i
                          << " r=" << m_state.r << '/' << oracle.m_state.r
                          << " top=" << m_state.top << '/' << oracle.m_state.top
                          << " itop=" << m_state.itop << '/' << oracle.m_state.itop
                          << " ebit=" << m_state.ebit << '/' << oracle.m_state.ebit
                          << " halt=" << m_state.haltAfterDelaySlot << '/'
                          << oracle.m_state.haltAfterDelaySlot
                          << " branch=" << m_state.branchPending << '/' << oracle.m_state.branchPending
                          << ',' << m_state.branchTarget << '/' << oracle.m_state.branchTarget
                          << ',' << m_state.branchDelay << '/' << oracle.m_state.branchDelay
                          << " seq=" << m_nextWriteSequence << '/' << oracle.m_nextWriteSequence
                          << " working_clip=" << m_workingClip << '/' << oracle.m_workingClip
                          << " branch_backup=" << static_cast<uint32_t>(m_viBranchBackupReg) << '/'
                          << static_cast<uint32_t>(oracle.m_viBranchBackupReg) << ','
                          << m_viBranchBackupValue << '/' << oracle.m_viBranchBackupValue << ','
                          << m_viBranchBackupValid << '/' << oracle.m_viBranchBackupValid << '\n';
                for (uint32_t reg = 0u; reg < 32u; ++reg)
                {
                    if (std::memcmp(m_state.vf[reg], oracle.m_state.vf[reg], sizeof(m_state.vf[reg])) != 0)
                        std::cerr << "[vu1-captured-diff-vf] reg=" << reg << '\n';
                }
                if (std::memcmp(m_state.acc, oracle.m_state.acc, sizeof(m_state.acc)) != 0)
                    std::cerr << "[vu1-captured-diff-acc] value mismatch\n";
                for (uint32_t reg = 0u; reg < 16u; ++reg)
                    if (m_state.vi[reg] != oracle.m_state.vi[reg])
                        std::cerr << "[vu1-captured-diff-vi] reg=" << reg << " value="
                                  << m_state.vi[reg] << '/' << oracle.m_state.vi[reg] << '\n';
                std::cerr << "[vu1-captured-diff-latest] vf=" << (m_vfLatestWrite == oracle.m_vfLatestWrite)
                          << " vi=" << (m_viLatestWrite == oracle.m_viLatestWrite)
                          << " acc=" << (m_accLatestWrite == oracle.m_accLatestWrite) << '\n';
                for (uint32_t index = 0u; index < kMaxPendingVfWrites; ++index)
                    if (std::memcmp(&m_vfWritePipeline[index], &oracle.m_vfWritePipeline[index], sizeof(PendingVfWrite)) != 0)
                    {
                        const auto &left = m_vfWritePipeline[index];
                        const auto &right = oracle.m_vfWritePipeline[index];
                        std::cerr << "[vu1-captured-diff-vfq] index=" << index
                                  << " valid=" << left.valid << '/' << right.valid
                                  << " ready=" << left.readyCycle << '/' << right.readyCycle
                                  << " seq=" << left.sequence << '/' << right.sequence
                                  << " reg=" << static_cast<uint32_t>(left.reg) << '/'
                                  << static_cast<uint32_t>(right.reg)
                                  << " lanes=" << static_cast<uint32_t>(left.laneMask) << '/'
                                  << static_cast<uint32_t>(right.laneMask)
                                  << " values=" << std::hex
                                  << std::bit_cast<uint32_t>(left.value[0]) << '/'
                                  << std::bit_cast<uint32_t>(right.value[0]) << ','
                                  << std::bit_cast<uint32_t>(left.value[1]) << '/'
                                  << std::bit_cast<uint32_t>(right.value[1]) << ','
                                  << std::bit_cast<uint32_t>(left.value[2]) << '/'
                                  << std::bit_cast<uint32_t>(right.value[2]) << ','
                                  << std::bit_cast<uint32_t>(left.value[3]) << '/'
                                  << std::bit_cast<uint32_t>(right.value[3]) << std::dec << '\n';
                    }
                for (uint32_t index = 0u; index < kMaxPendingAccWrites; ++index)
                    if (std::memcmp(&m_accWritePipeline[index], &oracle.m_accWritePipeline[index], sizeof(PendingAccWrite)) != 0)
                        std::cerr << "[vu1-captured-diff-accq] index=" << index << '\n';
                for (uint32_t index = 0u; index < kMaxFlagEntries; ++index)
                    if (std::memcmp(&m_flagPipeline[index], &oracle.m_flagPipeline[index], sizeof(FlagPipelineEntry)) != 0)
                    {
                        const auto &left = m_flagPipeline[index];
                        const auto &right = oracle.m_flagPipeline[index];
                        std::cerr << "[vu1-captured-diff-flagq] index=" << index
                                  << " valid=" << left.valid << '/' << right.valid
                                  << " ready=" << left.readyCycle << '/' << right.readyCycle
                                  << " mac=0x" << std::hex << left.mac << "/0x" << right.mac
                                  << " status=0x" << left.status << "/0x" << right.status
                                  << " sticky=0x" << left.extraSticky << "/0x" << right.extraSticky
                                  << " clip=0x" << left.clip << "/0x" << right.clip << std::dec
                                  << " writes=" << left.writesMac << left.writesStatus
                                  << left.writesSticky << left.writesClip << '/'
                                  << right.writesMac << right.writesStatus
                                  << right.writesSticky << right.writesClip << '\n';
                    }
                    std::cerr << "[vu1-captured-diff-semantic] blocks=" << differentialBlocks
                              << " match=" << semanticMatch
                              << " architectural=" << architecturalMatch
                              << " arch_parts=" << vfArchitecturalMatch << viArchitecturalMatch
                              << accArchitecturalMatch << scalarArchitecturalMatch
                              << controlArchitecturalMatch
                              << " control_parts="
                              << (m_state.pc == oracle.m_state.pc)
                              << (m_state.mac == oracle.m_state.mac)
                              << (m_state.clip == oracle.m_state.clip)
                              << (m_state.status == oracle.m_state.status)
                              << (m_cycle == oracle.m_cycle)
                              << (m_state.ebit == oracle.m_state.ebit)
                              << (m_state.haltAfterDelaySlot == oracle.m_state.haltAfterDelaySlot)
                              << (m_state.top == oracle.m_state.top)
                              << (m_state.itop == oracle.m_state.itop)
                              << (m_state.branchPending == oracle.m_state.branchPending)
                              << (m_state.branchTarget == oracle.m_state.branchTarget)
                              << (m_state.branchDelay == oracle.m_state.branchDelay)
                              << " scheduler=" << schedulerMatch
                              << " active=" << flagActiveMatch << vfActiveMatch
                              << viActiveMatch << accActiveMatch
                              << " data=" << (std::memcmp(vuData, oracleData.data(), dataSize) == 0)
                              << '\n';
                }
            }
#endif
            continue;
        }
#endif
        const uint32_t scalarPairPc = m_state.pc;
        const uint64_t scalarPairStartCycle = m_cycle;
        const bool samplePhase = aggregateRun &&
                                 ((g_vuPhaseSampleSequence++ & 0xFFu) == 0u);
        const auto phaseStart = samplePhase ? VuProfileClock::now()
                                            : VuProfileClock::time_point{};
        if (m_state.pc + 8u > codeSize)
            break;

        const uint32_t pairIndex = m_state.pc / 8u;
        const DecodedInstructionPair &decoded =
            runDecodedCode != nullptr && (m_state.pc & 7u) == 0u &&
                    pairIndex < runDecodedPairCount
                ? runDecodedCode[pairIndex]
                : getDecodedInstructionPairForPc(vuCode, codeSize, memory, m_state.pc);
        if (aggregateRun)
        {
            const uint8_t op = static_cast<uint8_t>(decoded.upper & 0x3Fu);
            const uint8_t key = op >= 0x3Cu
                                    ? static_cast<uint8_t>(128u +
                                        ((decoded.upper & 0x3u) | ((decoded.upper >> 4) & 0x7Cu)))
                                    : op;
            ++profileUpperOpcodeCounts[key];
            ++profileUpperDestinationLaneCounts[std::popcount(
                static_cast<unsigned int>(DEST(decoded.upper)))];
        }
        if (decoded.upperUsage.reserved || decoded.lowerUsage.reserved)
        {
            reportReservedInstruction(decoded.upperUsage.reserved, decoded.upperUsage.reserved ? decoded.upper : decoded.lower);
            break;
        }

        uint64_t readyCycle = calculatePairReadyCycle(decoded);
        while (readyCycle > m_cycle)
        {
            if (readyCycle >= budgetEnd)
            {
                advanceTo(budgetEnd);
                break;
            }
            advanceTo(readyCycle);
            readyCycle = calculatePairReadyCycle(decoded);
        }
        if (m_cycle >= budgetEnd)
            break;

        const auto phaseExecuteStart = samplePhase ? VuProfileClock::now()
                                                   : VuProfileClock::time_point{};

        uint8_t writtenVi = 0u;
        int32_t oldVi = 0;
        const uint16_t viWrites = static_cast<uint16_t>(decoded.lowerUsage.viWrite & 0xFFFEu);
        if (viWrites != 0u)
        {
            writtenVi = static_cast<uint8_t>(std::countr_zero(viWrites));
            oldVi = m_state.vi[writtenVi];
        }

        const VfAccess upperWrite = decoded.upperUsage.vfWrite;
        const VfAccess lowerWrite = decoded.lowerUsage.vfWrite;
        const bool hasUpperWrite = upperWrite.reg != 0u;
        const bool hasLowerWrite = lowerWrite.reg != 0u && decoded.suppressedLowerVf != lowerWrite.reg;
        const bool hasDistinctLowerWrite = hasLowerWrite && (!hasUpperWrite || lowerWrite.reg != upperWrite.reg);
        float oldUpperVf[4]{};
        float newUpperVf[4]{};
        float oldLowerVf[4]{};
        float newLowerVf[4]{};
        float oldAcc[4]{};
        float newAcc[4]{};
        if (hasUpperWrite)
            std::memcpy(oldUpperVf, m_state.vf[upperWrite.reg], sizeof(oldUpperVf));
        if (hasDistinctLowerWrite)
            std::memcpy(oldLowerVf, m_state.vf[lowerWrite.reg], sizeof(oldLowerVf));
        if (decoded.upperUsage.accWrite != 0u)
            std::memcpy(oldAcc, m_state.acc, sizeof(oldAcc));

        if (decoded.iBit)
        {
            execUpper(decoded.upper);
            float immediate = 0.0f;
            std::memcpy(&immediate, &decoded.lower, sizeof(immediate));
            m_state.i = normalizeOperand(immediate);
        }
        else if (decoded.upperVfShadowReg != 0u)
        {
            float oldVf[4]{};
            float upperVf[4]{};
            std::memcpy(oldVf,
                        m_state.vf[decoded.upperVfShadowReg],
                        sizeof(oldVf));
            execUpper(decoded.upper);
            std::memcpy(upperVf,
                        m_state.vf[decoded.upperVfShadowReg],
                        sizeof(upperVf));
            std::memcpy(m_state.vf[decoded.upperVfShadowReg],
                        oldVf,
                        sizeof(oldVf));
            execLower(decoded.lower, vuData, dataSize, gs, memory, decoded.upper);
            std::memcpy(m_state.vf[decoded.upperVfShadowReg],
                        upperVf,
                        sizeof(upperVf));
        }
        else
        {
            execUpper(decoded.upper);
            execLower(decoded.lower, vuData, dataSize, gs, memory, decoded.upper);
        }

        const auto phaseExecuteEnd = samplePhase ? VuProfileClock::now()
                                                 : VuProfileClock::time_point{};

        m_viBranchBackupValid = false;

        if (hasUpperWrite)
        {
            std::memcpy(newUpperVf, m_state.vf[upperWrite.reg], sizeof(newUpperVf));
            std::memcpy(m_state.vf[upperWrite.reg], oldUpperVf, sizeof(oldUpperVf));
            const uint32_t latency =
                decoded.upperUsage.vfLatency != 0u
                    ? decoded.upperUsage.vfLatency
                    : decoded.upperUsage.latency;
            queueVfWrite(upperWrite.reg, upperWrite.lanes, newUpperVf, latency);
        }
        if (hasDistinctLowerWrite)
        {
            std::memcpy(newLowerVf, m_state.vf[lowerWrite.reg], sizeof(newLowerVf));
            std::memcpy(m_state.vf[lowerWrite.reg], oldLowerVf, sizeof(oldLowerVf));
            const uint32_t latency = decoded.lowerUsage.vfLatency != 0u
                                         ? decoded.lowerUsage.vfLatency
                                         : decoded.lowerUsage.latency;
            queueVfWrite(lowerWrite.reg, lowerWrite.lanes, newLowerVf, latency);
        }
        if (decoded.upperUsage.accWrite != 0u)
        {
            std::memcpy(newAcc, m_state.acc, sizeof(newAcc));
            std::memcpy(m_state.acc, oldAcc, sizeof(oldAcc));
            // ACC is forwarded to the next upper instruction. Its arithmetic
            // flags still use the normal four-cycle FMAC timeline.
            queueAccWrite(decoded.upperUsage.accWrite, newAcc,
                          kAccForwardLatency);
        }
        if (writtenVi != 0u)
        {
            const int32_t newVi = m_state.vi[writtenVi];
            m_state.vi[writtenVi] = oldVi;
            const uint32_t latency =
                decoded.lowerUsage.viLatency != 0u
                    ? decoded.lowerUsage.viLatency
                    : decoded.lowerUsage.latency;
            queueViWrite(writtenVi, newVi, latency);
        }

        markPairWrites(decoded);
        if (writtenVi != 0u && decoded.lowerUsage.delaysNextBranchRead)
            recordViWriteForBranch(writtenVi, oldVi);

        m_state.vf[0][0] = 0.0f;
        m_state.vf[0][1] = 0.0f;
        m_state.vf[0][2] = 0.0f;
        m_state.vf[0][3] = 1.0f;
        m_state.vi[0] = 0;

        uint32_t nextPc = m_state.pc + 8u;
        if (nextPc >= codeSize)
            nextPc = 0u;
        m_state.pc = nextPc;

        if (m_state.branchPending)
        {
            if (m_state.branchDelay == 0u)
            {
                m_state.pc = m_state.branchTarget & microAddressMask();
                m_state.branchPending = false;
            }
            else
            {
                --m_state.branchDelay;
            }
        }

        const bool dHalt = decoded.dBit && m_state.dBitEnabled;
        const bool tHalt = decoded.tBit && m_state.tBitEnabled;
        const bool haltBit = dHalt || tHalt;
        const bool haltBranch = haltBit && decoded.lowerUsage.pipeline == PipelineBranch;

        if (m_state.haltAfterDelaySlot)
        {
            m_state.stoppedByD = m_pendingHaltD;
            m_state.stoppedByT = m_pendingHaltT;
            programEnded = true;
        }
        else if (m_state.ebit)
            programEnded = true;
        else if (haltBit && !haltBranch)
        {
            m_state.stoppedByD = dHalt;
            m_state.stoppedByT = tHalt;
            programEnded = true;
        }
        else if (decoded.eBit)
            m_state.ebit = true;
        else if (haltBranch)
        {
            m_state.haltAfterDelaySlot = true;
            m_pendingHaltD = dHalt;
            m_pendingHaltT = tHalt;
        }

        advanceOneCycle();
        if (capturedScheduleProfileEnabled &&
            capturedCodeHash == 0xa198bebb3e5909d9ull &&
            scalarPairPc <= 0x0290u && (scalarPairPc & 7u) == 0u)
        {
            struct CapturedScheduleProfile
            {
                std::array<std::array<uint64_t, 64>, 83> cycleDeltas{};
                std::array<std::array<uint64_t, 83>, 83> successors{};
                uint64_t pairs = 0u;
                bool printed = false;
            };
            static thread_local CapturedScheduleProfile profile;
            const uint32_t pcIndex = scalarPairPc / 8u;
            const uint32_t nextIndex = std::min<uint32_t>(m_state.pc / 8u, 82u);
            const uint64_t cycleDelta = m_cycle - scalarPairStartCycle;
            ++profile.cycleDeltas[pcIndex][std::min<uint64_t>(cycleDelta, 63u)];
            ++profile.successors[pcIndex][nextIndex];
            ++profile.pairs;
            if (!profile.printed && profile.pairs >= 1000000u)
            {
                profile.printed = true;
                std::cerr << "[vu1-captured-schedule] pairs=" << profile.pairs << std::endl;
                for (uint32_t source = 0u; source < profile.cycleDeltas.size(); ++source)
                {
                    uint64_t total = 0u;
                    for (uint64_t count : profile.cycleDeltas[source])
                        total += count;
                    if (total == 0u)
                        continue;
                    std::cerr << "[vu1-captured-schedule] pc=0x" << std::hex
                              << source * 8u << std::dec << " calls=" << total
                              << " deltas=";
                    for (uint32_t delta = 0u; delta < 64u; ++delta)
                        if (profile.cycleDeltas[source][delta] != 0u)
                            std::cerr << delta << ':'
                                      << profile.cycleDeltas[source][delta] << ',';
                    std::cerr << " successors=";
                    for (uint32_t successor = 0u;
                         successor < profile.successors[source].size(); ++successor)
                        if (profile.successors[source][successor] != 0u)
                            std::cerr << "0x" << std::hex << successor * 8u
                                      << std::dec << ':'
                                      << profile.successors[source][successor] << ',';
                    std::cerr << std::endl;
                }
            }
        }
        if (samplePhase)
        {
            const auto phaseEnd = VuProfileClock::now();
            ++profilePhaseSamples;
            profilePhasePreNanoseconds += static_cast<uint64_t>(
                std::chrono::duration_cast<std::chrono::nanoseconds>(
                    phaseExecuteStart - phaseStart).count());
            profilePhaseExecuteNanoseconds += static_cast<uint64_t>(
                std::chrono::duration_cast<std::chrono::nanoseconds>(
                    phaseExecuteEnd - phaseExecuteStart).count());
            profilePhasePostNanoseconds += static_cast<uint64_t>(
                std::chrono::duration_cast<std::chrono::nanoseconds>(
                    phaseEnd - phaseExecuteEnd).count());
        }
        if (programEnded)
            break;
    }

    if (programEnded)
    {
        flushPipelines();
        m_state.ebit = false;
        m_state.haltAfterDelaySlot = false;
        m_pendingHaltD = false;
        m_pendingHaltT = false;
    }
    m_state.cycles = m_cycle;
    if (useVuRounding && changeVuRounding && previousRoundingMode != -1)
        std::fesetround(previousRoundingMode);
    const auto profileEnd = profileRun ? VuProfileClock::now() : VuProfileClock::time_point{};
    if (aggregateRun)
    {
        VuAggregateStat &stat = aggregateProfile.stats[profileStartPc];
        ++stat.calls;
        stat.cycles += m_cycle - profileStartCycle;
        stat.nanoseconds += static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(profileEnd - profileStart).count());
        stat.xgkickNanoseconds += g_vuRunProfileXgkickNanoseconds;
        stat.xgkickPackets += g_vuRunProfileXgkickPackets;
        stat.decodeNanoseconds += g_vuRunProfileDecodeNanoseconds;
        stat.decodeRebuilds += g_vuRunProfileDecodeRebuilds;
        stat.phaseSamples += profilePhaseSamples;
        stat.phasePreNanoseconds += profilePhasePreNanoseconds;
        stat.phaseExecuteNanoseconds += profilePhaseExecuteNanoseconds;
        stat.phasePostNanoseconds += profilePhasePostNanoseconds;
        if (profileEntryStateHash != 0u)
            ++stat.entryStateCounts[profileEntryStateHash];
        for (size_t index = 0u; index < stat.upperOpcodeCounts.size(); ++index)
            stat.upperOpcodeCounts[index] += profileUpperOpcodeCounts[index];
        for (size_t index = 0u; index < stat.upperDestinationLaneCounts.size(); ++index)
            stat.upperDestinationLaneCounts[index] += profileUpperDestinationLaneCounts[index];
    }
    if (aggregateProfile.enabled && !aggregateProfile.printed &&
        VuProfileClock::now() >= aggregateProfile.deadline)
    {
        aggregateProfile.printed = true;
        std::vector<std::pair<uint32_t, VuAggregateStat>> ordered(
            aggregateProfile.stats.begin(), aggregateProfile.stats.end());
        std::sort(ordered.begin(), ordered.end(), [](const auto &left, const auto &right) {
            return left.second.nanoseconds > right.second.nanoseconds;
        });
        std::cerr << "[vu1-aggregate] top microprogram starts by host time" << std::endl;
        const size_t limit = std::min<size_t>(ordered.size(), 24u);
        for (size_t index = 0u; index < limit; ++index)
        {
            const auto &[startPc, stat] = ordered[index];
            std::cerr << "[vu1-aggregate] start_pc=0x" << std::hex << startPc << std::dec
                      << " calls=" << stat.calls
                      << " cycles=" << stat.cycles
                      << " total_ms=" << (stat.nanoseconds / 1.0e6)
                      << " xgkick_ms=" << (stat.xgkickNanoseconds / 1.0e6)
                      << " xgkick_packets=" << stat.xgkickPackets
                      << " decode_ms=" << (stat.decodeNanoseconds / 1.0e6)
                      << " decode_rebuilds=" << stat.decodeRebuilds
                      << " phase_samples=" << stat.phaseSamples
                      << " phase_pre_ns="
                      << (stat.phaseSamples != 0u
                              ? stat.phasePreNanoseconds / stat.phaseSamples
                              : 0u)
                      << " phase_execute_ns="
                      << (stat.phaseSamples != 0u
                              ? stat.phaseExecuteNanoseconds / stat.phaseSamples
                              : 0u)
                      << " phase_post_ns="
                      << (stat.phaseSamples != 0u
                              ? stat.phasePostNanoseconds / stat.phaseSamples
                              : 0u)
                      << " dest_lanes=" << stat.upperDestinationLaneCounts[0] << ','
                      << stat.upperDestinationLaneCounts[1] << ','
                      << stat.upperDestinationLaneCounts[2] << ','
                      << stat.upperDestinationLaneCounts[3] << ','
                      << stat.upperDestinationLaneCounts[4]
                      << std::endl;
            if (index < 2u)
            {
                std::vector<std::pair<uint64_t, uint64_t>> entryStates(
                    stat.entryStateCounts.begin(), stat.entryStateCounts.end());
                std::sort(entryStates.begin(), entryStates.end(), [](const auto &left, const auto &right) {
                    return left.second > right.second;
                });
                const size_t entryStateLimit = std::min<size_t>(entryStates.size(), 8u);
                for (size_t stateIndex = 0u; stateIndex < entryStateLimit; ++stateIndex)
                {
                    std::cerr << "[vu1-aggregate-entry] start_pc=0x" << std::hex << startPc
                              << " state=0x" << entryStates[stateIndex].first << std::dec
                              << " calls=" << entryStates[stateIndex].second << std::endl;
                }
                std::vector<std::pair<uint32_t, uint64_t>> opcodeCounts;
                for (uint32_t key = 0u; key < stat.upperOpcodeCounts.size(); ++key)
                {
                    if (stat.upperOpcodeCounts[key] != 0u)
                        opcodeCounts.emplace_back(key, stat.upperOpcodeCounts[key]);
                }
                std::sort(opcodeCounts.begin(), opcodeCounts.end(), [](const auto &left, const auto &right) {
                    return left.second > right.second;
                });
                const size_t opcodeLimit = std::min<size_t>(opcodeCounts.size(), 12u);
                for (size_t opcodeIndex = 0u; opcodeIndex < opcodeLimit; ++opcodeIndex)
                {
                    const auto &[key, count] = opcodeCounts[opcodeIndex];
                    std::cerr << "[vu1-aggregate-op] start_pc=0x" << std::hex << startPc
                              << (key >= 128u ? " special=0x" : " op=0x")
                              << (key >= 128u ? key - 128u : key)
                              << std::dec << " calls=" << count << std::endl;
                }
            }
        }
    }
    if (legacyProfileRun)
    {
        const double elapsedMilliseconds =
            std::chrono::duration<double, std::milli>(
                profileEnd - profileStart)
                .count();
        const double xgkickMilliseconds =
            static_cast<double>(g_vuRunProfileXgkickNanoseconds) / 1000000.0;
        RUNTIME_ERROR(
            "[vu1-run-profile]"
            << " start_pc=0x" << std::hex << profileStartPc
            << " end_pc=0x" << m_state.pc << std::dec
            << " cycles=" << (m_cycle - profileStartCycle)
            << " elapsed_ms=" << elapsedMilliseconds
            << " xgkick_ms=" << xgkickMilliseconds
            << " xgkick_packets=" << g_vuRunProfileXgkickPackets
            << " ended=" << (programEnded ? 1 : 0)
            << " stopped=" << (m_stopRequested ? 1 : 0)
            << '\n');
    }
}
