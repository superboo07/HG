#include "module_factories.h"

#include <algorithm>
#include <array>
#include <cstdlib>
#include <cstdint>
#include <iomanip>
#include <iostream>

namespace ps2x::iop::detail
{
    namespace
    {
        constexpr uint32_t kLibSdSid = 0x80000701u;

        class LibSdService final : public IopService
        {
        public:
            explicit LibSdService(IopHost &host)
                : m_host(host)
            {
            }

            [[nodiscard]] std::string_view name() const override
            {
                return "libsd";
            }

            [[nodiscard]] std::span<const uint32_t> sids() const override
            {
                return kSids;
            }

            void reset() override
            {
            }

            [[nodiscard]] RpcResult handleRpc(const RpcRequest &request) override
            {
                if (request.sid != kLibSdSid)
                {
                    return {};
                }

                m_host.audioCommand(request.sid,
                                    request.function,
                                    request.send,
                                    request.receive);

                if (std::getenv("PS2X_LIBSD_TRACE") != nullptr)
                {
                    std::array<uint8_t, 64> bytes{};
                    const size_t byteCount = std::min<size_t>(request.send.size, bytes.size());
                    const bool readable = byteCount == 0u ||
                                          m_host.readGuest(request.send.address, bytes.data(), byteCount);
                    std::cerr << "[libsd-rpc] function=0x" << std::hex << request.function
                              << " send=0x" << request.send.address
                              << " sendSize=0x" << request.send.size
                              << " receive=0x" << request.receive.address
                              << " receiveSize=0x" << request.receive.size
                              << " bytes=";
                    if (readable)
                    {
                        for (size_t index = 0; index < byteCount; ++index)
                        {
                            std::cerr << std::setw(2) << std::setfill('0')
                                      << static_cast<uint32_t>(bytes[index]);
                        }
                    }
                    else
                    {
                        std::cerr << "unreadable";
                    }
                    std::cerr << std::setfill(' ') << std::dec << std::endl;
                }

                RpcResult result;
                result.handled = true;
                result.resultAddress = request.receive.address;
                return result;
            }

        private:
            inline static constexpr std::array<uint32_t, 1> kSids{kLibSdSid};

            IopHost &m_host;
        };
    }

    std::unique_ptr<IopService> createLibSdService(IopHost &host)
    {
        return std::make_unique<LibSdService>(host);
    }
}
