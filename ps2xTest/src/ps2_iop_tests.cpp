#include "MiniTest.h"
#include "ps2x/iop/iop_subsystem.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <limits>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#elif defined(__linux__)
#include <dlfcn.h>
#endif

namespace
{
    using namespace ps2x::iop;

    constexpr uint32_t kSyntheticSid = 0xF00DCAFEu;
    constexpr uint32_t kCoreCollisionSid = 0x80001300u;
    constexpr uint32_t kSyntheticFunction = 0x42u;
    constexpr uint32_t kCoreCollisionFunction = 0x99u;
    constexpr uint32_t kSyntheticEntryPoint = 0x00123456u;
    constexpr uint32_t kSpecificRecvXEntryPoint = kSyntheticEntryPoint + 0x100u;
    constexpr uint32_t kSyntheticCrc32 = 0xA1B2C3D4u;
    constexpr uint32_t kResponseXor = 0xA5A55A5Au;
    constexpr uint32_t kCoreCollisionResponse = 0xC0DEF00Du;

    class FakeIopHost final : public IopHost
    {
    public:
        explicit FakeIopHost(size_t memorySize = 0x10000u)
            : memory(memorySize, 0u)
        {
        }

        bool readGuest(uint32_t address, void *destination, size_t size) const override
        {
            if ((!destination && size != 0u) || !contains(address, size))
            {
                return false;
            }
            if (size != 0u)
            {
                std::memcpy(destination, memory.data() + address, size);
            }
            return true;
        }

        bool writeGuest(uint32_t address, const void *source, size_t size) override
        {
            if ((!source && size != 0u) || !contains(address, size))
            {
                return false;
            }
            if (size != 0u)
            {
                std::memcpy(memory.data() + address, source, size);
            }
            return true;
        }

        bool zeroGuest(uint32_t address, size_t size) override
        {
            if (!contains(address, size))
            {
                return false;
            }
            std::fill(memory.begin() + address, memory.begin() + address + size, 0u);
            return true;
        }

        bool normalizeGuestAddress(uint32_t address, uint32_t &normalized) const override
        {
            normalized = address & 0x1FFFFFFFu;
            return normalized < memory.size();
        }

        uint32_t allocateIopHandle(IopHandleKind kind) override
        {
            const uint32_t value = nextHandle;
            nextHandle += (kind == IopHandleKind::RpcPacket) ? 0x40u : 0x80u;
            return value;
        }

        uint32_t allocateGuest(uint32_t size, uint32_t alignment) override
        {
            if (size == 0u)
            {
                return 0u;
            }
            const uint64_t effectiveAlignment = alignment == 0u ? 1u : alignment;
            const uint64_t aligned = ((static_cast<uint64_t>(nextGuestAddress) + effectiveAlignment - 1u) /
                                      effectiveAlignment) *
                                     effectiveAlignment;
            if (aligned + size > memory.size())
            {
                return 0u;
            }
            nextGuestAddress = static_cast<uint32_t>(aligned + size);
            guestAllocations.push_back(static_cast<uint32_t>(aligned));
            return static_cast<uint32_t>(aligned);
        }

        void freeGuest(uint32_t address) override
        {
            freedGuestAddresses.push_back(address);
        }

        void audioCommand(uint32_t sid,
                          uint32_t function,
                          GuestBuffer send,
                          GuestBuffer receive) override
        {
            lastAudioSid = sid;
            lastAudioFunction = function;
            lastAudioSend = send;
            lastAudioReceive = receive;
            ++audioCalls;
        }

        bool writeSpu2(uint32_t address, const void *source, size_t size) override
        {
            if ((!source && size != 0u) || !spu2WriteSucceeds ||
                address > spu2Memory.size() || size > spu2Memory.size() - address)
            {
                return false;
            }
            if (size != 0u)
            {
                std::memcpy(spu2Memory.data() + address, source, size);
            }
            ++spu2Writes;
            lastSpu2Address = address;
            lastSpu2Size = size;
            return true;
        }

        uint32_t submitSpu2StereoStream(uint32_t firstCursor,
                                        uint32_t secondCursor,
                                        uint32_t bytesPerChannel,
                                        uint32_t firstRingBase,
                                        uint32_t secondRingBase,
                                        uint32_t ringBytes,
                                        uint32_t sampleRate) override
        {
            ++spu2StreamSubmissions;
            lastSpu2FirstCursor = firstCursor;
            lastSpu2SecondCursor = secondCursor;
            lastSpu2StreamBytes = bytesPerChannel;
            lastSpu2FirstRingBase = firstRingBase;
            lastSpu2SecondRingBase = secondRingBase;
            lastSpu2RingBytes = ringBytes;
            lastSpu2SampleRate = sampleRate;
            return 0u;
        }

        std::string hostPath(HostPathKind kind) const override
        {
            switch (kind)
            {
            case HostPathKind::CdRoot:
                return "fake/cd";
            case HostPathKind::CdImage:
                return "fake/disc.iso";
            case HostPathKind::HostRoot:
                return "fake/host";
            case HostPathKind::MemoryCardRoot:
                return "fake/mc0";
            default:
                return "fake/elf";
            }
        }

        std::string translateGuestPath(std::string_view path) const override
        {
            return "translated/" + std::string(path);
        }

        uint64_t openHostFile(std::string_view path) override
        {
            const auto file = hostFileContents.find(std::string(path));
            if (file == hostFileContents.end())
            {
                return 0u;
            }
            const uint64_t handle = nextHostFileHandle++;
            openHostFiles.emplace(handle, file->first);
            return handle;
        }

        bool hostFileSize(uint64_t handle, uint64_t &size) const override
        {
            size = 0u;
            const auto open = openHostFiles.find(handle);
            if (open == openHostFiles.end())
            {
                return false;
            }
            const auto file = hostFileContents.find(open->second);
            if (file == hostFileContents.end())
            {
                return false;
            }
            const auto overriddenSize = hostFileSizes.find(open->second);
            if (overriddenSize != hostFileSizes.end())
            {
                size = overriddenSize->second;
                return true;
            }
            size = file->second.size();
            return true;
        }

        bool readHostFile(uint64_t handle,
                          uint64_t offset,
                          void *destination,
                          size_t size,
                          size_t &bytesRead) override
        {
            bytesRead = 0u;
            if (!destination && size != 0u)
            {
                return false;
            }
            const auto open = openHostFiles.find(handle);
            if (open == openHostFiles.end())
            {
                return false;
            }
            const auto file = hostFileContents.find(open->second);
            if (file == hostFileContents.end() || offset > file->second.size())
            {
                return false;
            }
            bytesRead = std::min<size_t>(size, file->second.size() - static_cast<size_t>(offset));
            if (bytesRead != 0u)
            {
                std::memcpy(destination,
                            file->second.data() + static_cast<size_t>(offset),
                            bytesRead);
            }
            return true;
        }

        void closeHostFile(uint64_t handle) override
        {
            if (openHostFiles.erase(handle) != 0u)
            {
                closedHostFileHandles.push_back(handle);
            }
        }

        bool registerCdFile(std::string_view path,
                            uint32_t lsn,
                            uint32_t size) override
        {
            registeredCdFiles.emplace_back(std::string(path), lsn, size);
            return cdFileRegistrationSucceeds;
        }

        int32_t memoryCard(const MemoryCardRequest &request) override
        {
            lastMemoryCardRequest = request;
            ++memoryCardCalls;
            return 0;
        }

        bool hasGuestFunction(uint32_t address) const override
        {
            return address == guestFunctionAddress;
        }

        bool invokeGuestFunction(uint64_t callToken,
                                 uint32_t address,
                                 uint32_t a0,
                                 uint32_t a1,
                                 uint32_t a2,
                                 uint32_t a3,
                                 uint32_t *resultAddress) override
        {
            if (!hasGuestFunction(address))
            {
                return false;
            }
            lastCallToken = callToken;
            lastGuestArguments = {a0, a1, a2, a3};
            if (resultAddress)
            {
                *resultAddress = guestFunctionResult;
            }
            return true;
        }

        void log(LogLevel level, std::string_view message) override
        {
            logs.emplace_back(level, std::string(message));
        }

        bool writeWord(uint32_t address, uint32_t value)
        {
            return writeGuest(address, &value, sizeof(value));
        }

        uint32_t readWord(uint32_t address) const
        {
            uint32_t value = 0u;
            (void)readGuest(address, &value, sizeof(value));
            return value;
        }

        bool hasLog(std::string_view expected) const
        {
            return std::any_of(logs.begin(), logs.end(), [&](const auto &entry)
                               { return entry.second == expected; });
        }

