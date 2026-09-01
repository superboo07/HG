#include "module_factories.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdlib>
#include <limits>
#include <mutex>
#include <sstream>
#include <stdexcept>
#include <utility>
#include <vector>

namespace ps2x::iop::detail
{
    namespace
    {
        constexpr uint32_t kRequestBytes = 32u;
        constexpr uint32_t kSourceWord = 2u;
        constexpr uint32_t kDestinationWord = 3u;
        constexpr uint32_t kSizeWord = 4u;
        constexpr uint32_t kWaitStrategyWord = 5u;
        constexpr uint32_t kMailboxHeaderBytes = 16u;
        constexpr uint32_t kMailboxRecordBytes = 16u;
        constexpr uint32_t kStreamControlOpcode = 5u;
        constexpr uint32_t kStreamStartOpcode = 8u;

        uint32_t readLe32(const uint8_t *bytes)
        {
            return static_cast<uint32_t>(bytes[0]) |
                   (static_cast<uint32_t>(bytes[1]) << 8u) |
                   (static_cast<uint32_t>(bytes[2]) << 16u) |
                   (static_cast<uint32_t>(bytes[3]) << 24u);
        }

        std::array<uint8_t, 4> encodeLe32(int32_t value)
        {
            const uint32_t word = static_cast<uint32_t>(value);
            return {
                static_cast<uint8_t>(word),
                static_cast<uint8_t>(word >> 8u),
                static_cast<uint8_t>(word >> 16u),
                static_cast<uint8_t>(word >> 24u),
            };
        }

        void writeLe32(uint8_t *bytes, uint32_t value)
        {
            bytes[0] = static_cast<uint8_t>(value);
            bytes[1] = static_cast<uint8_t>(value >> 8u);
            bytes[2] = static_cast<uint8_t>(value >> 16u);
            bytes[3] = static_cast<uint8_t>(value >> 24u);
        }

        uint32_t ceilDivide(uint64_t numerator, uint32_t denominator)
        {
            return static_cast<uint32_t>((numerator + denominator - 1u) / denominator);
        }

        uint32_t transferCompletionDelayMicroseconds(
            const CapcomSnddrvTransferBindings &bindings,
            uint32_t sizeBytes,
            uint32_t waitStrategy)
        {
            // sceSdVoiceTrans counts 16-bit words. PCSX2 2.6.3 models a plain
            // SPU2 DMA write at four IOP cycles per word; the SNDDRV polling
            // path observes completion on the next 100-us poll boundary.
            const uint64_t words = (static_cast<uint64_t>(sizeBytes) + 1u) / 2u;
            const uint64_t dmaCycles = words * bindings.dmaCyclesPer16BitWord;
            const uint32_t dmaMicroseconds = std::max(
                1u, ceilDivide(dmaCycles * 1'000'000u, bindings.iopClockHz));
            const uint32_t pollQuantum = waitStrategy != 0u
                                             ? bindings.pollDelayMicroseconds
                                             : bindings.blockingDelayMicroseconds;
            return ceilDivide(dmaMicroseconds, pollQuantum) * pollQuantum;
        }

        class CapcomSnddrvTransferService final : public IopService
        {
        public:
            CapcomSnddrvTransferService(IopHost &host, CapcomSnddrvTransferBindings bindings)
                : m_host(host), m_bindings(std::move(bindings)), m_sids{m_bindings.sid}
            {
            }

            [[nodiscard]] std::string_view name() const override
            {
                return m_bindings.serviceName;
            }

            [[nodiscard]] std::span<const uint32_t> sids() const override
            {
                return m_sids;
            }

