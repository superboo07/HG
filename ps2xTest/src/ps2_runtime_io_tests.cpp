#include "MiniTest.h"
#include "ps2_runtime.h"
#include "ps2_syscalls.h"
#include "ps2_stubs.h"
#include "Kernel/Stubs/CD.h"
#include "runtime/ee_scheduler.h"

#include <filesystem>
#include <fstream>
#include <array>
#include <vector>
#include <cstring>
#include <chrono>
#include <cstdlib>
#include <optional>

using namespace ps2_syscalls;

namespace
{
    // Guest memory address ranges for test data
    constexpr uint32_t GUEST_STRING_AREA_START = 0x1000;
    constexpr uint32_t GUEST_BUFFER_AREA_START = 0x2000;
    constexpr uint32_t GUEST_STACK_AREA_START = 0x6000;
    constexpr uint32_t GUEST_MC_SYNC_CMD_ADDR = GUEST_BUFFER_AREA_START + 0x1C00;
    constexpr uint32_t GUEST_MC_SYNC_RESULT_ADDR = GUEST_BUFFER_AREA_START + 0x1C04;
    constexpr uint32_t GUEST_MC_TABLE_ADDR = GUEST_BUFFER_AREA_START + 0x2000;
    
    // Common file I/O flag combinations
    constexpr uint32_t PS2_FIO_WRITE_CREATE_TRUNC = 
        PS2_FIO_O_WRONLY | PS2_FIO_O_CREAT | PS2_FIO_O_TRUNC;

    struct SceMcStDateTime
    {
        uint8_t resv2;
        uint8_t sec;
        uint8_t min;
        uint8_t hour;
        uint8_t day;
        uint8_t month;
        uint16_t year;
    };

    struct SceMcTblGetDir
    {
        SceMcStDateTime create;
        SceMcStDateTime modify;
        uint32_t fileSizeByte;
        uint16_t attrFile;
        uint16_t reserve1;
        uint32_t reserve2;
        uint32_t pdaAplNo;
        char entryName[32];
    };

    static_assert(sizeof(SceMcTblGetDir) == 64, "sceMcTblGetDir size mismatch");

    void setRegU32(R5900Context &ctx, int reg, uint32_t value)
    {
        ctx.r[reg] = _mm_set_epi64x(0, static_cast<int64_t>(value));
    }

    int32_t getRegS32(const R5900Context *ctx, int reg)
    {
        return static_cast<int32_t>(::getRegU32(ctx, reg));
    }

    void writeGuestString(uint8_t *rdram, uint32_t addr, const std::string &value)
    {
        std::memcpy(rdram + addr, value.c_str(), value.size() + 1);
    }

    void writeGuestU32(uint8_t *rdram, uint32_t addr, uint32_t value)
    {
        std::memcpy(rdram + addr, &value, sizeof(value));
    }

    int32_t readGuestS32(const uint8_t *rdram, uint32_t addr)
    {
        int32_t value = 0;
        std::memcpy(&value, rdram + addr, sizeof(value));
        return value;
    }

    uint32_t readGuestU32(const uint8_t *rdram, uint32_t addr)
    {
        uint32_t value = 0;
        std::memcpy(&value, rdram + addr, sizeof(value));
        return value;
    }

    void clearContext(R5900Context &ctx)
    {
        std::memset(&ctx, 0, sizeof(ctx));
    }

    int32_t syncMc(std::vector<uint8_t> &rdram, int32_t *cmdOut = nullptr)
    {
        R5900Context syncCtx{};
        setRegU32(syncCtx, 4, 0u);
        setRegU32(syncCtx, 5, GUEST_MC_SYNC_CMD_ADDR);
        setRegU32(syncCtx, 6, GUEST_MC_SYNC_RESULT_ADDR);
        ps2_stubs::sceMcSync(rdram.data(), &syncCtx, nullptr);

        if (cmdOut)
        {
            *cmdOut = readGuestS32(rdram.data(), GUEST_MC_SYNC_CMD_ADDR);
        }
        return readGuestS32(rdram.data(), GUEST_MC_SYNC_RESULT_ADDR);
    }

    struct TempPaths
    {
        std::filesystem::path base;
        std::filesystem::path mcRoot;
        std::filesystem::path cdRoot;

        ~TempPaths()
        {
            std::error_code ec;
            std::filesystem::remove_all(base, ec);
        }
    };

    TempPaths makeTempPaths()
    {
        TempPaths paths;
        const auto now = std::chrono::steady_clock::now().time_since_epoch().count();
        paths.base = std::filesystem::temp_directory_path()
                   / ("ps2recomp-mc0-" + std::to_string(now));
        paths.mcRoot = paths.base / "mcroot";
        paths.cdRoot = paths.base / "cdroot";
        std::filesystem::create_directories(paths.mcRoot);
        std::filesystem::create_directories(paths.cdRoot);
        return paths;
    }

    void setEnvironmentValue(const char *name, const char *value)
    {
#if defined(_WIN32)
        _putenv_s(name, value ? value : "");
#else
        if (value)
        {
            setenv(name, value, 1);
        }
        else
        {
            unsetenv(name);
        }
#endif
    }

    struct ScopedEnvironment
    {
        explicit ScopedEnvironment(const char *variable, const char *value)
            : name(variable)
        {
            if (const char *current = std::getenv(name.c_str()))
            {
                oldValue = current;
            }
            setEnvironmentValue(name.c_str(), value);
        }