        std::vector<uint8_t> memory;
        uint32_t nextHandle = 0x8000u;
        uint32_t nextGuestAddress = 0x4000u;
        std::vector<uint32_t> guestAllocations;
        std::vector<uint32_t> freedGuestAddresses;
        uint32_t audioCalls = 0u;
        uint32_t lastAudioSid = 0u;
        uint32_t lastAudioFunction = 0u;
        GuestBuffer lastAudioSend{};
        GuestBuffer lastAudioReceive{};
        std::vector<uint8_t> spu2Memory = std::vector<uint8_t>(2u * 1024u * 1024u, 0u);
        uint32_t spu2Writes = 0u;
        uint32_t lastSpu2Address = 0u;
        size_t lastSpu2Size = 0u;
        bool spu2WriteSucceeds = true;
        uint32_t spu2StreamSubmissions = 0u;
        uint32_t lastSpu2FirstCursor = 0u;
        uint32_t lastSpu2SecondCursor = 0u;
        uint32_t lastSpu2StreamBytes = 0u;
        uint32_t lastSpu2FirstRingBase = 0u;
        uint32_t lastSpu2SecondRingBase = 0u;
        uint32_t lastSpu2RingBytes = 0u;
        uint32_t lastSpu2SampleRate = 0u;
        uint32_t memoryCardCalls = 0u;
        MemoryCardRequest lastMemoryCardRequest{};
        uint32_t guestFunctionAddress = 0x2000u;
        uint32_t guestFunctionResult = 0x3000u;
        uint64_t lastCallToken = 0u;
        std::vector<uint32_t> lastGuestArguments;
        std::vector<std::pair<LogLevel, std::string>> logs;
        std::unordered_map<std::string, std::vector<uint8_t>> hostFileContents;
        std::unordered_map<std::string, uint64_t> hostFileSizes;
        std::unordered_map<uint64_t, std::string> openHostFiles;
        std::vector<uint64_t> closedHostFileHandles;
        struct RegisteredCdFile
        {
            std::string path;
            uint32_t lsn = 0u;
            uint32_t size = 0u;
        };
        std::vector<RegisteredCdFile> registeredCdFiles;
        bool cdFileRegistrationSucceeds = true;
        uint64_t nextHostFileHandle = 1u;

    private:
        bool contains(uint32_t address, size_t size) const
        {
            const uint64_t end = static_cast<uint64_t>(address) + static_cast<uint64_t>(size);
            return end <= memory.size();
        }
    };

    bool containsDiagnostic(const DebugSnapshot &snapshot, std::string_view text)
    {
        return std::any_of(snapshot.diagnostics.begin(), snapshot.diagnostics.end(), [&](const std::string &diagnostic)
                           { return diagnostic.find(text) != std::string::npos; });
    }

    const DebugService *findService(const DebugSnapshot &snapshot, std::string_view name)
    {
        const auto it = std::find_if(snapshot.services.begin(), snapshot.services.end(), [&](const DebugService &service)
                                     { return service.name == name; });
        return it == snapshot.services.end() ? nullptr : &*it;
    }

    uint64_t metricValue(const DebugService &service, std::string_view name)
    {
        const auto it = std::find_if(service.metrics.begin(), service.metrics.end(), [&](const DebugMetric &metric)
                                     { return metric.name == name; });
        return it == service.metrics.end() ? std::numeric_limits<uint64_t>::max() : it->value;
    }

    bool pluginModuleIsLoaded(const std::filesystem::path &path)
    {
#if defined(_WIN32)
        return GetModuleHandleW(path.c_str()) != nullptr;
#elif defined(__linux__)
        void *handle = dlopen(path.c_str(), RTLD_NOW | RTLD_NOLOAD);
        if (!handle)
        {
            return false;
        }
        dlclose(handle);
        return true;
#else
        (void)path;
        return false;
#endif
    }
}