            void reset() override
            {
                std::lock_guard<std::mutex> lock(m_mutex);
                m_transferCount = 0u;
                m_transferredBytes = 0u;
                m_failureCount = 0u;
                m_lastSource = 0u;
                m_lastDestination = 0u;
                m_lastSize = 0u;
                m_lastWaitStrategy = 0u;
                m_lastStatus = 0;
                m_emptyMailboxAcks = 0u;
                m_nonemptyMailboxGenerations = 0u;
                m_streamStartCommands = 0u;
                m_streamControlCommands = 0u;
                m_streamConfigurationBatches = 0u;
                m_playbackResponseBatches = 0u;
                m_activeStreamHandle = 0u;
                m_lastPlaybackCursor = 0u;
                m_lastPlaybackBytes = 0u;
                m_playbackHandles = {};
                m_playbackRingBases = {};
                m_pendingPlaybackCallbacks = {};
                m_pendingPlaybackCallbackCount = 0u;
                m_pendingPlaybackCallbacksReady = false;
                m_deferredPlaybackTransfer = {};
                m_hasDeferredPlaybackTransfer = false;
                ++m_resetGeneration;
            }

            [[nodiscard]] RpcResult handleRpc(const RpcRequest &request) override
            {
                if (request.sid != m_bindings.sid ||
                    (request.function & 0xFFFF0000u) != m_bindings.functionClass)
                {
                    return {};
                }

                RpcResult result{};
                result.handled = true;
                result.resultAddress = request.receive.address;
                result.serverDispatchPolicy = ServerDispatchPolicy::Suppress;

                int32_t status = -1;
                uint32_t source = 0u;
                uint32_t destination = 0u;
                uint32_t size = 0u;
                uint32_t waitStrategy = 0u;

                std::array<uint8_t, kRequestBytes> command{};
                if (request.send.address != 0u && request.send.size >= command.size() &&
                    m_host.readGuest(request.send.address, command.data(), command.size()))
                {
                    source = readLe32(command.data() + kSourceWord * sizeof(uint32_t));
                    destination = readLe32(command.data() + kDestinationWord * sizeof(uint32_t));
                    size = readLe32(command.data() + kSizeWord * sizeof(uint32_t));
                    waitStrategy = readLe32(command.data() + kWaitStrategyWord * sizeof(uint32_t));

                    if (source == 0u || destination == 0u || size == 0u ||
                        size > m_bindings.maximumTransferBytes)
                    {
                        status = -1;
                    }
                    else if (m_bindings.channel < 0)
                    {
                        status = -2;
                    }
                    else
                    {
                        std::vector<uint8_t> payload(size);
                        if (!m_host.readGuest(source, payload.data(), payload.size()) ||
                            !m_host.writeSpu2(destination, payload.data(), payload.size()))
                        {
                            status = -3;
                        }
                        else
                        {
                            status = 0;
                            result.completionDelayMicroseconds =
                                transferCompletionDelayMicroseconds(
                                    m_bindings, size, waitStrategy);
                            if (std::getenv("PS2X_SNDDRV_TRANSFER_TRACE") != nullptr)
                            {
                                const size_t nonzero = static_cast<size_t>(std::count_if(
                                    payload.begin(), payload.end(),
                                    [](uint8_t value) { return value != 0u; }));
                                std::ostringstream message;
                                message << "SNDDRV voice transfer source=0x" << std::hex << source
                                        << " destination=0x" << destination
                                        << " size=0x" << size << std::dec
                                        << " wait=" << waitStrategy
                                        << " completion_us=" << result.completionDelayMicroseconds
                                        << " nonzero=" << nonzero << " first16=";
                                const size_t previewBytes = std::min<size_t>(payload.size(), 16u);
                                for (size_t index = 0u; index < previewBytes; ++index)
                                {
                                    if (index != 0u)
                                        message << ':';
                                    message << std::hex << static_cast<uint32_t>(payload[index]);
                                }
                                m_host.log(LogLevel::Info, message.str());
                            }
                        }
                    }
                }

                if (request.receive.address != 0u && request.receive.size >= sizeof(uint32_t))
                {
                    const auto response = encodeLe32(status);
                    if (!m_host.writeGuest(request.receive.address,
                                           response.data(),
                                           response.size()))
                    {
                        result.resultAddress = 0u;
                    }
                }

                {
                    std::lock_guard<std::mutex> lock(m_mutex);
                    m_lastSource = source;
                    m_lastDestination = destination;
                    m_lastSize = size;
                    m_lastWaitStrategy = waitStrategy;
                    m_lastStatus = status;
                    if (status == 0)
                    {
                        ++m_transferCount;
                        m_transferredBytes += size;
                    }
                    else
                    {
                        ++m_failureCount;
                    }
                }

                return result;
            }

