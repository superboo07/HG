#include "module_factories.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdint>
#include <cstring>
#include <string>
#include <utility>
#include <vector>

namespace ps2x::iop::detail
{
    namespace
    {
        constexpr size_t kCdlFileBytes = 32u;
        constexpr size_t kCdlFileNameOffset = 8u;
        constexpr size_t kCdlFileNameBytes = 16u;
        constexpr size_t kCdlFileDateOffset = 24u;

        std::string normalizeCdPath(std::string path)
        {
            std::replace(path.begin(), path.end(), '\\', '/');
            std::transform(path.begin(), path.end(), path.begin(), [](unsigned char value)
                           { return static_cast<char>(std::tolower(value)); });

            constexpr std::array<std::string_view, 2> prefixes = {"cdrom0:", "cdrom:"};
            for (const std::string_view prefix : prefixes)
            {
                if (path.rfind(prefix, 0u) == 0u)
                {
                    path.erase(0u, prefix.size());
                    break;
                }
            }
            while (!path.empty() && path.front() == '/')
            {
                path.erase(path.begin());
            }
            return path;
        }

        std::string leafName(std::string path)
        {
            std::replace(path.begin(), path.end(), '\\', '/');
            const size_t separator = path.find_last_of('/');
            if (separator != std::string::npos)
            {
                path.erase(0u, separator + 1u);
            }
            return path;
        }

        class CdSearchFileService final : public IopService
        {
        public:
            CdSearchFileService(IopHost &host, CdSearchFileBindings bindings)
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
                m_searchCalls = 0u;
                m_successfulSearches = 0u;
                m_failedSearches = 0u;
            }

            [[nodiscard]] RpcResult handleRpc(const RpcRequest &request) override
            {
                RpcResult result;
                if (request.sid != m_bindings.sid || request.function != m_bindings.function)
                {
                    return result;
                }

                result.handled = true;
                result.resultAddress = request.receive.address;
                ++m_searchCalls;

                const uint32_t availablePathBytes =
                    request.send.size > m_bindings.pathOffset
                        ? std::min(request.send.size - m_bindings.pathOffset, m_bindings.pathBytes)
                        : 0u;
                const std::string requestedPath =
                    readGuestString(request.send.address + m_bindings.pathOffset, availablePathBytes);
                const std::string normalizedRequestedPath = normalizeCdPath(requestedPath);
                const auto record = std::find_if(
                    m_bindings.files.begin(), m_bindings.files.end(), [&](const CdSearchFileRecord &candidate)
                    { return normalizeCdPath(candidate.guestPath) == normalizedRequestedPath; });

                if (record == m_bindings.files.end() || !validateHostFile(requestedPath, *record) ||
                    !m_host.registerCdFile(requestedPath, record->lsn, record->size) ||
                    !writeSearchResult(request.send, *record))
                {
                    ++m_failedSearches;
                    writeReceiveValue(request.receive, m_bindings.failureValue);
                    return result;
                }

                ++m_successfulSearches;
                writeReceiveValue(request.receive, m_bindings.successValue);
                return result;
            }

            void appendDebugMetrics(std::vector<DebugMetric> &metrics) const override
            {
                metrics.push_back({"search_calls", m_searchCalls, false});
                metrics.push_back({"successful_searches", m_successfulSearches, false});
                metrics.push_back({"failed_searches", m_failedSearches, false});
            }

        private:
            [[nodiscard]] std::string readGuestString(uint32_t address, uint32_t maxBytes) const
            {
                if (address == 0u || maxBytes == 0u)
                {
                    return {};
                }
                std::vector<char> bytes(maxBytes);
                if (!m_host.readGuest(address, bytes.data(), bytes.size()))
                {
                    return {};
                }
                const auto terminator = std::find(bytes.begin(), bytes.end(), '\0');
                return std::string(bytes.begin(), terminator);
            }

            [[nodiscard]] bool validateHostFile(const std::string &requestedPath,
                                                const CdSearchFileRecord &record)
            {
                if (!m_bindings.validateHostFileSize)
                {
                    return true;
                }

                const std::string translated = m_host.translateGuestPath(requestedPath);
                const uint64_t file = m_host.openHostFile(translated);
                if (file == 0u)
                {
                    return false;
                }
                uint64_t size = 0u;
                const bool valid = m_host.hostFileSize(file, size) && size == record.size;
                m_host.closeHostFile(file);
                return valid;
            }

            [[nodiscard]] bool writeSearchResult(GuestBuffer send,
                                                 const CdSearchFileRecord &record)
            {
                if (send.address == 0u || send.size < m_bindings.resultBytes ||
                    m_bindings.resultBytes < kCdlFileBytes)
                {
                    return false;
                }

                std::vector<uint8_t> packed(m_bindings.resultBytes, 0u);
                std::memcpy(packed.data(), &record.lsn, sizeof(record.lsn));
                std::memcpy(packed.data() + 4u, &record.size, sizeof(record.size));

                const std::string leaf = leafName(record.guestPath);
                const size_t nameBytes = std::min(leaf.size(), kCdlFileNameBytes - 1u);
                std::memcpy(packed.data() + kCdlFileNameOffset, leaf.data(), nameBytes);
                std::memcpy(packed.data() + kCdlFileDateOffset,
                            record.date.data(), record.date.size());
                return m_host.writeGuest(send.address, packed.data(), packed.size());
            }

            void writeReceiveValue(GuestBuffer receive, uint32_t value)
            {
                if (receive.address != 0u &&
                    receive.size >= m_bindings.receiveResultOffset + sizeof(value))
                {
                    (void)m_host.writeGuest(receive.address + m_bindings.receiveResultOffset,
                                            &value, sizeof(value));
                }
            }

            IopHost &m_host;
            CdSearchFileBindings m_bindings;
            std::array<uint32_t, 1> m_sids{};
            uint64_t m_searchCalls = 0u;
            uint64_t m_successfulSearches = 0u;
            uint64_t m_failedSearches = 0u;
        };
    }

    std::unique_ptr<IopService> createCdSearchFileService(IopHost &host,
                                                          CdSearchFileBindings bindings)
    {
        return std::make_unique<CdSearchFileService>(host, std::move(bindings));
    }
}