void register_ps2_iop_tests()
{
    MiniTest::Case("PS2IopSubsystem", [](TestCase &tc)
    {
        tc.Run("unknown SID remains unhandled without a matching profile", [](TestCase &t)
        {
            FakeIopHost host;
            ps2x::iop::IopSubsystem subsystem(host);

            std::string error;
            const bool configured = subsystem.configure({"unmatched.elf", 0x100000u, 0x12345678u}, &error);
            t.IsTrue(configured, "configuring an unmatched game should keep core-only IOP services available");

            ps2x::iop::RpcRequest request{};
            request.sid = 0xDEADC0DEu;
            request.function = 0x99u;
            const ps2x::iop::RpcResult result = subsystem.handleRpc(request);
            t.IsFalse(result.handled, "an unknown SID should not be claimed by the IOP subsystem");
            t.Equals(result.resultAddress, 0u, "an unknown SID should not return a guest result address");
            t.IsFalse(result.signalNowaitCompletion, "an unknown SID should not signal nowait completion");
            t.Equals(result.callbackPolicy, ps2x::iop::CallbackPolicy::RuntimeDefault,
                     "an unknown SID should preserve runtime callback handling");

            const ps2x::iop::DebugSnapshot snapshot = subsystem.debugSnapshot();
            t.IsTrue(snapshot.activeProfile.empty(), "an unmatched game should not activate a profile");
            t.IsTrue(snapshot.activeProvider.empty(), "an unmatched game should not report a profile provider");
        });

        tc.Run("built-in profiles select by ELF basename and keep core services active", [](TestCase &t)
        {
            FakeIopHost host;
            ps2x::iop::IopSubsystem subsystem(host);
            std::string error;

            t.IsTrue(subsystem.configure({"SLUS_201.84", 0u, 0u}, &error),
                     "RECVX profile should match case-insensitively by basename");
            ps2x::iop::DebugSnapshot snapshot = subsystem.debugSnapshot();
            t.Equals(snapshot.activeProfile, std::string("recvx-us"),
                     "RECVX ELF should select its built-in profile");
            t.IsNotNull(findService(snapshot, "TSNDDRV"),
                        "RECVX profile should register TSNDDRV");
            t.IsNotNull(findService(snapshot, "CRI DTX"),
                        "RECVX profile should register CRI DTX");
            t.IsNotNull(findService(snapshot, "dbcman"),
                        "core DBCMAN should remain active with a game profile");
            t.IsNotNull(findService(snapshot, "libsd"),
                        "core LIBSD should remain active with a game profile");
            t.IsNotNull(findService(snapshot, "MCSERV"),
                        "core MCSERV should remain active with a game profile");

            error.clear();
            t.IsTrue(subsystem.configure({"slus_203.88", 0u, 0u}, &error),
                     "Fatal Frame profile should configure after a different game");
            snapshot = subsystem.debugSnapshot();
            t.Equals(snapshot.activeProfile, std::string("fatal-frame-us"),
                     "reload should replace the active profile");
            t.IsNull(findService(snapshot, "CRI DTX"),
                     "reload should destroy services from the previous profile");
            t.IsNotNull(findService(snapshot, "SDRDRV"),
                        "Fatal Frame profile should expose SDRDRV");
        });

        tc.Run("MCSERV GetDir carries all six EE n32 arguments", [](TestCase &t)
        {
            struct NameParameter
            {
                int32_t port;
                int32_t slot;
                int32_t flags;
                int32_t maxEntries;
                uint32_t pointer;
                char name[1024];
            };
            static_assert(sizeof(NameParameter) == 1044u);

            FakeIopHost host;
            ps2x::iop::IopSubsystem subsystem(host);
            std::string error;
            t.IsTrue(subsystem.configure({"unmatched.elf", 0x100000u, 0x12345678u}, &error),
                     "core MCSERV should configure without a game profile");

            constexpr uint32_t kSendAddress = 0x0800u;
            constexpr uint32_t kReceiveAddress = 0x1800u;
            constexpr uint32_t kTableAddress = 0x5000u;
            NameParameter parameter{};
            parameter.port = 1;
            parameter.slot = 0;
            parameter.flags = 3;
            parameter.maxEntries = 7;
            parameter.pointer = kTableAddress;
            std::memcpy(parameter.name, "/BISLUS-21075*", 16u);
            t.IsTrue(host.writeGuest(kSendAddress, &parameter, sizeof(parameter)),
                     "the complete MCSERV name parameter should fit in guest memory");

            ps2x::iop::RpcRequest request{};
            request.sid = 0x80000400u;
            request.function = 0x0Du;
            request.send = {kSendAddress, sizeof(parameter)};
            request.receive = {kReceiveAddress, sizeof(int32_t)};
            const ps2x::iop::RpcResult result = subsystem.handleRpc(request);

            t.IsTrue(result.handled, "MCSERV should claim GetDir");
            t.Equals(host.memoryCardCalls, 2u,
                     "MCSERV configure/reset and GetDir should each reach the memory-card seam");
            t.Equals(static_cast<uint32_t>(host.lastMemoryCardRequest.operation),
                     static_cast<uint32_t>(MemoryCardOperation::GetDir),
                     "MCSERV should issue a real GetDir operation");
            t.Equals(host.lastMemoryCardRequest.arguments[0], 1u, "GetDir should preserve port");
            t.Equals(host.lastMemoryCardRequest.arguments[1], 0u, "GetDir should preserve slot");
            t.Equals(host.lastMemoryCardRequest.arguments[2], kSendAddress + 20u,
                     "GetDir should pass the in-packet path address");
            t.Equals(host.lastMemoryCardRequest.arguments[3], 3u, "GetDir should preserve mode flags");
            t.Equals(host.lastMemoryCardRequest.arguments[4], 7u, "GetDir should carry maxent in $t0");
            t.Equals(host.lastMemoryCardRequest.arguments[5], kTableAddress,
                     "GetDir should carry the result table pointer in $t1");
            t.Equals(host.readWord(kReceiveAddress), 0u,
                     "MCSERV should return the memory-card operation result");
        });

        tc.Run("Haunting Ground CD search returns the verified DATA.CVM disc record", [](TestCase &t)
        {
            FakeIopHost host;
            constexpr std::string_view kGuestPath = "\\DATA.CVM;1";
            const std::string hostPath = "translated/" + std::string(kGuestPath);
            host.hostFileContents[hostPath] = {};
            host.hostFileSizes[hostPath] = 0x5E018000u;

            ps2x::iop::IopSubsystem subsystem(host);
            std::string error;
            t.IsTrue(subsystem.configure({"SLUS_210.75", 0x00100008u, 0xA295AF2Bu}, &error),
                     "the exact Haunting Ground ELF identity should configure");

            constexpr uint32_t kSendAddress = 0x0800u;
            constexpr uint32_t kReceiveAddress = 0x0A00u;
            constexpr uint32_t kPathOffset = 0x24u;
            t.IsTrue(host.writeGuest(kSendAddress + kPathOffset,
                                     kGuestPath.data(), kGuestPath.size() + 1u),
                     "the CD search path should fit in the request block");

            ps2x::iop::RpcRequest request{};
            request.sid = 0x80000597u;
            request.function = 0u;
            request.send = {kSendAddress, 0x12Cu};
            request.receive = {kReceiveAddress, sizeof(uint32_t)};
            const ps2x::iop::RpcResult result = subsystem.handleRpc(request);

            t.IsTrue(result.handled, "the game profile should claim the CD search RPC");
            t.Equals(result.resultAddress, kReceiveAddress,
                     "the RPC result should remain in the caller's receive buffer");
            t.Equals(host.readWord(kReceiveAddress), 1u,
                     "a matching extracted DATA.CVM should report success");
            t.Equals(host.readWord(kSendAddress + 0u), 0x00164972u,
                     "the result should expose the verified disc LSN");
            t.Equals(host.readWord(kSendAddress + 4u), 0x5E018000u,
                     "the result should expose the verified file size");

            std::array<uint8_t, 24> payload{};
            t.IsTrue(host.readGuest(kSendAddress + 8u, payload.data(), payload.size()),
                     "the packed name and date should be readable");
            constexpr std::array<uint8_t, 24> kExpectedPayload = {
                'D', 'A', 'T', 'A', '.', 'C', 'V', 'M', ';', '1', 0u, 0u, 0u, 0u, 0u, 0u,
                0x00u, 0x2Au, 0x25u, 0x0Au, 0x06u, 0x01u, 0xD5u, 0x07u,
            };
            t.IsTrue(payload == kExpectedPayload,
                     "the packed sceCdlFILE bytes should match the oracle response");
            t.Equals(host.closedHostFileHandles.size(), size_t{1},
                     "host validation should not leak its file handle");
            t.Equals(host.registeredCdFiles.size(), size_t{1},
                     "the verified disc file should be registered for sector reads");
            if (!host.registeredCdFiles.empty())
            {
                t.Equals(host.registeredCdFiles[0].path, std::string(kGuestPath),
                         "the sector mapping should retain the requested PS2 path");
                t.Equals(host.registeredCdFiles[0].lsn, 0x00164972u,
                         "the sector mapping should use the verified disc LSN");
                t.Equals(host.registeredCdFiles[0].size, 0x5E018000u,
                         "the sector mapping should use the verified file size");
            }

            const ps2x::iop::DebugSnapshot snapshot = subsystem.debugSnapshot();
            const ps2x::iop::DebugService *service =
                findService(snapshot, "Haunting Ground CD search");
            if (!service)
            {
                t.Fail("the CD search service should appear in the debug snapshot");
                return;
            }
            t.Equals(metricValue(*service, "successful_searches"), uint64_t{1},
                     "the successful lookup should be counted");
        });

        tc.Run("Haunting Ground CD search rejects mismatched extracted data", [](TestCase &t)
        {
            FakeIopHost host;
            constexpr std::string_view kGuestPath = "\\DATA.CVM;1";
            const std::string hostPath = "translated/" + std::string(kGuestPath);
            host.hostFileContents[hostPath] = {};
            host.hostFileSizes[hostPath] = 123u;

            ps2x::iop::IopSubsystem subsystem(host);
            std::string error;
            t.IsTrue(subsystem.configure({"SLUS_210.75", 0x00100008u, 0xA295AF2Bu}, &error),
                     "the exact Haunting Ground ELF identity should configure");

            constexpr uint32_t kSendAddress = 0x0800u;
            constexpr uint32_t kReceiveAddress = 0x0A00u;
            t.IsTrue(host.writeGuest(kSendAddress + 0x24u,
                                     kGuestPath.data(), kGuestPath.size() + 1u),
                     "the CD search path should fit in the request block");
            host.writeWord(kReceiveAddress, 0xFFFFFFFFu);

            ps2x::iop::RpcRequest request{};
            request.sid = 0x80000597u;
            request.function = 0u;
            request.send = {kSendAddress, 0x12Cu};
            request.receive = {kReceiveAddress, sizeof(uint32_t)};
            t.IsTrue(subsystem.handleRpc(request).handled,
                     "the malformed-data request should still complete deterministically");
            t.Equals(host.readWord(kReceiveAddress), 0u,
                     "a host file with the wrong size must not receive verified disc metadata");
            t.Equals(host.readWord(kSendAddress), 0u,
                     "failure must not fabricate a disc LSN");
        });

        tc.Run("Haunting Ground MODHSYN commands reach the portable audio seam", [](TestCase &t)
        {
            FakeIopHost host;
            ps2x::iop::IopSubsystem subsystem(host);
            std::string error;
            t.IsTrue(subsystem.configure({"SLUS_210.75", 0x00100008u, 0xA295AF2Bu}, &error),
                     "the exact Haunting Ground ELF identity should configure");

            constexpr uint32_t kSendAddress = 0x0800u;
            constexpr uint32_t kReceiveAddress = 0x0A00u;
            host.writeWord(kSendAddress + 8u, 0x219715C0u);
            host.writeWord(kReceiveAddress, 0xFFFFFFFFu);

            ps2x::iop::RpcRequest request{};
            request.sid = 0x77777777u;
            request.function = 0x00040000u;
            request.send = {kSendAddress, 32u};
            request.receive = {kReceiveAddress, sizeof(uint32_t)};
            const ps2x::iop::RpcResult result = subsystem.handleRpc(request);

            t.IsTrue(result.handled, "the game profile should claim MODHSYN commands");
            t.Equals(host.readWord(kReceiveAddress), 0u,
                     "the observed MODHSYN status response should be zero");
            t.Equals(host.audioCalls, 1u,
                     "the command should be forwarded through the portable audio backend seam");
            t.Equals(host.lastAudioSid, 0x77777777u,
                     "the audio backend should retain the MODHSYN SID");
            t.Equals(host.lastAudioFunction, 0x00040000u,
                     "the audio backend should retain the MODHSYN function code");
        });

        tc.Run("Haunting Ground SNDDRV transfer copies staging data and reports deferred completion", [](TestCase &t)
        {
            FakeIopHost host;
            ps2x::iop::IopSubsystem subsystem(host);
            std::string error;
            t.IsTrue(subsystem.configure({"SLUS_210.75", 0x00100008u, 0xA295AF2Bu}, &error),
                     "the exact Haunting Ground ELF identity should configure");

            constexpr uint32_t kSendAddress = 0x0800u;
            constexpr uint32_t kReceiveAddress = 0x0A00u;
            constexpr uint32_t kSourceAddress = 0x1000u;
            constexpr uint32_t kSpu2Address = 0x000DAAC0u;
            constexpr uint32_t kTransferBytes = 16u;
            std::array<uint8_t, 32> command{};
            const auto putLe32 = [&](size_t offset, uint32_t value)
            {
                command[offset + 0u] = static_cast<uint8_t>(value);
                command[offset + 1u] = static_cast<uint8_t>(value >> 8u);
                command[offset + 2u] = static_cast<uint8_t>(value >> 16u);
                command[offset + 3u] = static_cast<uint8_t>(value >> 24u);
            };
            const auto readLe32 = [&](uint32_t address)
            {
                return static_cast<uint32_t>(host.memory[address + 0u]) |
                       (static_cast<uint32_t>(host.memory[address + 1u]) << 8u) |
                       (static_cast<uint32_t>(host.memory[address + 2u]) << 16u) |
                       (static_cast<uint32_t>(host.memory[address + 3u]) << 24u);
            };
            putLe32(8u, kSourceAddress);
            putLe32(12u, kSpu2Address);
            putLe32(16u, kTransferBytes);
            putLe32(20u, 1u);
            const std::array<uint8_t, kTransferBytes> payload = {
                0x10u, 0x21u, 0x32u, 0x43u, 0x54u, 0x65u, 0x76u, 0x87u,
                0x98u, 0xA9u, 0xBAu, 0xCBu, 0xDCu, 0xEDu, 0xFEu, 0x0Fu,
            };
            t.IsTrue(host.writeGuest(kSendAddress, command.data(), command.size()),
                     "the exact 32-byte SNDDRV command should fit in guest memory");
            t.IsTrue(host.writeGuest(kSourceAddress, payload.data(), payload.size()),
                     "the SNDDRV source payload should fit in staging memory");

            ps2x::iop::RpcRequest request{};
            request.sid = 0x77777778u;
            request.function = 0x00120000u;
            request.mode = 1u;
            request.send = {kSendAddress, static_cast<uint32_t>(command.size())};
            request.receive = {kReceiveAddress, sizeof(uint32_t)};
            const ps2x::iop::RpcResult result = subsystem.handleRpc(request);

            t.IsTrue(result.handled, "the exact SNDDRV transfer class should be claimed");
            t.Equals(result.serverDispatchPolicy, ps2x::iop::ServerDispatchPolicy::Suppress,
                     "the HLE transfer should suppress unavailable IOP guest dispatch");
            t.Equals(result.completionDelayMicroseconds, 100u,
                     "the live polling strategy should defer completion by 100 microseconds");
            t.Equals(readLe32(kReceiveAddress), 0u,
                     "a successful transfer should return an explicit little-endian zero");
            t.Equals(host.spu2Writes, 1u, "one SPU2 transfer should be issued");
            t.Equals(host.lastSpu2Address, kSpu2Address,
                     "the transfer should retain the observed SPU2 destination");
            t.Equals(host.lastSpu2Size, size_t{kTransferBytes},
                     "the transfer should retain the observed byte count");
            t.IsTrue(std::equal(payload.begin(), payload.end(),
                                host.spu2Memory.begin() + kSpu2Address),
                     "SPU2 RAM should receive the exact staging payload");

            constexpr uint32_t kFullTransferBytes = 0x4000u;
            putLe32(16u, kFullTransferBytes);
            std::vector<uint8_t> fullPayload(kFullTransferBytes, 0x5Au);
            t.IsTrue(host.writeGuest(kSendAddress, command.data(), command.size()),
                     "the full-size SNDDRV command should fit in guest memory");
            t.IsTrue(host.writeGuest(kSourceAddress, fullPayload.data(), fullPayload.size()),
                     "the full-size SNDDRV payload should fit in guest memory");
            const ps2x::iop::RpcResult fullTransfer = subsystem.handleRpc(request);
            t.Equals(fullTransfer.completionDelayMicroseconds, 900u,
                     "a 0x4000-byte SPU2 DMA should complete on the ninth 100-us poll");
            t.Equals(host.lastSpu2Size, size_t{kFullTransferBytes},
                     "the full-size transfer should retain its exact byte count");

            putLe32(8u, 0u);
            t.IsTrue(host.writeGuest(kSendAddress, command.data(), command.size()),
                     "the invalid command should fit in guest memory");
            const ps2x::iop::RpcResult invalid = subsystem.handleRpc(request);
            t.Equals(readLe32(kReceiveAddress), 0xFFFFFFFFu,
                     "a zero source should return the observed -1 status encoding");
            t.Equals(invalid.completionDelayMicroseconds, 0u,
                     "failed transfers must not schedule a false completion delay");
            t.Equals(host.spu2Writes, 2u, "failed validation must not write SPU2 RAM");

            const DebugSnapshot snapshot = subsystem.debugSnapshot();
            const DebugService *service =
                findService(snapshot, "Haunting Ground Capcom SNDDRV transfer");
            if (!service)
            {
                t.Fail("the game-scoped SNDDRV transfer service should expose diagnostics");
                return;
            }
            t.Equals(metricValue(*service, "transfer_count"), uint64_t{2},
                     "diagnostics should count successful transfers");
            t.Equals(metricValue(*service, "failure_count"), uint64_t{1},
                     "diagnostics should count rejected transfers");
        });

        tc.Run("Haunting Ground SNDDRV accepts only proven mailbox generations", [](TestCase &t)
        {
            FakeIopHost host(0xC0000u);
            ps2x::iop::IopSubsystem subsystem(host);
            std::string error;
            t.IsTrue(subsystem.configure({"SLUS_210.75", 0x00100008u, 0xA295AF2Bu}, &error),
                     "the exact Haunting Ground ELF identity should configure");

            constexpr uint32_t kMailbox = 0x2000u;
            constexpr uint32_t kMailboxBytes = 0x880u;
            constexpr uint32_t kSequenceOffset = 0x87Cu;
            host.writeWord(kMailbox, 0u);
            host.writeWord(kMailbox + kSequenceOffset, 1u);
            subsystem.onSifTransfer({ps2x::iop::SifTransferKind::SetDma,
                                     ps2x::iop::SifTransferPhase::AfterCopy,
                                     kMailbox,
                                     0x000B2780u,
                                     kMailboxBytes});
            t.Equals(host.readWord(kMailbox + kSequenceOffset), 2u,
                     "the exact empty heartbeat should return the next even generation");

            host.writeWord(kMailbox, 1u);
            host.writeWord(kMailbox + kSequenceOffset, 3u);
            subsystem.onSifTransfer({ps2x::iop::SifTransferKind::SetDma,
                                     ps2x::iop::SifTransferPhase::AfterCopy,
                                     kMailbox,
                                     0x000B2780u,
                                     kMailboxBytes});
            t.Equals(host.readWord(kMailbox + kSequenceOffset), 3u,
                     "an uncharacterized nonempty generation must remain pending");

            constexpr uint32_t kStreamHandle = 0x01F20000u;
            host.writeWord(kMailbox + 0x10u, 8u);
            host.writeWord(kMailbox + 0x14u, kStreamHandle);
            host.writeWord(kMailbox + 0x18u, 1u);
            host.writeWord(kMailbox + 0x1Cu, 0u);
            host.writeWord(kMailbox + kSequenceOffset, 5u);
            subsystem.onSifTransfer({ps2x::iop::SifTransferKind::SetDma,
                                     ps2x::iop::SifTransferPhase::AfterCopy,
                                     kMailbox,
                                     0x000B2780u,
                                     kMailboxBytes});
            t.Equals(host.readWord(kMailbox + kSequenceOffset), 6u,
                     "the exact opcode-8 stream-start generation should complete");

            host.writeWord(kMailbox + 0x10u, 5u);
            host.writeWord(kMailbox + 0x18u, 0u);
            host.writeWord(kMailbox + kSequenceOffset, 7u);
            subsystem.onSifTransfer({ps2x::iop::SifTransferKind::SetDma,
                                     ps2x::iop::SifTransferPhase::AfterCopy,
                                     kMailbox,
                                     0x000B2780u,
                                     kMailboxBytes});
            t.Equals(host.readWord(kMailbox + kSequenceOffset), 8u,
                     "the exact opcode-5 zero-argument control generation should complete");

            constexpr uint32_t kConfigurationHandle = 0x01F20040u;
            const std::array<std::array<uint32_t, 4>, 4> configuration{{
                {8u, kConfigurationHandle, 0u, 0u},
                {4u, kConfigurationHandle, 0x0000BB80u, 0u},
                {9u, kConfigurationHandle, 0u, 0xFFFFFFF1u},
                {9u, kConfigurationHandle, 1u, 0x0000000Fu},
            }};
            host.writeWord(kMailbox, 4u);
            for (uint32_t record = 0u; record < configuration.size(); ++record)
                for (uint32_t field = 0u; field < configuration[record].size(); ++field)
                    host.writeWord(kMailbox + 0x10u + record * 0x10u + field * 4u,
                                   configuration[record][field]);
            host.writeWord(kMailbox + kSequenceOffset, 9u);
            subsystem.onSifTransfer({ps2x::iop::SifTransferKind::SetDma,
                                     ps2x::iop::SifTransferPhase::AfterCopy,
                                     kMailbox,
                                     0x000B2780u,
                                     kMailboxBytes});
            t.Equals(host.readWord(kMailbox + kSequenceOffset), 10u,
                     "the oracle-matched four-record configuration batch should complete");

            host.writeWord(kMailbox, 2u);
            host.writeWord(kMailbox + 0x10u, 0x100u);
            host.writeWord(kMailbox + 0x14u, 0x01F200A0u);
            host.writeWord(kMailbox + 0x18u, 0x00079A40u);
            host.writeWord(kMailbox + 0x1Cu, 0x980u);
            host.writeWord(kMailbox + 0x20u, 0x100u);
            host.writeWord(kMailbox + 0x24u, 0x01F200E0u);
            host.writeWord(kMailbox + 0x28u, 0x0007DB40u);
            host.writeWord(kMailbox + 0x2Cu, 0x980u);
            host.writeWord(kMailbox + kSequenceOffset, 11u);
            subsystem.onSifTransfer({ps2x::iop::SifTransferKind::SetDma,
                                     ps2x::iop::SifTransferPhase::AfterCopy,
                                     kMailbox,
                                     0x00070E40u,
                                     kMailboxBytes});
            t.Equals(host.readWord(kMailbox + kSequenceOffset), 12u,
                     "the exact paired playback request batch should complete");
            t.Equals(host.readWord(kMailbox), 0u,
                     "the first playback request should return an empty prior generation");
            t.Equals(host.spu2StreamSubmissions, 1u,
                     "the validated playback pair should reach the portable stream seam once");
            t.Equals(host.lastSpu2FirstCursor, 0x00079A40u,
                     "the stream seam should retain the first-channel cursor");
            t.Equals(host.lastSpu2SecondCursor, 0x0007DB40u,
                     "the stream seam should retain the second-channel cursor");
            t.Equals(host.lastSpu2StreamBytes, 0x980u,
                     "the stream seam should retain the per-channel byte count");
            t.Equals(host.lastSpu2FirstRingBase, 0x00079A40u,
                     "the first request should establish the first-channel ring base");
            t.Equals(host.lastSpu2SecondRingBase, 0x0007DB40u,
                     "the first request should establish the second-channel ring base");
            t.Equals(host.lastSpu2RingBytes, 0x4000u,
                     "the stream seam should retain the oracle-proven ring length");
            t.Equals(host.lastSpu2SampleRate, 0xBB80u,
                     "the stream seam should retain the configured 48 kHz rate");

            host.writeWord(kMailbox, 2u);
            host.writeWord(kMailbox + 0x10u, 0x100u);
            host.writeWord(kMailbox + 0x14u, 0x01F200A0u);
            host.writeWord(kMailbox + 0x18u, 0x0007D640u);
            host.writeWord(kMailbox + 0x1Cu, 0x800u);
            host.writeWord(kMailbox + 0x20u, 0x100u);
            host.writeWord(kMailbox + 0x24u, 0x01F200E0u);
            host.writeWord(kMailbox + 0x28u, 0x00081740u);
            host.writeWord(kMailbox + 0x2Cu, 0x800u);
            host.writeWord(kMailbox + kSequenceOffset, 13u);
            subsystem.onSifTransfer({ps2x::iop::SifTransferKind::SetDma,
                                     ps2x::iop::SifTransferPhase::AfterCopy,
                                     kMailbox,
                                     0x00070E40u,
                                     kMailboxBytes});
            t.Equals(host.readWord(kMailbox + kSequenceOffset), 14u,
                     "a wrapped paired playback request should complete");
            t.Equals(host.readWord(kMailbox), 2u,
                     "the next generation should return the prior non-wrapping callbacks");
            t.Equals(host.readWord(kMailbox + 0x10u), 0u,
                     "the returned record must use the callback opcode");
            t.Equals(host.readWord(kMailbox + 0x14u), 0x003D4E00u,
                     "the first IOP descriptor should map to its exact EE callback object");
            t.Equals(host.readWord(kMailbox + 0x24u), 0x003D4E14u,
                     "the second IOP descriptor should map to its exact EE callback object");

            host.writeWord(kMailbox, 2u);
            host.writeWord(kMailbox + 0x10u, 0x100u);
            host.writeWord(kMailbox + 0x14u, 0x01F200A0u);
            host.writeWord(kMailbox + 0x18u, 0x00079E40u);
            host.writeWord(kMailbox + 0x1Cu, 0x4C0u);
            host.writeWord(kMailbox + 0x20u, 0x100u);
            host.writeWord(kMailbox + 0x24u, 0x01F200E0u);
            host.writeWord(kMailbox + 0x28u, 0x0007DF40u);
            host.writeWord(kMailbox + 0x2Cu, 0x4C0u);
            host.writeWord(kMailbox + kSequenceOffset, 15u);
            subsystem.onSifTransfer({ps2x::iop::SifTransferKind::SetDma,
                                     ps2x::iop::SifTransferPhase::AfterCopy,
                                     kMailbox,
                                     0x00070E40u,
                                     kMailboxBytes});
            t.Equals(host.readWord(kMailbox + kSequenceOffset), 16u,
                     "the following playback generation should complete");
            t.Equals(host.readWord(kMailbox), 4u,
                     "the following generation should return four wrapped callbacks");
            t.Equals(host.readWord(kMailbox + 0x18u), 0x0007D640u,
                     "the first callback should retain the tail cursor");
            t.Equals(host.readWord(kMailbox + 0x1Cu), 0x400u,
                     "the first callback should stop at the first ring boundary");
            t.Equals(host.readWord(kMailbox + 0x28u), 0x00079A40u,
                     "the second callback should wrap to the first ring base");
            t.Equals(host.readWord(kMailbox + 0x38u), 0x00081740u,
                     "the third callback should retain the second tail cursor");
            t.Equals(host.readWord(kMailbox + 0x48u), 0x0007DB40u,
                     "the fourth callback should wrap to the second ring base");
            t.Equals(host.spu2StreamSubmissions, 3u,
                     "the wrapped and following partial requests should each submit one stereo chunk");

            host.writeWord(kMailbox, 0u);
            host.writeWord(kMailbox + kSequenceOffset, 17u);
            subsystem.onSifTransfer({ps2x::iop::SifTransferKind::SetDma,
                                     ps2x::iop::SifTransferPhase::AfterCopy,
                                     kMailbox,
                                     0x00070E40u,
                                     kMailboxBytes});
            t.Equals(host.readWord(kMailbox + kSequenceOffset), 18u,
                     "an empty response generation should complete normally");
            t.Equals(host.readWord(kMailbox), 2u,
                     "an empty response generation should release the final delayed callbacks");
            t.Equals(host.readWord(kMailbox + 0x14u), 0x003D4E00u,
                     "the final first-channel callback should retain its exact object");
            t.Equals(host.readWord(kMailbox + 0x18u), 0x00079E40u,
                     "the final first-channel callback should retain its cursor");
            t.Equals(host.readWord(kMailbox + 0x1Cu), 0x4C0u,
                     "the final partial-block callback should retain its 0x40-aligned byte count");
            t.Equals(host.readWord(kMailbox + 0x24u), 0x003D4E14u,
                     "the final second-channel callback should retain its exact object");

            // The game switches to a second descriptor pair while leaving the
            // callback objects unchanged.  Its rings have independent bases and
            // may replace the previous pair only after all delayed callbacks drain.
            host.writeWord(kMailbox, 2u);
            host.writeWord(kMailbox + 0x10u, 0x100u);
            host.writeWord(kMailbox + 0x14u, 0x01F20020u);
            host.writeWord(kMailbox + 0x18u, 0x00071840u);
            host.writeWord(kMailbox + 0x1Cu, 0x1300u);
            host.writeWord(kMailbox + 0x20u, 0x100u);
            host.writeWord(kMailbox + 0x24u, 0x01F20060u);
            host.writeWord(kMailbox + 0x28u, 0x00075940u);
            host.writeWord(kMailbox + 0x2Cu, 0x1300u);
            host.writeWord(kMailbox + kSequenceOffset, 19u);
            subsystem.onSifTransfer({ps2x::iop::SifTransferKind::SetDma,
                                     ps2x::iop::SifTransferPhase::AfterCopy,
                                     kMailbox,
                                     0x00070E40u,
                                     kMailboxBytes});
            t.Equals(host.readWord(kMailbox + kSequenceOffset), 20u,
                     "a drained descriptor-pair transition should establish new ring bases");
            t.Equals(host.readWord(kMailbox), 0u,
                     "the new descriptor pair should return an empty prior generation");
            t.Equals(host.spu2StreamSubmissions, 4u,
                     "the drained descriptor-pair transition should submit its audio chunk");
            t.Equals(host.lastSpu2FirstRingBase, 0x00071840u,
                     "the replacement pair should establish its independent first ring");
            t.Equals(host.lastSpu2SecondRingBase, 0x00075940u,
                     "the replacement pair should establish its independent second ring");

            host.writeWord(kMailbox, 0u);
            host.writeWord(kMailbox + kSequenceOffset, 21u);
            subsystem.onSifTransfer({ps2x::iop::SifTransferKind::SetDma,
                                     ps2x::iop::SifTransferPhase::AfterCopy,
                                     kMailbox,
                                     0x00070E40u,
                                     kMailboxBytes});
            t.Equals(host.readWord(kMailbox + kSequenceOffset), 22u,
                     "the new descriptor pair's delayed callbacks should complete");
            t.Equals(host.readWord(kMailbox), 2u,
                     "the new descriptor pair should produce two callback records");
            t.Equals(host.readWord(kMailbox + 0x14u), 0x003D4DD8u,
                     "the first new ring should select its exact callback object");
            t.Equals(host.readWord(kMailbox + 0x18u), 0x00071840u,
                     "the first new ring callback should retain its cursor");
            t.Equals(host.readWord(kMailbox + 0x24u), 0x003D4DECu,
                     "the second new ring should select its exact callback object");
            t.Equals(host.readWord(kMailbox + 0x28u), 0x00075940u,
                     "the second new ring callback should retain its cursor");

            host.writeWord(kMailbox, 2u);
            host.writeWord(kMailbox + 0x10u, 0x100u);
            host.writeWord(kMailbox + 0x14u, 0x01F20020u);
            host.writeWord(kMailbox + 0x18u, 0x00079A40u);
            host.writeWord(kMailbox + 0x1Cu, 0x980u);
            host.writeWord(kMailbox + 0x20u, 0x100u);
            host.writeWord(kMailbox + 0x24u, 0x01F20060u);
            host.writeWord(kMailbox + 0x28u, 0x0007DB40u);
            host.writeWord(kMailbox + 0x2Cu, 0x980u);
            host.writeWord(kMailbox + kSequenceOffset, 23u);
            subsystem.onSifTransfer({ps2x::iop::SifTransferKind::SetDma,
                                     ps2x::iop::SifTransferPhase::AfterCopy,
                                     kMailbox,
                                     0x00070E40u,
                                     kMailboxBytes});
            t.Equals(host.readWord(kMailbox + kSequenceOffset), 23u,
                     "the same descriptor pair must not silently rebase out-of-window cursors");
            t.Equals(host.spu2StreamSubmissions, 4u,
                     "a rejected out-of-window request must not reach the audio stream seam");

            host.writeWord(kMailbox, 0u);
            subsystem.onSifTransfer({ps2x::iop::SifTransferKind::SetDma,
                                     ps2x::iop::SifTransferPhase::AfterCopy,
                                     kMailbox,
                                     0x000B2700u,
                                     kMailboxBytes});
            t.Equals(host.readWord(kMailbox + kSequenceOffset), 23u,
                     "an unrelated IOP destination must not be acknowledged");

            host.writeWord(kSequenceOffset, 1u);
            subsystem.onSifTransfer({ps2x::iop::SifTransferKind::SetDma,
                                     ps2x::iop::SifTransferPhase::AfterCopy,
                                     0u,
                                     0x00070E40u,
                                     kMailboxBytes});
            t.Equals(host.readWord(kSequenceOffset), 1u,
                     "a null payload pointer must remain pending for allocation diagnosis");

            const DebugSnapshot snapshot = subsystem.debugSnapshot();
            const DebugService *service =
                findService(snapshot, "Haunting Ground Capcom SNDDRV transfer");
            if (!service)
            {
                t.Fail("the game-scoped SNDDRV transfer service should expose diagnostics");
                return;
            }
            t.Equals(metricValue(*service, "empty_mailbox_acks"), uint64_t{3},
                     "diagnostics should count exact empty-heartbeat acknowledgments");
            t.Equals(metricValue(*service, "nonempty_mailbox_generations"), uint64_t{9},
                     "diagnostics should expose supported and unsupported command generations");
            t.Equals(metricValue(*service, "stream_start_commands"), uint64_t{1},
                     "diagnostics should count the exact supported stream-start command");
            t.Equals(metricValue(*service, "stream_control_commands"), uint64_t{1},
                     "diagnostics should count the exact supported stream-control command");
            t.Equals(metricValue(*service, "stream_configuration_batches"), uint64_t{1},
                     "diagnostics should count the exact four-record configuration batch");
            t.Equals(metricValue(*service, "playback_response_batches"), uint64_t{4},
                     "diagnostics should count the exact paired playback response batch");
            t.Equals(metricValue(*service, "active_stream_handle"),
                     uint64_t{kConfigurationHandle},
                     "the accepted configuration should retain its driver stream handle");
            t.Equals(metricValue(*service, "last_playback_cursor"), uint64_t{0x00071840u},
                     "diagnostics should retain the first playback cursor");
            t.Equals(metricValue(*service, "last_playback_bytes"), uint64_t{0x1300u},
                     "diagnostics should retain the playback byte count");
            t.Equals(metricValue(*service, "pending_playback_callback_count"), uint64_t{0},
                     "the empty response generation should release the delayed callback batch");
        });

        tc.Run("two subsystem instances isolate profile state and reset deterministically", [](TestCase &t)
        {
            FakeIopHost hostA;
            FakeIopHost hostB;
            ps2x::iop::IopSubsystem subsystemA(hostA);
            ps2x::iop::IopSubsystem subsystemB(hostB);
            std::string error;
            t.IsTrue(subsystemA.configure({"SLUS_205.78", 0u, 0u}, &error),
                     "first LotR instance should configure");
            t.IsTrue(subsystemB.configure({"SLUS_205.78", 0u, 0u}, &error),
                     "second LotR instance should configure");

            ps2x::iop::RpcRequest request{};
            request.sid = 0x00012345u;
            request.receive = {0x1000u, 8u};

            t.IsTrue(subsystemA.handleRpc(request).handled,
                     "first instance should handle LotR sound RPC");
            t.Equals(hostA.readWord(0x1004u), 1u,
                     "first instance should start its counter at one");
            (void)subsystemA.handleRpc(request);
            t.Equals(hostA.readWord(0x1004u), 2u,
                     "first instance should advance independently");

            t.IsTrue(subsystemB.handleRpc(request).handled,
                     "second instance should handle LotR sound RPC");
            t.Equals(hostB.readWord(0x1004u), 1u,
                     "second instance must not inherit the first counter");

            subsystemA.reset();
            (void)subsystemA.handleRpc(request);
            t.Equals(hostA.readWord(0x1004u), 1u,
                     "reset should restore per-instance service state");
        });

        tc.Run("LotR sound update completes queued PlayStream slots", [](TestCase &t)
        {
            FakeIopHost host;
            ps2x::iop::IopSubsystem subsystem(host);
            std::string error;
            t.IsTrue(subsystem.configure({"SLUS_205.78", 0u, 0u}, &error),
                     "LotR profile should configure");

            constexpr uint32_t kSendAddress = 0x0800u;
            constexpr uint32_t kReceiveAddress = 0x1000u;
            constexpr uint16_t kStreamSlot = 7u;
            const std::array<uint16_t, 10> playStreamPacket = {
                1u, // command count
                1u, // PlayStream
                7u, // argument count
                0u,
                static_cast<uint16_t>(kStreamSlot << 8u),
                0u,
                0u,
                0u,
                0u,
                0u,
            };
            t.IsTrue(host.writeGuest(kSendAddress,
                                     playStreamPacket.data(),
                                     sizeof(playStreamPacket)),
                     "PlayStream command packet should fit in guest memory");

            ps2x::iop::RpcRequest request{};
            request.sid = 0x00012345u;
            request.send = {kSendAddress, sizeof(playStreamPacket)};
            request.receive = {kReceiveAddress, 0x100u};

            t.IsTrue(subsystem.handleRpc(request).handled,
                     "LotR sound service should handle PlayStream");
            t.Equals(host.readWord(kReceiveAddress), 1u,
                     "PlayStream response should expose one active record");
            const uint32_t packedStream = host.readWord(kReceiveAddress + 4u);
            t.Equals((packedStream >> 4u) & 0x3Fu,
                     static_cast<uint32_t>(kStreamSlot),
                     "active record should identify the queued EE stream slot");
            t.Equals(host.readWord(kReceiveAddress + 0x24u), 1u,
                     "response counter should follow the active record");

            const std::array<uint16_t, 5> statusPacket = {
                1u, // command count
                9u, // GetStatus
                2u, // argument count
                kStreamSlot,
                0u,
            };
            t.IsTrue(host.writeGuest(kSendAddress, statusPacket.data(), sizeof(statusPacket)),
                     "GetStatus command packet should fit in guest memory");
            request.send.size = sizeof(statusPacket);

            t.IsTrue(subsystem.handleRpc(request).handled,
                     "LotR sound service should handle the following status update");
            t.Equals(host.readWord(kReceiveAddress), 0u,
                     "the update after PlayStream should report no active records");
            t.Equals(host.readWord(kReceiveAddress + 4u), 2u,
                     "empty response counter should return to the base offset");
        });

        tc.Run("TSNDDRV uses profile checksum bindings without writing invalid ports", [](TestCase &t)
        {
            FakeIopHost host(0x02000000u);
            ps2x::iop::IopSubsystem subsystem(host);
            std::string error;
            t.IsTrue(subsystem.configure({"slus_201.84", 0u, 0u}, &error),
                     "RECVX profile should configure for TSNDDRV command testing");

            constexpr uint32_t kResponseAddress = 0x1000u;
            ps2x::iop::RpcRequest stateRequest{};
            stateRequest.sid = 1u;
            stateRequest.function = 0x12u;
            stateRequest.receive = {kResponseAddress, sizeof(uint32_t)};
            t.IsTrue(subsystem.handleRpc(stateRequest).handled,
                     "TSNDDRV should return its configured status buffer");
            const uint32_t statusAddress = host.readWord(kResponseAddress);
            t.IsTrue(statusAddress != 0u, "TSNDDRV status buffer should be allocated");

            constexpr int16_t kChecksum = 0x1234;
            t.IsTrue(host.writeGuest(0x01E0EF10u, &kChecksum, sizeof(kChecksum)),
                     "RECVX primary checksum binding should be writable in the fake guest");

            constexpr uint32_t kCommandAddress = 0x2000u;
            std::array<uint8_t, 8> command{};
            command[0] = 0x29u;
            command[1] = 0u;
            t.IsTrue(host.writeGuest(kCommandAddress, command.data(), command.size()),
                     "valid TSNDDRV command should be writable");

            ps2x::iop::RpcRequest commandRequest{};
            commandRequest.sid = 0u;
            commandRequest.function = 0u;
            commandRequest.send = {kCommandAddress, static_cast<uint32_t>(command.size())};
            t.IsTrue(subsystem.handleRpc(commandRequest).handled,
                     "TSNDDRV should handle the characterized command queue");

            int16_t writtenChecksum = 0;
            t.IsTrue(host.readGuest(statusAddress + 0x26u,
                                    &writtenChecksum,
                                    sizeof(writtenChecksum)),
                     "TSNDDRV SE checksum slot should be readable");
            t.Equals(writtenChecksum, kChecksum,
                     "valid port should mirror the profile-bound checksum table");

            constexpr uint32_t kPastStatusAddress = 0x44u;
            constexpr uint16_t kSentinel = 0xBEEFu;
            t.IsTrue(host.writeGuest(statusAddress + kPastStatusAddress,
                                     &kSentinel,
                                     sizeof(kSentinel)),
                     "sentinel after the status structure should be writable");
            command[1] = 0x0Fu;
            (void)host.writeGuest(kCommandAddress, command.data(), command.size());
            (void)subsystem.handleRpc(commandRequest);

            uint16_t sentinelAfter = 0u;
            (void)host.readGuest(statusAddress + kPastStatusAddress,
                                 &sentinelAfter,
                                 sizeof(sentinelAfter));
            t.Equals(sentinelAfter, kSentinel,
                     "invalid port must not overwrite memory past the 0x42-byte status structure");
        });

        tc.Run("RECVX reset clears CRI object maps without global state", [](TestCase &t)
        {
            FakeIopHost host(0x02000000u);
            ps2x::iop::IopSubsystem subsystem(host);
            std::string error;
            t.IsTrue(subsystem.configure({"slus_201.84", 0u, 0u}, &error),
                     "RECVX profile should configure");

            constexpr uint32_t kSendAddress = 0x2000u;
            constexpr uint32_t kReceiveAddress = 0x2100u;
            host.writeWord(kSendAddress + 0u, 0u);
            host.writeWord(kSendAddress + 4u, 0x4000u);
            host.writeWord(kSendAddress + 8u, 0x100u);

            ps2x::iop::RpcRequest request{};
            request.sid = 0x7D000000u;
            request.function = 0x422u;
            request.send = {kSendAddress, 12u};
            request.receive = {kReceiveAddress, 4u};
            t.IsTrue(subsystem.handleRpc(request).handled,
                     "SJRMT create should be emulated by the RECVX profile");

            ps2x::iop::DebugSnapshot snapshot = subsystem.debugSnapshot();
            const ps2x::iop::DebugService *service =
                findService(snapshot, "CRI DTX");
            if (!service)
            {
                t.Fail("CRI DTX service should be visible in the debug snapshot");
                return;
            }
            t.Equals(metricValue(*service, "sjrmt_objects"), uint64_t{1},
                     "created CRI object should be tracked by this instance");

            subsystem.reset();
            snapshot = subsystem.debugSnapshot();
            service = findService(snapshot, "CRI DTX");
            if (!service)
            {
                t.Fail("CRI DTX service should survive reset");
                return;
            }
            t.Equals(metricValue(*service, "sjrmt_objects"), uint64_t{0},
                     "reset should clear CRI object maps");
        });

        tc.Run("reset closes profile-owned host file handles", [](TestCase &t)
        {
            FakeIopHost host;
            host.hostFileContents["translated/test.bin"] = {0x10u, 0x20u, 0x30u};

            ps2x::iop::IopSubsystem subsystem(host);
            std::string error;
            t.IsTrue(subsystem.configure({"SLUS_205.78", 0u, 0u}, &error),
                     "LotR profile should configure for file lifecycle testing");

            constexpr uint32_t kPathAddress = 0x1000u;
            constexpr uint32_t kReceiveAddress = 0x1100u;
            constexpr char kPath[] = "test.bin";
            t.IsTrue(host.writeGuest(kPathAddress, kPath, sizeof(kPath)),
                     "fake guest path should be writable");

            ps2x::iop::RpcRequest request{};
            request.sid = 0x0000FF01u;
            request.function = 0x08u;
            request.send = {kPathAddress, sizeof(kPath)};
            request.receive = {kReceiveAddress, 8u};
            t.IsTrue(subsystem.handleRpc(request).handled,
                     "LotR CLFILE open should be handled");
            t.Equals(host.openHostFiles.size(), size_t{1},
                     "open RPC should retain one opaque host file handle");

            ps2x::iop::DebugSnapshot snapshot = subsystem.debugSnapshot();
            const ps2x::iop::DebugService *service =
                findService(snapshot, "CLFILE");
            if (!service)
            {
                t.Fail("LotR CLFILE service should be visible before reset");
                return;
            }
            t.Equals(metricValue(*service, "open_files"), uint64_t{1},
                     "debug state should report the open file");

            subsystem.reset();
            t.IsTrue(host.openHostFiles.empty(),
                     "reset should release every retained host file handle");
            t.Equals(host.closedHostFileHandles.size(), size_t{1},
                     "host close callback should run exactly once");
            snapshot = subsystem.debugSnapshot();
            service = findService(snapshot, "CLFILE");
            if (!service)
            {
                t.Fail("LotR CLFILE service should survive reset");
                return;
            }
            t.Equals(metricValue(*service, "open_files"), uint64_t{0},
                     "reset should clear the CLFILE handle registry");
        });

#if defined(PS2X_TEST_IOP_PLUGIN_DIR)
        tc.Run("plugin module remains loaded through instances and unloads after subsystem destruction", [](TestCase &t)
        {
            const std::filesystem::path pluginDirectory(PS2X_TEST_IOP_PLUGIN_DIR);
#if defined(_WIN32)
            const std::filesystem::path pluginPath =
                pluginDirectory / "ps2_iop_fake_plugin.dll";
#else
            const std::filesystem::path pluginPath =
                pluginDirectory / "ps2_iop_fake_plugin.so";
#endif
            t.IsFalse(pluginModuleIsLoaded(pluginPath),
                      "synthetic plugin should not be loaded before discovery");
            {
                FakeIopHost host;
                ps2x::iop::IopSubsystem subsystem(host);
                subsystem.setPluginSearchPaths({pluginDirectory});
                std::string error;
                t.IsTrue(subsystem.loadPlugins(&error),
                         "synthetic plugins should load for lifetime testing");
                t.IsTrue(subsystem.configure({"synthetic_iop_test.elf",
                                              kSyntheticEntryPoint,
                                              kSyntheticCrc32},
                                             &error),
                         "synthetic plugin instance should be created");
                t.IsTrue(pluginModuleIsLoaded(pluginPath),
                         "module must stay loaded while a profile instance exists");
            }
            t.IsFalse(pluginModuleIsLoaded(pluginPath),
                      "module should unload after profile destruction and catalog teardown");
        });

        tc.Run("plugin discovery matches all identity fields and dispatches through the host bridge", [](TestCase &t)
        {
            FakeIopHost host;
            ps2x::iop::IopSubsystem subsystem(host);
            const std::filesystem::path pluginDirectory(PS2X_TEST_IOP_PLUGIN_DIR);

            t.IsTrue(std::filesystem::is_directory(pluginDirectory),
                     "the synthetic IOP plugin directory should be staged by the test build");
            subsystem.setPluginSearchPaths({pluginDirectory});

            std::string error;
            t.IsTrue(subsystem.loadPlugins(&error), "synthetic IOP plugin discovery should succeed");
            ps2x::iop::DebugSnapshot discoverySnapshot = subsystem.debugSnapshot();
            t.IsTrue(containsDiagnostic(discoverySnapshot, "loaded 4 profile(s)"),
                     "plugin discovery diagnostics should report all accepted synthetic profiles");
            t.IsTrue(containsDiagnostic(discoverySnapshot, "too many SIDs"),
                     "an invalid profile descriptor should be ignored with a diagnostic");
            t.IsTrue(containsDiagnostic(discoverySnapshot, "bad_abi"),
                     "an ABI-incompatible plugin should be ignored with a diagnostic");
            t.IsTrue(containsDiagnostic(discoverySnapshot, "incompatible ABI"),
                     "the incompatible-plugin diagnostic should explain the ABI failure");
            t.IsTrue(containsDiagnostic(discoverySnapshot, "missing_symbol"),
                     "a plugin without the query symbol should be ignored with a diagnostic");
            t.IsTrue(containsDiagnostic(discoverySnapshot, "missing ps2x_iop_query_v1"),
                     "the missing-symbol diagnostic should name the required entry point");

            auto expectNoProfile = [&](const ps2x::iop::GameIdentity &identity, const std::string &reason) {
                error.clear();
                t.IsTrue(subsystem.configure(identity, &error), "mismatching plugin identity should configure core-only services");
                const ps2x::iop::DebugSnapshot snapshot = subsystem.debugSnapshot();
                t.IsTrue(snapshot.activeProfile.empty(), reason);

                ps2x::iop::RpcRequest request{};
                request.sid = kSyntheticSid;
                request.function = kSyntheticFunction;
                t.IsFalse(subsystem.handleRpc(request).handled,
                          "a mismatching profile must not expose its synthetic SID");
            };

            expectNoProfile({"different.elf", kSyntheticEntryPoint, kSyntheticCrc32},
                            "a different ELF basename should not match the plugin profile");
            expectNoProfile({"synthetic_iop_test.elf", kSyntheticEntryPoint + 4u, kSyntheticCrc32},
                            "a different entry point should not match the plugin profile");
            expectNoProfile({"synthetic_iop_test.elf", kSyntheticEntryPoint, kSyntheticCrc32 ^ 1u},
                            "a different CRC32 should not match the plugin profile");

            error.clear();
            t.IsTrue(subsystem.configure({"synthetic_iop_test.elf", kSyntheticEntryPoint, kSyntheticCrc32}, &error),
                     "the synthetic ELF identity should activate the plugin profile");

            ps2x::iop::DebugSnapshot snapshot = subsystem.debugSnapshot();
            t.Equals(snapshot.activeProfile, std::string("synthetic-test-profile"),
                     "debug snapshot should expose the active plugin profile id");
            t.Equals(snapshot.activeProvider, std::string("ps2x-test-plugin"),
                     "debug snapshot should expose the plugin provider name");
            const ps2x::iop::DebugService *service = findService(snapshot, "synthetic-test-profile");
            if (!service)
            {
                t.Fail("debug snapshot should include the synthetic profile service");
                return;
            }
            t.IsTrue(service->profileSpecific, "plugin service should be marked profile-specific");
            t.IsTrue(std::find(service->sids.begin(), service->sids.end(), kSyntheticSid) != service->sids.end(),
                     "plugin service should advertise its synthetic SID");
            t.Equals(metricValue(*service, "reset_generation"), uint64_t{1},
                     "profile configuration should reset a new plugin instance once");

            ps2x::iop::RpcAbiRequest abiRequest{};
            abiRequest.boundSid = kSyntheticSid;
            abiRequest.function = kSyntheticFunction;
            abiRequest.registers.plausible = true;
            abiRequest.stack.plausible = true;
            t.Equals(subsystem.selectRpcAbi(abiRequest), ps2x::iop::RpcAbi::Stack,
                     "plugin should be able to select the stack RPC ABI");
            abiRequest.function = kSyntheticFunction + 1u;
            t.Equals(subsystem.selectRpcAbi(abiRequest), ps2x::iop::RpcAbi::RuntimeDefault,
                     "plugin ABI selection should fall back for unrelated functions");

            constexpr uint32_t kSendAddress = 0x1000u;
            constexpr uint32_t kReceiveAddress = 0x1100u;
            constexpr uint32_t kInput = 0x1234ABCDu;
            t.IsTrue(host.writeWord(kSendAddress, kInput), "fake host should seed the plugin send buffer");
            t.IsTrue(host.writeWord(kReceiveAddress, 0u), "fake host should clear the plugin receive buffer");

            ps2x::iop::RpcRequest request{};
            request.callToken = 0x1122334455667788ull;
            request.sid = kSyntheticSid;
            request.function = kSyntheticFunction;
            request.send = {kSendAddress, sizeof(uint32_t)};
            request.receive = {kReceiveAddress, sizeof(uint32_t)};
            const ps2x::iop::RpcResult result = subsystem.handleRpc(request);

            t.IsTrue(result.handled, "matching synthetic SID/function should dispatch to the plugin");
            t.Equals(result.resultAddress, kReceiveAddress, "plugin should return its receive-buffer address");
            t.IsTrue(result.signalNowaitCompletion, "plugin should request nowait completion signaling");
            t.Equals(result.callbackPolicy, ps2x::iop::CallbackPolicy::Suppress,
                     "plugin should be able to suppress the runtime callback");
            t.Equals(host.readWord(kReceiveAddress), kInput ^ kResponseXor,
                     "plugin should read and write guest memory through the IopHost bridge");

            ps2x::iop::RpcRequest unknownRequest{};
            unknownRequest.sid = 0xDEADC0DEu;
            unknownRequest.function = kSyntheticFunction;
            t.IsFalse(subsystem.handleRpc(unknownRequest).handled,
                      "unknown SID should remain unhandled while a plugin profile is active");

            constexpr uint32_t kCoreCollisionReceiveAddress = 0x1200u;
            ps2x::iop::RpcRequest collisionRequest{};
            collisionRequest.sid = kCoreCollisionSid;
            collisionRequest.function = kCoreCollisionFunction;
            collisionRequest.receive = {kCoreCollisionReceiveAddress, sizeof(uint32_t)};
            const ps2x::iop::RpcResult collisionResult = subsystem.handleRpc(collisionRequest);
            t.IsTrue(collisionResult.handled,
                     "a profile service should take precedence over a core service for the same SID");
            t.Equals(host.readWord(kCoreCollisionReceiveAddress), kCoreCollisionResponse,
                     "the profile collision route should reach the plugin implementation");

            subsystem.onSifTransfer({ps2x::iop::SifTransferKind::SetDma,
                                     ps2x::iop::SifTransferPhase::AfterCopy,
                                     kSendAddress,
                                     kReceiveAddress,
                                     sizeof(uint32_t)});
            snapshot = subsystem.debugSnapshot();
            service = findService(snapshot, "synthetic-test-profile");
            if (!service)
            {
                t.Fail("synthetic profile service should remain visible after dispatch");
                return;
            }
            t.Equals(metricValue(*service, "rpc_calls"), uint64_t{2},
                     "plugin debug metrics should count dispatched RPCs");
            t.Equals(metricValue(*service, "sif_transfers"), uint64_t{1},
                     "plugin debug metrics should count SIF transfer hooks");

            subsystem.reset();
            snapshot = subsystem.debugSnapshot();
            service = findService(snapshot, "synthetic-test-profile");
            if (!service)
            {
                t.Fail("synthetic profile service should remain visible after reset");
                return;
            }
            t.Equals(metricValue(*service, "reset_generation"), uint64_t{2},
                     "explicit subsystem reset should reach the plugin instance");
            t.Equals(metricValue(*service, "rpc_calls"), uint64_t{0},
                     "plugin reset should clear per-instance RPC state");
            t.Equals(metricValue(*service, "sif_transfers"), uint64_t{0},
                     "plugin reset should clear per-instance transfer state");

            error.clear();
            t.IsFalse(subsystem.configure({"synthetic_duplicate.elf", kSyntheticEntryPoint, kSyntheticCrc32}, &error),
                      "duplicate SIDs inside one profile layer should reject configuration");
            t.IsTrue(error.find("duplicate IOP SID") != std::string::npos,
                     "duplicate-SID failure should clearly identify the registry conflict");

            error.clear();
            t.IsFalse(subsystem.configure({"slus_201.84", kSyntheticEntryPoint, kSyntheticCrc32}, &error),
                      "equally specific built-in and plugin matchers should be ambiguous");
            t.IsTrue(error.find("ambiguous IOP profiles") != std::string::npos,
                     "ambiguous profile selection should fail clearly");

            error.clear();
            t.IsTrue(subsystem.configure({"slus_201.84",
                                          kSpecificRecvXEntryPoint,
                                          kSyntheticCrc32},
                                         &error),
                     "a more-specific matcher should win over a lower-specificity tie");
            t.Equals(subsystem.debugSnapshot().activeProfile,
                     std::string("synthetic-specific-recvx-profile"),
                     "the most specific plugin profile should be selected");

            error.clear();
            t.IsTrue(subsystem.configure({"different.elf", kSyntheticEntryPoint, kSyntheticCrc32}, &error),
                     "switching to an unmatched ELF should destroy the active plugin profile");
            t.IsTrue(host.hasLog("fake-plugin-destroy"),
                     "plugin profile destroy callback should run when the active profile is replaced");
            t.IsTrue(subsystem.debugSnapshot().activeProfile.empty(),
                     "switching to an unmatched ELF should leave no active profile");
        });
#endif
    });
}