            void onSifTransfer(const SifTransfer &transfer) override
            {
                if (transfer.kind != SifTransferKind::SetDma ||
                    transfer.phase != SifTransferPhase::AfterCopy ||
                    m_bindings.mailboxBytes == 0u ||
                    transfer.size != m_bindings.mailboxBytes)
                {
                    return;
                }

                uint32_t source = 0u;
                uint32_t destination = 0u;
                if (!m_host.normalizeGuestAddress(transfer.sourceAddress, source) ||
                    !m_host.normalizeGuestAddress(transfer.destinationAddress, destination) ||
                    source == 0u ||
                    std::find(m_bindings.mailboxIopDestinations.begin(),
                              m_bindings.mailboxIopDestinations.end(),
                              destination) == m_bindings.mailboxIopDestinations.end())
                {
                    return;
                }

                std::array<uint8_t, sizeof(uint32_t)> bytes{};
                if (!m_host.readGuest(source, bytes.data(), bytes.size()))
                {
                    return;
                }
                const uint32_t commandCount = readLe32(bytes.data());
                if (destination == m_bindings.mailboxResponseIopDestination)
                {
                    std::lock_guard<std::mutex> lock(m_mutex);
                    if (m_pendingPlaybackCallbackCount != 0u &&
                        !m_pendingPlaybackCallbacksReady)
                    {
                        if (!m_hasDeferredPlaybackTransfer)
                        {
                            m_deferredPlaybackTransfer = transfer;
                            m_hasDeferredPlaybackTransfer = true;
                        }
                        return;
                    }
                }
                const bool traceMailbox =
                    std::getenv("PS2X_SNDDRV_MAILBOX_TRACE") != nullptr;
                uint32_t sequenceBefore = 0u;
                std::array<uint8_t, sizeof(uint32_t)> sequenceBytes{};
                if (traceMailbox &&
                    m_host.readGuest(source + m_bindings.mailboxSequenceOffset,
                                     sequenceBytes.data(), sequenceBytes.size()))
                {
                    sequenceBefore = readLe32(sequenceBytes.data());
                    std::ostringstream message;
                    message << "SNDDRV mailbox generation destination=0x" << std::hex
                            << destination << " source=0x" << source << std::dec
                            << " count=" << commandCount
                            << " sequence=" << sequenceBefore;
                    m_host.log(LogLevel::Info, message.str());
                }
                if (commandCount == 0u &&
                    destination == m_bindings.mailboxResponseIopDestination)
                {
                    std::array<uint8_t, 4u * kMailboxRecordBytes>
                        completedCallbacks{};
                    uint32_t completedCallbackCount = 0u;
                    {
                        std::lock_guard<std::mutex> lock(m_mutex);
                        if (m_pendingPlaybackCallbackCount != 0u &&
                            m_pendingPlaybackCallbacksReady)
                        {
                            completedCallbacks = m_pendingPlaybackCallbacks;
                            completedCallbackCount = m_pendingPlaybackCallbackCount;
                        }
                    }

                    if (completedCallbackCount != 0u)
                    {
                        const auto countBytes = encodeLe32(
                            static_cast<int32_t>(completedCallbackCount));
                        if (!m_host.writeGuest(source,
                                               countBytes.data(),
                                               countBytes.size()) ||
                            !m_host.writeGuest(source + kMailboxHeaderBytes,
                                               completedCallbacks.data(),
                                               completedCallbackCount *
                                                   kMailboxRecordBytes))
                        {
                            return;
                        }

                        std::lock_guard<std::mutex> lock(m_mutex);
                        m_pendingPlaybackCallbacks = {};
                        m_pendingPlaybackCallbackCount = 0u;
                        m_pendingPlaybackCallbacksReady = false;

                        if (traceMailbox)
                        {
                            std::ostringstream message;
                            message << "SNDDRV mailbox returned pending callbacks count="
                                    << completedCallbackCount
                                    << " on empty generation sequence=" << sequenceBefore;
                            m_host.log(LogLevel::Info, message.str());
                        }
                    }
                }
                if (commandCount != 0u)
                {
                    if (traceMailbox && commandCount <= 4u)
                    {
                        std::array<uint8_t, 4u * kMailboxRecordBytes> traceRecords{};
                        if (m_host.readGuest(source + kMailboxHeaderBytes,
                                             traceRecords.data(),
                                             commandCount * kMailboxRecordBytes))
                        {
                            std::ostringstream message;
                            message << "SNDDRV mailbox raw";
                            for (uint32_t recordIndex = 0u;
                                 recordIndex < commandCount;
                                 ++recordIndex)
                            {
                                message << " record" << recordIndex << "={";
                                for (uint32_t wordIndex = 0u; wordIndex < 4u; ++wordIndex)
                                {
                                    if (wordIndex != 0u)
                                    {
                                        message << ',';
                                    }
                                    message << "0x" << std::hex
                                            << readLe32(traceRecords.data() +
                                                        recordIndex * kMailboxRecordBytes +
                                                        wordIndex * sizeof(uint32_t));
                                }
                                message << '}';
                            }
                            message << std::dec << " sequence=" << sequenceBefore;
                            m_host.log(LogLevel::Info, message.str());
                        }
                    }

                    bool supported = false;
                    uint32_t commandOpcode = 0u;
                    uint32_t streamHandle = 0u;
                    bool configurationBatch = false;
                    bool playbackResponseBatch = false;
                    std::array<uint8_t, 4u * kMailboxRecordBytes> playbackCallbacks{};
                    uint32_t playbackCallbackCount = 0u;
                    uint32_t playbackCursor = 0u;
                    uint32_t playbackBytes = 0u;
                    if (destination == m_bindings.mailboxCommandIopDestination &&
                        commandCount == 1u)
                    {
                        std::array<uint8_t, kMailboxRecordBytes> record{};
                        if (m_host.readGuest(source + kMailboxHeaderBytes,
                                             record.data(),
                                             record.size()))
                        {
                            const uint32_t opcode = readLe32(record.data());
                            commandOpcode = opcode;
                            streamHandle = readLe32(record.data() + 4u);
                            const uint32_t start = readLe32(record.data() + 8u);
                            const uint32_t reserved = readLe32(record.data() + 12u);
                            supported = streamHandle != 0u && reserved == 0u &&
                                        ((opcode == kStreamStartOpcode && start == 1u) ||
                                         (opcode == kStreamControlOpcode && start == 0u));
                        }
                    }
                    else if (destination == m_bindings.mailboxCommandIopDestination &&
                             commandCount == 4u)
                    {
                        std::array<uint8_t, 4u * kMailboxRecordBytes> records{};
                        if (m_host.readGuest(source + kMailboxHeaderBytes,
                                             records.data(),
                                             records.size()))
                        {
                            const auto word = [&](uint32_t record, uint32_t field) {
                                return readLe32(records.data() +
                                                record * kMailboxRecordBytes + field * 4u);
                            };
                            streamHandle = word(0u, 1u);
                            supported = streamHandle != 0u &&
                                        word(0u, 0u) == 8u && word(0u, 2u) == 0u && word(0u, 3u) == 0u &&
                                        word(1u, 0u) == 4u && word(1u, 1u) == streamHandle &&
                                        word(1u, 2u) == 0x0000BB80u && word(1u, 3u) == 0u &&
                                        word(2u, 0u) == 9u && word(2u, 1u) == streamHandle &&
                                        word(2u, 2u) == 0u && word(2u, 3u) == 0xFFFFFFF1u &&
                                        word(3u, 0u) == 9u && word(3u, 1u) == streamHandle &&
                                        word(3u, 2u) == 1u && word(3u, 3u) == 0x0000000Fu;
                            configurationBatch = supported;
                        }
                    }
                    else if (destination == m_bindings.mailboxResponseIopDestination &&
                             commandCount == 2u)
                    {
                        std::array<uint8_t, 2u * kMailboxRecordBytes> records{};
                        if (m_host.readGuest(source + kMailboxHeaderBytes,
                                             records.data(),
                                             records.size()))
                        {
                            const auto word = [&](uint32_t record, uint32_t field) {
                                return readLe32(records.data() +
                                                record * kMailboxRecordBytes + field * 4u);
                            };
                            const uint32_t firstHandle = word(0u, 1u);
                            const uint32_t secondHandle = word(1u, 1u);
                            const uint32_t firstCursor = word(0u, 2u);
                            const uint32_t secondCursor = word(1u, 2u);
                            const uint32_t firstBytes = word(0u, 3u);
                            const uint32_t secondBytes = word(1u, 3u);
                            supported = word(0u, 0u) == 0x100u &&
                                        word(1u, 0u) == 0x100u &&
                                        firstHandle != 0u && secondHandle - firstHandle == 0x40u &&
                                        firstCursor != 0u && secondCursor - firstCursor == 0x4100u &&
                                        firstBytes != 0u && firstBytes <= 0x4000u &&
                                        // The cathedral's final partial ADX block is
                                        // 0x4C0 bytes. PCSX2 advances the matching
                                        // record-owned ring through this block, so
                                        // the protocol's demonstrated granularity is
                                        // 0x40 rather than 0x80 bytes.
                                        (firstBytes & 0x3Fu) == 0u && secondBytes == firstBytes;
                            playbackResponseBatch = supported;
                            playbackCursor = firstCursor;
                            playbackBytes = firstBytes;

                            if (supported)
                            {
                                const std::array<uint32_t, 2> handles{
                                    firstHandle, secondHandle};
                                std::array<uint32_t, 2> callbackObjects{};
                                const auto handlePair = std::find(
                                    m_bindings.playbackDescriptorHandles.begin(),
                                    m_bindings.playbackDescriptorHandles.end(),
                                    handles);
                                if (handlePair == m_bindings.playbackDescriptorHandles.end())
                                {
                                    supported = false;
                                }
                                else
                                {
                                    callbackObjects = m_bindings.playbackCallbackObjects[
                                        static_cast<size_t>(std::distance(
                                            m_bindings.playbackDescriptorHandles.begin(),
                                            handlePair))];
                                }
                                std::array<uint32_t, 2> playbackHandles{};
                                std::array<uint32_t, 2> ringBases{};
                                uint32_t pendingCallbackCount = 0u;
                                {
                                    std::lock_guard<std::mutex> lock(m_mutex);
                                    playbackHandles = m_playbackHandles;
                                    ringBases = m_playbackRingBases;
                                    pendingCallbackCount = m_pendingPlaybackCallbackCount;
                                    if (playbackHandles[0] == 0u)
                                    {
                                        playbackHandles = handles;
                                    }
                                    else if (playbackHandles != handles)
                                    {
                                        // The game owns multiple two-channel ADX
                                        // descriptor pairs.  A freshly selected pair
                                        // has its own 16 KiB ring bases, but it is not
                                        // safe to retire a pair while callbacks for it
                                        // are still outstanding.
                                        if (pendingCallbackCount != 0u)
                                        {
                                            supported = false;
                                        }
                                        else
                                        {
                                            playbackHandles = handles;
                                            ringBases = {};
                                        }
                                    }
                                }
                                const std::array<uint32_t, 2> cursors{firstCursor, secondCursor};
                                for (uint32_t index = 0u; index < cursors.size() && supported; ++index)
                                {
                                    if (ringBases[index] == 0u)
                                    {
                                        ringBases[index] = cursors[index];
                                    }
                                    const uint64_t ringEnd =
                                        static_cast<uint64_t>(ringBases[index]) +
                                        m_bindings.playbackRingBytes;
                                    if (cursors[index] < ringBases[index] ||
                                        cursors[index] >= ringEnd)
                                    {
                                        supported = false;
                                        break;
                                    }

                                    uint32_t cursor = cursors[index];
                                    uint32_t remaining = firstBytes;
                                    while (remaining != 0u)
                                    {
                                        if (playbackCallbackCount >= 4u)
                                        {
                                            supported = false;
                                            break;
                                        }
                                        const uint32_t contiguous = static_cast<uint32_t>(ringEnd - cursor);
                                        const uint32_t chunk = std::min(remaining, contiguous);
                                        uint8_t *record = playbackCallbacks.data() +
                                                          playbackCallbackCount * kMailboxRecordBytes;
                                        writeLe32(record, 0u);
                                        writeLe32(record + 4u, callbackObjects[index]);
                                        writeLe32(record + 8u, cursor);
                                        writeLe32(record + 12u, chunk);
                                        ++playbackCallbackCount;
                                        remaining -= chunk;
                                        cursor = ringBases[index];
                                    }
                                }

                                if (supported)
                                {
                                    // PCSX2 2.6.3 oracle captures show that an
                                    // outgoing opcode-0x100 request remains visible
                                    // for one mailbox generation.  The following
                                    // generation returns its opcode-zero callback
                                    // records.  Returning the callbacks in this same
                                    // generation keeps the guest's 16 KiB playback
                                    // queue falsely full and prevents movie startup.
                                    std::array<uint8_t, 4u * kMailboxRecordBytes>
                                        completedCallbacks{};
                                    uint32_t completedCallbackCount = 0u;
                                    {
                                        std::lock_guard<std::mutex> lock(m_mutex);
                                        if (m_pendingPlaybackCallbackCount != 0u &&
                                            m_pendingPlaybackCallbacksReady)
                                        {
                                            completedCallbacks = m_pendingPlaybackCallbacks;
                                            completedCallbackCount =
                                                m_pendingPlaybackCallbackCount;
                                        }
                                    }

                                    const auto countBytes = encodeLe32(
                                        static_cast<int32_t>(completedCallbackCount));
                                    supported = m_host.writeGuest(source,
                                                                  countBytes.data(),
                                                                  countBytes.size()) &&
                                                (completedCallbackCount == 0u ||
                                                 m_host.writeGuest(
                                                     source + kMailboxHeaderBytes,
                                                     completedCallbacks.data(),
                                                     completedCallbackCount *
                                                         kMailboxRecordBytes));
                                    if (supported)
                                    {
                                        const uint32_t playbackDelayMicroseconds =
                                            m_host.submitSpu2StereoStream(
                                                firstCursor,
                                                secondCursor,
                                                firstBytes,
                                                ringBases[0],
                                                ringBases[1],
                                                 m_bindings.playbackRingBytes,
                                                 0x0000BB80u);
                                        uint64_t resetGeneration = 0u;
                                        {
                                            std::lock_guard<std::mutex> lock(m_mutex);
                                            m_playbackHandles = playbackHandles;
                                            m_playbackRingBases = ringBases;
                                            m_pendingPlaybackCallbacks = playbackCallbacks;
                                            m_pendingPlaybackCallbackCount =
                                                playbackCallbackCount;
                                            m_pendingPlaybackCallbacksReady =
                                                playbackDelayMicroseconds == 0u;
                                            resetGeneration = m_resetGeneration;
                                        }

                                        if (playbackDelayMicroseconds != 0u)
                                        {
                                            auto markReady = [this, resetGeneration]()
                                            {
                                                SifTransfer deferred{};
                                                bool replayDeferred = false;
                                                {
                                                    std::lock_guard<std::mutex> lock(m_mutex);
                                                    if (m_resetGeneration != resetGeneration ||
                                                        m_pendingPlaybackCallbackCount == 0u)
                                                    {
                                                        return;
                                                    }
                                                    m_pendingPlaybackCallbacksReady = true;
                                                    if (m_hasDeferredPlaybackTransfer)
                                                    {
                                                        deferred = m_deferredPlaybackTransfer;
                                                        m_deferredPlaybackTransfer = {};
                                                        m_hasDeferredPlaybackTransfer = false;
                                                        replayDeferred = true;
                                                    }
                                                }
                                                if (replayDeferred)
                                                {
                                                    onSifTransfer(deferred);
                                                }
                                            };
                                            if (!m_host.scheduleHostCallback(
                                                    playbackDelayMicroseconds, markReady))
                                            {
                                                markReady();
                                            }
                                        }
                                    }

                                    if (traceMailbox && supported)
                                    {
                                        std::ostringstream message;
                                        message << "SNDDRV mailbox playback request first_cursor=0x"
                                                << std::hex << firstCursor
                                                << " second_cursor=0x" << secondCursor
                                                << " bytes=0x" << firstBytes << std::dec
                                                << " returned_callbacks="
                                                << completedCallbackCount
                                                << " queued_callbacks="
                                                << playbackCallbackCount
                                                << " sequence=" << sequenceBefore;
                                        m_host.log(LogLevel::Info, message.str());
                                    }
                                }
                            }
                        }
                    }

                    std::lock_guard<std::mutex> lock(m_mutex);
                    ++m_nonemptyMailboxGenerations;
                    if (!supported)
                    {
                        return;
                    }
                    if (configurationBatch)
                    {
                        ++m_streamConfigurationBatches;
                        m_activeStreamHandle = streamHandle;
                    }
                    else if (playbackResponseBatch)
                    {
                        ++m_playbackResponseBatches;
                        m_lastPlaybackCursor = playbackCursor;
                        m_lastPlaybackBytes = playbackBytes;
                    }
                    else if (commandOpcode == kStreamStartOpcode)
                    {
                        ++m_streamStartCommands;
                        m_activeStreamHandle = streamHandle;
                    }
                    else
                    {
                        ++m_streamControlCommands;
                        m_activeStreamHandle = streamHandle;
                    }
                }

                const uint32_t sequenceAddress = source + m_bindings.mailboxSequenceOffset;
                if (!m_host.readGuest(sequenceAddress, bytes.data(), bytes.size()))
                {
                    return;
                }
                const auto response = encodeLe32(static_cast<int32_t>(readLe32(bytes.data()) + 1u));
                if (!m_host.writeGuest(sequenceAddress, response.data(), response.size()))
                {
                    return;
                }

                std::lock_guard<std::mutex> lock(m_mutex);
                if (commandCount == 0u)
                {
                    ++m_emptyMailboxAcks;
                }
            }