        ~ScopedEnvironment()
        {
            setEnvironmentValue(name.c_str(), oldValue ? oldValue->c_str() : nullptr);
        }

        std::string name;
        std::optional<std::string> oldValue;
    };

    struct TestContext
    {
        TempPaths paths;
        std::vector<uint8_t> rdram;
        R5900Context ctx;

        TestContext() : paths(makeTempPaths()), rdram(PS2_RAM_SIZE, 0)
        {
            PS2Runtime::IoPaths ioPaths;
            ioPaths.elfDirectory = paths.cdRoot;
            ioPaths.hostRoot = paths.cdRoot;
            ioPaths.cdRoot = paths.cdRoot;
            ioPaths.mcRoot = paths.mcRoot;
            PS2Runtime::setIoPaths(ioPaths);
        }
    };
}

void register_ps2_runtime_io_tests()
{
    MiniTest::Case("PS2RuntimeIO", [](TestCase &tc)
    {
        tc.Run("ELF configuration selects a portable per-game memory-card root", [](TestCase &t)
        {
            TempPaths paths = makeTempPaths();
            const PS2Runtime::IoPaths oldPaths = PS2Runtime::getIoPaths();
            const std::filesystem::path platformData = paths.base / "portable-data";
            const std::filesystem::path elfPath = paths.base / "game" / "SLUS_210.75";

            ScopedEnvironment mcOverride("PS2X_MC_ROOT", nullptr);
#if defined(_WIN32)
            ScopedEnvironment platformRoot("LOCALAPPDATA", platformData.string().c_str());
#elif defined(__APPLE__)
            const std::filesystem::path fakeHome = paths.base / "home";
            ScopedEnvironment platformRoot("HOME", fakeHome.string().c_str());
#else
            ScopedEnvironment platformRoot("XDG_DATA_HOME", platformData.string().c_str());
#endif

            PS2Runtime::configureIoPathsFromElf(elfPath.string());
            const PS2Runtime::IoPaths configured = PS2Runtime::getIoPaths();
#if defined(__APPLE__)
            const std::filesystem::path expected = paths.base / "home" / "Library" /
                                                   "Application Support" / "PS2Recomp" /
                                                   "SLUS_210.75" / "mc0";
#else
            const std::filesystem::path expected = platformData / "PS2Recomp" /
                                                   "SLUS_210.75" / "mc0";
#endif
            t.Equals(configured.mcRoot.lexically_normal().string(),
                     std::filesystem::absolute(expected).lexically_normal().string(),
                     "the default save root should use platform user data and isolate the ELF identity");

            const std::filesystem::path explicitRoot = paths.base / "explicit-card";
            {
                ScopedEnvironment explicitOverride("PS2X_MC_ROOT", explicitRoot.string().c_str());
                PS2Runtime::configureIoPathsFromElf(elfPath.string());
                t.Equals(PS2Runtime::getIoPaths().mcRoot.lexically_normal().string(),
                         std::filesystem::absolute(explicitRoot).lexically_normal().string(),
                         "PS2X_MC_ROOT should select an exact portable memory-card directory");
            }

            PS2Runtime::setIoPaths(oldPaths);
        });

        tc.Run("mc0 directory creation", [](TestCase &t)
        {
            TestContext test;

            const std::string dirPath = "mc0:/SAVEDATA";
            const uint32_t dirAddr = GUEST_STRING_AREA_START;
            writeGuestString(test.rdram.data(), dirAddr, dirPath);

            setRegU32(test.ctx, 4, dirAddr);
            fioMkdir(test.rdram.data(), &test.ctx, nullptr);
            
            const int32_t result = getRegS32(&test.ctx, 2);
            t.IsTrue(result >= 0, "fioMkdir should succeed for mc0: directory");

            const std::filesystem::path expected = test.paths.mcRoot / "SAVEDATA";
            t.IsTrue(std::filesystem::exists(expected), 
                "Directory should exist under mcRoot");
            t.IsTrue(std::filesystem::is_directory(expected), 
                "Created path should be a directory");
        });

        tc.Run("mc0 file write operations", [](TestCase &t)
        {
            TestContext test;

            // Setup: create directory first
            const std::string dirPath = "mc0:/SAVEDATA";
            const uint32_t dirAddr = GUEST_STRING_AREA_START;
            writeGuestString(test.rdram.data(), dirAddr, dirPath);
            setRegU32(test.ctx, 4, dirAddr);
            fioMkdir(test.rdram.data(), &test.ctx, nullptr);

            // Test: open file for writing
            const std::string filePath = "mc0:/SAVEDATA/test.txt";
            const uint32_t fileAddr = GUEST_STRING_AREA_START + 0x100;
            writeGuestString(test.rdram.data(), fileAddr, filePath);

            setRegU32(test.ctx, 4, fileAddr);
            setRegU32(test.ctx, 5, PS2_FIO_WRITE_CREATE_TRUNC);
            fioOpen(test.rdram.data(), &test.ctx, nullptr);
            
            const int32_t fd = getRegS32(&test.ctx, 2);
            t.IsTrue(fd >= 0, "fioOpen should return valid file descriptor");

            // Write payload
            const std::string payload = "hello mc0";
            const uint32_t bufAddr = GUEST_BUFFER_AREA_START;
            std::memcpy(test.rdram.data() + bufAddr, payload.data(), payload.size());

            setRegU32(test.ctx, 4, static_cast<uint32_t>(fd));
            setRegU32(test.ctx, 5, bufAddr);
            setRegU32(test.ctx, 6, static_cast<uint32_t>(payload.size()));
            fioWrite(test.rdram.data(), &test.ctx, nullptr);
            
            const int32_t bytesWritten = getRegS32(&test.ctx, 2);
            t.Equals(bytesWritten, static_cast<int32_t>(payload.size()), 
                "fioWrite should write all bytes");

            // Close file
            setRegU32(test.ctx, 4, static_cast<uint32_t>(fd));
            fioClose(test.rdram.data(), &test.ctx, nullptr);
            
            const int32_t closeResult = getRegS32(&test.ctx, 2);
            t.IsTrue(closeResult >= 0, "fioClose should succeed");

            // Verify on host filesystem
            const std::filesystem::path expectedPath = 
                test.paths.mcRoot / "SAVEDATA" / "test.txt";
            t.IsTrue(std::filesystem::exists(expectedPath), 
                "File should exist under mcRoot");

            std::ifstream in(expectedPath, std::ios::binary);
            std::string readback(
                (std::istreambuf_iterator<char>(in)), 
                std::istreambuf_iterator<char>());
            t.Equals(readback, payload, "File content should match written payload");
        });

        tc.Run("mc0 file read operations", [](TestCase &t)
        {
            TestContext test;

            // Setup: create directory and write file
            const std::string dirPath = "mc0:/SAVEDATA";
            const uint32_t dirAddr = GUEST_STRING_AREA_START;
            writeGuestString(test.rdram.data(), dirAddr, dirPath);
            setRegU32(test.ctx, 4, dirAddr);
            fioMkdir(test.rdram.data(), &test.ctx, nullptr);

            const std::string filePath = "mc0:/SAVEDATA/test.txt";
            const uint32_t fileAddr = GUEST_STRING_AREA_START + 0x100;
            writeGuestString(test.rdram.data(), fileAddr, filePath);

            // Write data
            const std::string payload = "hello mc0 read test";
            const uint32_t writeBufAddr = GUEST_BUFFER_AREA_START;
            std::memcpy(test.rdram.data() + writeBufAddr, payload.data(), payload.size());

            setRegU32(test.ctx, 4, fileAddr);
            setRegU32(test.ctx, 5, PS2_FIO_WRITE_CREATE_TRUNC);
            fioOpen(test.rdram.data(), &test.ctx, nullptr);
            int32_t fd = getRegS32(&test.ctx, 2);

            setRegU32(test.ctx, 4, static_cast<uint32_t>(fd));
            setRegU32(test.ctx, 5, writeBufAddr);
            setRegU32(test.ctx, 6, static_cast<uint32_t>(payload.size()));
            fioWrite(test.rdram.data(), &test.ctx, nullptr);

            setRegU32(test.ctx, 4, static_cast<uint32_t>(fd));
            fioClose(test.rdram.data(), &test.ctx, nullptr);

            // Test: read back via fioRead
            setRegU32(test.ctx, 4, fileAddr);
            setRegU32(test.ctx, 5, PS2_FIO_O_RDONLY);
            fioOpen(test.rdram.data(), &test.ctx, nullptr);
            fd = getRegS32(&test.ctx, 2);
            t.IsTrue(fd >= 0, "fioOpen for reading should succeed");

            // Read into different buffer area
            const uint32_t readBufAddr = GUEST_BUFFER_AREA_START + 0x1000;
            std::memset(test.rdram.data() + readBufAddr, 0, payload.size());

            setRegU32(test.ctx, 4, static_cast<uint32_t>(fd));
            setRegU32(test.ctx, 5, readBufAddr);
            setRegU32(test.ctx, 6, static_cast<uint32_t>(payload.size()));
            fioRead(test.rdram.data(), &test.ctx, nullptr);

            const int32_t bytesRead = getRegS32(&test.ctx, 2);
            t.Equals(bytesRead, static_cast<int32_t>(payload.size()), 
                "fioRead should read all bytes");

            std::string readback(
                reinterpret_cast<const char*>(test.rdram.data() + readBufAddr),
                payload.size()
            );
            t.Equals(readback, payload, "fioRead content should match original");

            setRegU32(test.ctx, 4, static_cast<uint32_t>(fd));
            fioClose(test.rdram.data(), &test.ctx, nullptr);
        });

        tc.Run("mc0 paths isolated from cdRoot", [](TestCase &t)
        {
            TestContext test;

            const std::string dirPath = "mc0:/ISOLATED";
            const std::string filePath = "mc0:/ISOLATED/test.txt";
            const uint32_t dirAddr = GUEST_STRING_AREA_START;
            const uint32_t fileAddr = GUEST_STRING_AREA_START + 0x100;
            
            writeGuestString(test.rdram.data(), dirAddr, dirPath);
            writeGuestString(test.rdram.data(), fileAddr, filePath);

            // Create directory and file on mc0:
            setRegU32(test.ctx, 4, dirAddr);
            fioMkdir(test.rdram.data(), &test.ctx, nullptr);

            setRegU32(test.ctx, 4, fileAddr);
            setRegU32(test.ctx, 5, PS2_FIO_WRITE_CREATE_TRUNC);
            fioOpen(test.rdram.data(), &test.ctx, nullptr);
            const int32_t fd = getRegS32(&test.ctx, 2);

            const std::string payload = "isolation test";
            const uint32_t bufAddr = GUEST_BUFFER_AREA_START;
            std::memcpy(test.rdram.data() + bufAddr, payload.data(), payload.size());

            setRegU32(test.ctx, 4, static_cast<uint32_t>(fd));
            setRegU32(test.ctx, 5, bufAddr);
            setRegU32(test.ctx, 6, static_cast<uint32_t>(payload.size()));
            fioWrite(test.rdram.data(), &test.ctx, nullptr);

            setRegU32(test.ctx, 4, static_cast<uint32_t>(fd));
            fioClose(test.rdram.data(), &test.ctx, nullptr);

            // Verify isolation
            const std::filesystem::path expectedMc = 
                test.paths.mcRoot / "ISOLATED" / "test.txt";
            const std::filesystem::path unexpectedCd = 
                test.paths.cdRoot / "ISOLATED" / "test.txt";

            t.IsTrue(std::filesystem::exists(expectedMc), 
                "mc0: file should exist under mcRoot");
            t.IsFalse(std::filesystem::exists(unexpectedCd), 
                "mc0: file should NOT exist under cdRoot");

            // Verify mcRoot directory structure
            t.IsTrue(std::filesystem::exists(test.paths.mcRoot / "ISOLATED"), 
                "mc0: directory should exist under mcRoot");
            t.IsFalse(std::filesystem::exists(test.paths.cdRoot / "ISOLATED"), 
                "mc0: directory should NOT exist under cdRoot");
        });

        tc.Run("sceMc byte-stable roundtrip survives libmc cold reinit", [](TestCase &t)
        {
            TestContext test;

            const uint32_t dirAddr = GUEST_STRING_AREA_START + 0x400;
            const uint32_t fileAddr = GUEST_STRING_AREA_START + 0x500;
            const uint32_t writeBufAddr = GUEST_BUFFER_AREA_START + 0x300;
            const uint32_t readBufAddr = GUEST_BUFFER_AREA_START + 0x500;
            constexpr std::array<uint8_t, 16> payload = {
                0x00u, 0x01u, 0xFEu, 0xFFu,
                0x78u, 0x56u, 0x34u, 0x12u,
                0x12u, 0x34u, 0x56u, 0x78u,
                0xA5u, 0x5Au, 0x80u, 0x7Fu,
            };

            writeGuestString(test.rdram.data(), dirAddr, "/SAVEDATA");
            writeGuestString(test.rdram.data(), fileAddr, "/SAVEDATA/test.bin");
            std::memcpy(test.rdram.data() + writeBufAddr, payload.data(), payload.size());

            clearContext(test.ctx);
            setRegU32(test.ctx, 4, 0u);
            setRegU32(test.ctx, 5, 0u);
            setRegU32(test.ctx, 6, dirAddr);
            ps2_stubs::sceMcMkdir(test.rdram.data(), &test.ctx, nullptr);
            t.Equals(getRegS32(&test.ctx, 2), 0, "sceMcMkdir should dispatch successfully");

            int32_t cmd = 0;
            t.Equals(syncMc(test.rdram, &cmd), 0, "sceMcMkdir should finish successfully");
            t.Equals(cmd, 0x0B, "sceMcSync should report MKDIR as the last command");

            clearContext(test.ctx);
            setRegU32(test.ctx, 4, 0u);
            setRegU32(test.ctx, 5, 0u);
            setRegU32(test.ctx, 6, fileAddr);
            setRegU32(test.ctx, 7, PS2_FIO_O_RDWR | PS2_FIO_O_CREAT | PS2_FIO_O_TRUNC);
            ps2_stubs::sceMcOpen(test.rdram.data(), &test.ctx, nullptr);
            t.Equals(getRegS32(&test.ctx, 2), 0, "sceMcOpen should dispatch successfully");

            const int32_t fd = syncMc(test.rdram, &cmd);
            t.IsTrue(fd > 0, "sceMcOpen should produce a positive descriptor in sceMcSync");
            t.Equals(cmd, 0x02, "sceMcSync should report OPEN as the last command");

            clearContext(test.ctx);
            setRegU32(test.ctx, 4, static_cast<uint32_t>(fd));
            setRegU32(test.ctx, 5, writeBufAddr);
            setRegU32(test.ctx, 6, static_cast<uint32_t>(payload.size()));
            ps2_stubs::sceMcWrite(test.rdram.data(), &test.ctx, nullptr);
            t.Equals(syncMc(test.rdram, &cmd), static_cast<int32_t>(payload.size()),
                     "sceMcWrite should report the full byte count via sceMcSync");
            t.Equals(cmd, 0x06, "sceMcSync should report WRITE as the last command");

            clearContext(test.ctx);
            setRegU32(test.ctx, 4, static_cast<uint32_t>(fd));
            setRegU32(test.ctx, 5, 0u);
            setRegU32(test.ctx, 6, PS2_FIO_SEEK_SET);
            ps2_stubs::sceMcSeek(test.rdram.data(), &test.ctx, nullptr);
            t.Equals(syncMc(test.rdram, &cmd), 0, "sceMcSeek should rewind to offset zero");
            t.Equals(cmd, 0x04, "sceMcSync should report SEEK as the last command");

            std::memset(test.rdram.data() + readBufAddr, 0, payload.size());
            clearContext(test.ctx);
            setRegU32(test.ctx, 4, static_cast<uint32_t>(fd));
            setRegU32(test.ctx, 5, readBufAddr);
            setRegU32(test.ctx, 6, static_cast<uint32_t>(payload.size()));
            ps2_stubs::sceMcRead(test.rdram.data(), &test.ctx, nullptr);
            t.Equals(syncMc(test.rdram, &cmd), static_cast<int32_t>(payload.size()),
                     "sceMcRead should report the full byte count via sceMcSync");
            t.Equals(cmd, 0x05, "sceMcSync should report READ as the last command");

            t.Equals(std::memcmp(test.rdram.data() + readBufAddr, payload.data(), payload.size()),
                     0,
                     "sceMcRead should preserve every literal save byte");

            clearContext(test.ctx);
            setRegU32(test.ctx, 4, static_cast<uint32_t>(fd));
            ps2_stubs::sceMcClose(test.rdram.data(), &test.ctx, nullptr);
            t.Equals(syncMc(test.rdram, &cmd), 0, "sceMcClose should finish successfully");
            t.Equals(cmd, 0x03, "sceMcSync should report CLOSE as the last command");

            const std::filesystem::path hostPath = test.paths.mcRoot / "SAVEDATA" / "test.bin";
            t.IsTrue(std::filesystem::exists(hostPath), "sceMcOpen/sceMcWrite should create the host file under mcRoot");

            std::array<uint8_t, payload.size()> hostBytes{};
            {
                std::ifstream input(hostPath, std::ios::binary);
                input.read(reinterpret_cast<char *>(hostBytes.data()),
                           static_cast<std::streamsize>(hostBytes.size()));
            }
            t.IsTrue(hostBytes == payload,
                     "the portable save file should contain the exact guest byte sequence");

            clearContext(test.ctx);
            ps2_stubs::sceMcEnd(test.rdram.data(), &test.ctx, nullptr);
            std::fill(test.rdram.begin(), test.rdram.end(), 0u);
            writeGuestString(test.rdram.data(), fileAddr, "/SAVEDATA/test.bin");

            clearContext(test.ctx);
            setRegU32(test.ctx, 4, 0u);
            setRegU32(test.ctx, 5, 0u);
            setRegU32(test.ctx, 6, fileAddr);
            setRegU32(test.ctx, 7, PS2_FIO_O_RDONLY);
            ps2_stubs::sceMcOpen(test.rdram.data(), &test.ctx, nullptr);
            const int32_t coldFd = syncMc(test.rdram, &cmd);
            t.IsTrue(coldFd > 0, "a cold libmc reinit should reopen the persisted save");

            clearContext(test.ctx);
            setRegU32(test.ctx, 4, static_cast<uint32_t>(coldFd));
            setRegU32(test.ctx, 5, readBufAddr);
            setRegU32(test.ctx, 6, static_cast<uint32_t>(payload.size()));
            ps2_stubs::sceMcRead(test.rdram.data(), &test.ctx, nullptr);
            t.Equals(syncMc(test.rdram, &cmd), static_cast<int32_t>(payload.size()),
                     "a cold libmc reinit should read the complete persisted save");
            t.Equals(std::memcmp(test.rdram.data() + readBufAddr, payload.data(), payload.size()),
                     0,
                     "cold-reload bytes should remain endian-stable");
        });

        tc.Run("sceMcGetDir includes dot entries and file metadata", [](TestCase &t)
        {
            TestContext test;

            std::filesystem::create_directories(test.paths.mcRoot / "SAVEDATA");
            const std::string hostPayload = "abc123";
            {
                std::ofstream out(test.paths.mcRoot / "SAVEDATA" / "game.dat", std::ios::binary);
                out.write(hostPayload.data(), static_cast<std::streamsize>(hostPayload.size()));
            }

            const uint32_t patternAddr = GUEST_STRING_AREA_START + 0x700;
            writeGuestString(test.rdram.data(), patternAddr, "/SAVEDATA/*");

            clearContext(test.ctx);
            setRegU32(test.ctx, 4, 0u);
            setRegU32(test.ctx, 5, 0u);
            setRegU32(test.ctx, 6, patternAddr);
            setRegU32(test.ctx, 7, 0u);
            // EE n32 ABI: arguments 5 and 6 travel in $t0/$t1
            setRegU32(test.ctx, 8, 8u);
            setRegU32(test.ctx, 9, GUEST_MC_TABLE_ADDR);

            ps2_stubs::sceMcGetDir(test.rdram.data(), &test.ctx, nullptr);

            int32_t cmd = 0;
            const int32_t entryCount = syncMc(test.rdram, &cmd);
            t.Equals(cmd, 0x0D, "sceMcSync should report GETDIR as the last command");
            t.Equals(entryCount, 3, "sceMcGetDir should return '.', '..', and the matching file");

            const auto *entries = reinterpret_cast<const SceMcTblGetDir *>(test.rdram.data() + GUEST_MC_TABLE_ADDR);
            t.Equals(std::string(entries[0].entryName), std::string("."), "sceMcGetDir should return '.' first");
            t.Equals(std::string(entries[1].entryName), std::string(".."), "sceMcGetDir should return '..' second");
            t.Equals(std::string(entries[2].entryName), std::string("game.dat"), "sceMcGetDir should include the matching file entry");
            t.Equals(entries[2].fileSizeByte, static_cast<uint32_t>(hostPayload.size()),
                     "sceMcGetDir should report the host file size");
            t.IsTrue((entries[2].attrFile & 0x0080u) != 0u,
                     "sceMcGetDir file entries should carry the closed-file attribute");
            const uint8_t *fileEntryBytes =
                test.rdram.data() + GUEST_MC_TABLE_ADDR + 2u * sizeof(SceMcTblGetDir);
            constexpr std::array<uint8_t, 6> kExpectedLittleEndianFields = {
                0x06u, 0x00u, 0x00u, 0x00u, 0x93u, 0x80u,
            };
            t.Equals(std::memcmp(fileEntryBytes + 16u,
                                 kExpectedLittleEndianFields.data(),
                                 kExpectedLittleEndianFields.size()),
                     0,
                     "sceMcGetDir size and attributes should use literal PS2 little-endian bytes");
        });

        tc.Run("sceMcGetDir nonpositive maxent performs no lookup", [](TestCase &t)
        {
            TestContext test;

            const uint32_t patternAddr = GUEST_STRING_AREA_START + 0x780;
            writeGuestString(test.rdram.data(), patternAddr, "/BASLUS-21075HG/*");

            uint8_t expectedTable[sizeof(SceMcTblGetDir)];
            std::memset(expectedTable, 0xA5, sizeof(expectedTable));

            for (const int32_t maxEntries : {-1, 0})
            {
                std::memset(test.rdram.data() + GUEST_MC_TABLE_ADDR,
                            0xA5,
                            sizeof(expectedTable));
                clearContext(test.ctx);
                setRegU32(test.ctx, 4, 0u);
                setRegU32(test.ctx, 5, 0u);
                setRegU32(test.ctx, 6, patternAddr);
                setRegU32(test.ctx, 7, 0u);
                setRegU32(test.ctx, 8, static_cast<uint32_t>(maxEntries));
                setRegU32(test.ctx, 9, GUEST_MC_TABLE_ADDR);

                ps2_stubs::sceMcGetDir(test.rdram.data(), &test.ctx, nullptr);

                int32_t cmd = 0;
                t.Equals(syncMc(test.rdram, &cmd), 0,
                         "nonpositive maxent should complete with zero entries");
                t.Equals(cmd, 0x0D,
                         "sceMcSync should report GETDIR for nonpositive maxent");
                t.Equals(std::memcmp(test.rdram.data() + GUEST_MC_TABLE_ADDR,
                                     expectedTable,
                                     sizeof(expectedTable)),
                         0,
                         "nonpositive maxent should leave the result table untouched");
                t.IsFalse(std::filesystem::exists(test.paths.mcRoot / "BASLUS-21075HG"),
                          "nonpositive maxent should not create or resolve the queried path");
            }
        });

        tc.Run("sceMcGetInfo reports formatted and unformatted states", [](TestCase &t)
        {
            TestContext test;

            constexpr uint32_t typeAddr = GUEST_BUFFER_AREA_START + 0x900;
            constexpr uint32_t freeAddr = GUEST_BUFFER_AREA_START + 0x904;
            constexpr uint32_t formatAddr = GUEST_BUFFER_AREA_START + 0x908;

            clearContext(test.ctx);
            setRegU32(test.ctx, 4, 0u);
            setRegU32(test.ctx, 5, 0u);
            setRegU32(test.ctx, 6, typeAddr);
            setRegU32(test.ctx, 7, freeAddr);
            // EE n32 ABI: the fifth argument travels in $t0
            setRegU32(test.ctx, 8, formatAddr);
            ps2_stubs::sceMcGetInfo(test.rdram.data(), &test.ctx, nullptr);

            int32_t cmd = 0;
            t.Equals(syncMc(test.rdram, &cmd), 0, "formatted cards should report success through sceMcSync");
            t.Equals(cmd, 0x01, "sceMcSync should report GETINFO as the last command");
            t.Equals(readGuestS32(test.rdram.data(), typeAddr), 2, "sceMcGetInfo should report a PS2 memory card");
            t.Equals(readGuestS32(test.rdram.data(), formatAddr), 1, "sceMcGetInfo should report a formatted card");

            clearContext(test.ctx);
            setRegU32(test.ctx, 4, 0u);
            setRegU32(test.ctx, 5, 0u);
            ps2_stubs::sceMcUnformat(test.rdram.data(), &test.ctx, nullptr);
            t.Equals(syncMc(test.rdram, &cmd), 0, "sceMcUnformat should complete successfully");

            clearContext(test.ctx);
            setRegU32(test.ctx, 4, 0u);
            setRegU32(test.ctx, 5, 0u);
            setRegU32(test.ctx, 6, typeAddr);
            setRegU32(test.ctx, 7, freeAddr);
            setRegU32(test.ctx, 8, formatAddr);
            ps2_stubs::sceMcGetInfo(test.rdram.data(), &test.ctx, nullptr);

            t.Equals(syncMc(test.rdram, &cmd), -2, "unformatted cards should report sceMcResNoFormat through sceMcSync");
            t.Equals(readGuestS32(test.rdram.data(), formatAddr), 0, "sceMcGetInfo should report an unformatted card after sceMcUnformat");
        });

        tc.Run("sceMcEnd resets libmc state so sync reports no active command", [](TestCase &t)
        {
            TestContext test;

            constexpr uint32_t dirAddr = GUEST_STRING_AREA_START + 0xB00;

            clearContext(test.ctx);
            ps2_stubs::sceMcInit(test.rdram.data(), &test.ctx, nullptr);
            t.Equals(getRegS32(&test.ctx, 2), 0, "sceMcInit should succeed");

            writeGuestString(test.rdram.data(), dirAddr, "/SAVEDATA");
            clearContext(test.ctx);
            setRegU32(test.ctx, 4, 0u);
            setRegU32(test.ctx, 5, 0u);
            setRegU32(test.ctx, 6, dirAddr);
            ps2_stubs::sceMcMkdir(test.rdram.data(), &test.ctx, nullptr);
            int32_t cmd = 0;
            t.Equals(syncMc(test.rdram, &cmd), 0, "sceMcMkdir should complete before teardown");
            t.Equals(cmd, 0x0B, "sceMcSync should report MKDIR before teardown");

            clearContext(test.ctx);
            ps2_stubs::sceMcEnd(test.rdram.data(), &test.ctx, nullptr);
            t.Equals(getRegS32(&test.ctx, 2), 0, "sceMcEnd should succeed");

            // libmc semantics: with no async command pending, sceMcSync returns -1
            // and leaves the cmd/result out-parameters untouched.
            R5900Context syncCtx{};
            setRegU32(syncCtx, 4, 0u);
            setRegU32(syncCtx, 5, GUEST_MC_SYNC_CMD_ADDR);
            setRegU32(syncCtx, 6, GUEST_MC_SYNC_RESULT_ADDR);
            ps2_stubs::sceMcSync(test.rdram.data(), &syncCtx, nullptr);
            t.Equals(getRegS32(&syncCtx, 2), -1,
                     "sceMcSync after sceMcEnd should report that no command is active");
        });

        tc.Run("sceIoctl cmd1 updates wait flag state", [](TestCase &t)
        {
            TestContext test;

            constexpr uint32_t statusAddr = GUEST_BUFFER_AREA_START + 0x1800;
            const uint32_t busy = 1u;
            std::memcpy(test.rdram.data() + statusAddr, &busy, sizeof(busy));

            setRegU32(test.ctx, 4, 3u);          // fd
            setRegU32(test.ctx, 5, 1u);          // cmd
            setRegU32(test.ctx, 6, statusAddr);  // arg

            ps2_stubs::sceIoctl(test.rdram.data(), &test.ctx, nullptr);

            t.Equals(getRegS32(&test.ctx, 2), 0, "sceIoctl cmd1 should return success");

            uint32_t state = 0xFFFFFFFFu;
            std::memcpy(&state, test.rdram.data() + statusAddr, sizeof(state));
            t.Equals(state, 0u, "sceIoctl cmd1 should clear wait state from busy to ready");
        });

        tc.Run("sceCdSearchFile resolves movie filenames with zero-padded host leaf", [](TestCase &t)
        {
            TestContext test;

            std::filesystem::create_directories(test.paths.cdRoot / "movie");
            {
                std::ofstream out(test.paths.cdRoot / "movie" / "mv_016.pss", std::ios::binary);
                const std::string payload = "pss";
                out.write(payload.data(), static_cast<std::streamsize>(payload.size()));
            }

            constexpr uint32_t fileAddr = GUEST_BUFFER_AREA_START + 0x1A00;
            constexpr uint32_t pathAddr = GUEST_STRING_AREA_START + 0xA00;
            writeGuestString(test.rdram.data(), pathAddr, "\\MOVIE\\MV_16.PSS;1");

            clearContext(test.ctx);
            setRegU32(test.ctx, 4, fileAddr);
            setRegU32(test.ctx, 5, pathAddr);
            ps2_stubs::sceCdSearchFile(test.rdram.data(), &test.ctx, nullptr);

            t.Equals(getRegS32(&test.ctx, 2), 1, "sceCdSearchFile should resolve the extracted movie file");
            t.Equals(readGuestU32(test.rdram.data(), fileAddr + 4), 3u,
                     "sceCdSearchFile should report the host file size");
            t.IsTrue(readGuestU32(test.rdram.data(), fileAddr + 0) >= 0x00100000u,
                     "sceCdSearchFile should assign a pseudo LSN for the resolved host file");
        });

        tc.Run("sceCdRead reads from explicit cdImage path", [](TestCase &t)
        {
            TestContext test;

            constexpr uint32_t kSectorSize = 2048u;
            constexpr uint32_t bufAddr = GUEST_BUFFER_AREA_START + 0x1C80;
            const std::filesystem::path imagePath = test.paths.base / "disc.iso";
            {
                std::vector<uint8_t> sector(kSectorSize, 0);
                const char payload[] = "cd-image";
                std::memcpy(sector.data(), payload, sizeof(payload) - 1);

                std::ofstream out(imagePath, std::ios::binary);
                out.write(reinterpret_cast<const char *>(sector.data()),
                          static_cast<std::streamsize>(sector.size()));
            }

            PS2Runtime::IoPaths ioPaths;
            ioPaths.elfDirectory = test.paths.cdRoot;
            ioPaths.hostRoot = test.paths.cdRoot;
            ioPaths.cdRoot = test.paths.cdRoot;
            ioPaths.mcRoot = test.paths.mcRoot;
            ioPaths.cdImage = imagePath;
            PS2Runtime::setIoPaths(ioPaths);

            clearContext(test.ctx);
            setRegU32(test.ctx, 4, 0u);
            setRegU32(test.ctx, 5, 1u);
            setRegU32(test.ctx, 6, bufAddr);
            ps2_stubs::sceCdRead(test.rdram.data(), &test.ctx, nullptr);

            t.Equals(getRegS32(&test.ctx, 2), 1, "sceCdRead should succeed when cdImage is configured");
            t.Equals(std::memcmp(test.rdram.data() + bufAddr, "cd-image", 8), 0,
                     "sceCdRead should copy sector data from the configured image");
        });

        tc.Run("sceCdRead with a runtime defers guest data and reports busy", [](TestCase &t)
        {
            TestContext test;
            PS2Runtime runtime;
            runtime.eeScheduler().bindMainContextForSyscall(test.ctx, test.rdram.data());

            constexpr uint32_t kSectorSize = 2048u;
            constexpr uint32_t kBufferAddress = GUEST_BUFFER_AREA_START + 0x1C80;
            const std::filesystem::path imagePath = test.paths.base / "async-disc.iso";
            {
                std::vector<uint8_t> sector(kSectorSize, 0u);
                const char payload[] = "deferred-cd-image";
                std::memcpy(sector.data(), payload, sizeof(payload) - 1u);
                std::ofstream out(imagePath, std::ios::binary);
                out.write(reinterpret_cast<const char *>(sector.data()),
                          static_cast<std::streamsize>(sector.size()));
            }

            PS2Runtime::IoPaths ioPaths;
            ioPaths.elfDirectory = test.paths.cdRoot;
            ioPaths.hostRoot = test.paths.cdRoot;
            ioPaths.cdRoot = test.paths.cdRoot;
            ioPaths.mcRoot = test.paths.mcRoot;
            ioPaths.cdImage = imagePath;
            PS2Runtime::setIoPaths(ioPaths);

            clearContext(test.ctx);
            ps2_stubs::sceCdInit(test.rdram.data(), &test.ctx, &runtime);
            std::memset(test.rdram.data() + kBufferAddress, 0xCC, kSectorSize);

            clearContext(test.ctx);
            setRegU32(test.ctx, 4, 0u);
            setRegU32(test.ctx, 5, 1u);
            setRegU32(test.ctx, 6, kBufferAddress);
            ps2_stubs::sceCdRead(test.rdram.data(), &test.ctx, &runtime);

            t.Equals(getRegS32(&test.ctx, 2), 1,
                     "an asynchronous disc command should be accepted");
            t.Equals(static_cast<uint32_t>(test.rdram[kBufferAddress]), 0xCCu,
                     "accepted asynchronous reads must not expose data before completion");

            const ps2_stubs::CdDebugSnapshot pending = ps2_stubs::getCdDebugSnapshot();
            t.IsTrue(pending.readBusy,
                     "the accepted read should remain busy until its scheduled completion");
            t.Equals(pending.readLbn, 0u,
                     "the pending read should preserve its source LBN");
            t.Equals(pending.readSectors, 1u,
                     "the pending read should preserve its sector count");
            t.Equals(pending.readDestination, kBufferAddress,
                     "the pending read should preserve its guest destination");
            t.Equals(static_cast<uint32_t>(pending.readBytes), kSectorSize,
                     "the staged transfer should preserve the exact byte count");

            clearContext(test.ctx);
            setRegU32(test.ctx, 4, 1u);
            ps2_stubs::sceCdSync(test.rdram.data(), &test.ctx, &runtime);
            t.Equals(getRegS32(&test.ctx, 2), 1,
                     "nonblocking sceCdSync should report an active command");

            clearContext(test.ctx);
            ps2_stubs::sceCdInit(test.rdram.data(), &test.ctx, &runtime);
            t.IsFalse(ps2_stubs::getCdDebugSnapshot().readBusy,
                      "reinitializing libcdvd should cancel the staged test request");
        });

        tc.Run("sceCdRead uses an exact extracted-file disc mapping", [](TestCase &t)
        {
            TestContext test;

            constexpr uint32_t kSectorSize = 2048u;
            constexpr uint32_t kBaseLbn = 0x00164972u;
            constexpr uint32_t kBufferAddress = GUEST_BUFFER_AREA_START + 0x1E80;
            const std::filesystem::path dataPath = test.paths.cdRoot / "DATA.CVM";
            {
                std::vector<uint8_t> contents(kSectorSize * 2u, 0u);
                const char payload[] = "mapped-sector";
                std::memcpy(contents.data() + kSectorSize, payload, sizeof(payload) - 1u);
                std::ofstream out(dataPath, std::ios::binary);
                out.write(reinterpret_cast<const char *>(contents.data()),
                          static_cast<std::streamsize>(contents.size()));
            }

            t.IsTrue(ps2_stubs::registerCdFileMapping("\\DATA.CVM;1",
                                                       kBaseLbn,
                                                       kSectorSize * 2u),
                     "an extracted file with the verified size should accept its exact disc LSN");
            t.IsFalse(ps2_stubs::registerCdFileMapping("\\DATA.CVM;1",
                                                        kBaseLbn,
                                                        kSectorSize),
                      "a mismatched expected size must not replace an exact mapping");

            clearContext(test.ctx);
            setRegU32(test.ctx, 4, kBaseLbn + 1u);
            setRegU32(test.ctx, 5, 1u);
            setRegU32(test.ctx, 6, kBufferAddress);
            ps2_stubs::sceCdRead(test.rdram.data(), &test.ctx, nullptr);

            t.Equals(getRegS32(&test.ctx, 2), 1,
                     "sceCdRead should resolve sectors through the exact extracted-file mapping");
            t.Equals(std::memcmp(test.rdram.data() + kBufferAddress, "mapped-sector", 13), 0,
                     "the mapped LSN offset should select the corresponding host-file sector");

            clearContext(test.ctx);
            setRegU32(test.ctx, 4, kBaseLbn + 1u);
            setRegU32(test.ctx, 5, 2u);
            setRegU32(test.ctx, 6, kBufferAddress);
            ps2_stubs::sceCdRead(test.rdram.data(), &test.ctx, nullptr);
            t.Equals(getRegS32(&test.ctx, 2), 0,
                     "a sector request crossing the mapped file boundary must fail");
        });
    });
}