            void appendDebugMetrics(std::vector<DebugMetric> &metrics) const override
            {
                std::lock_guard<std::mutex> lock(m_mutex);
                metrics.push_back({"transfer_count", m_transferCount, false});
                metrics.push_back({"transferred_bytes", m_transferredBytes, false});
                metrics.push_back({"failure_count", m_failureCount, false});
                metrics.push_back({"last_source", m_lastSource, true});
                metrics.push_back({"last_destination", m_lastDestination, true});
                metrics.push_back({"last_size", m_lastSize, true});
                metrics.push_back({"last_wait_strategy", m_lastWaitStrategy, false});
                metrics.push_back({"last_status", static_cast<uint32_t>(m_lastStatus), true});
                metrics.push_back({"empty_mailbox_acks", m_emptyMailboxAcks, false});
                metrics.push_back({"nonempty_mailbox_generations", m_nonemptyMailboxGenerations, false});
                metrics.push_back({"stream_start_commands", m_streamStartCommands, false});
                metrics.push_back({"stream_control_commands", m_streamControlCommands, false});
                metrics.push_back({"stream_configuration_batches", m_streamConfigurationBatches, false});
                metrics.push_back({"playback_response_batches", m_playbackResponseBatches, false});
                metrics.push_back({"active_stream_handle", m_activeStreamHandle, true});
                metrics.push_back({"last_playback_cursor", m_lastPlaybackCursor, true});
                metrics.push_back({"last_playback_bytes", m_lastPlaybackBytes, true});
                metrics.push_back({"pending_playback_callback_count",
                                   m_pendingPlaybackCallbackCount,
                                   false});
            }

        private:
            IopHost &m_host;
            CapcomSnddrvTransferBindings m_bindings;
            std::array<uint32_t, 1> m_sids;
            mutable std::mutex m_mutex;
            uint64_t m_transferCount = 0u;
            uint64_t m_transferredBytes = 0u;
            uint64_t m_failureCount = 0u;
            uint32_t m_lastSource = 0u;
            uint32_t m_lastDestination = 0u;
            uint32_t m_lastSize = 0u;
            uint32_t m_lastWaitStrategy = 0u;
            int32_t m_lastStatus = 0;
            uint64_t m_emptyMailboxAcks = 0u;
            uint64_t m_nonemptyMailboxGenerations = 0u;
            uint64_t m_streamStartCommands = 0u;
            uint64_t m_streamControlCommands = 0u;
            uint64_t m_streamConfigurationBatches = 0u;
            uint64_t m_playbackResponseBatches = 0u;
            uint32_t m_activeStreamHandle = 0u;
            uint32_t m_lastPlaybackCursor = 0u;
            uint32_t m_lastPlaybackBytes = 0u;
            std::array<uint32_t, 2> m_playbackHandles{};
            std::array<uint32_t, 2> m_playbackRingBases{};
            std::array<uint8_t, 4u * kMailboxRecordBytes> m_pendingPlaybackCallbacks{};
            uint32_t m_pendingPlaybackCallbackCount = 0u;
            bool m_pendingPlaybackCallbacksReady = false;
            SifTransfer m_deferredPlaybackTransfer{};
            bool m_hasDeferredPlaybackTransfer = false;
            uint64_t m_resetGeneration = 0u;
        };
    }

    std::unique_ptr<IopService> createCapcomSnddrvTransferService(
        IopHost &host,
        CapcomSnddrvTransferBindings bindings)
    {
        if (bindings.serviceName.empty() || bindings.sid == 0u ||
            (bindings.functionClass & 0xFFFFu) != 0u ||
            bindings.maximumTransferBytes == 0u ||
            bindings.iopClockHz == 0u ||
            bindings.dmaCyclesPer16BitWord == 0u ||
            bindings.pollDelayMicroseconds == 0u ||
            bindings.blockingDelayMicroseconds == 0u)
        {
            throw std::invalid_argument("invalid Capcom SNDDRV transfer bindings");
        }
        if ((bindings.mailboxBytes == 0u) != bindings.mailboxIopDestinations.empty() ||
            (bindings.mailboxBytes != 0u &&
             (bindings.mailboxSequenceOffset > bindings.mailboxBytes - sizeof(uint32_t) ||
              bindings.mailboxCommandIopDestination == 0u ||
              bindings.mailboxResponseIopDestination == 0u ||
              std::any_of(bindings.playbackDescriptorHandles.begin(),
                          bindings.playbackDescriptorHandles.end(),
                          [](const auto &pair) { return pair[0] == 0u || pair[1] == 0u; }) ||
              std::any_of(bindings.playbackCallbackObjects.begin(),
                          bindings.playbackCallbackObjects.end(),
                          [](const auto &pair) { return pair[0] == 0u || pair[1] == 0u; }) ||
              bindings.playbackRingBytes == 0u ||
              std::find(bindings.mailboxIopDestinations.begin(),
                        bindings.mailboxIopDestinations.end(),
                        bindings.mailboxCommandIopDestination) ==
                  bindings.mailboxIopDestinations.end() ||
              std::find(bindings.mailboxIopDestinations.begin(),
                        bindings.mailboxIopDestinations.end(),
                        bindings.mailboxResponseIopDestination) ==
                  bindings.mailboxIopDestinations.end())))
        {
            throw std::invalid_argument("invalid Capcom SNDDRV mailbox bindings");
        }
        return std::make_unique<CapcomSnddrvTransferService>(host, std::move(bindings));
    }
}
