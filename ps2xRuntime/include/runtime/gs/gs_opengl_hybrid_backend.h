#pragma once

#include "runtime/gs/gs_backend.h"
#include "runtime/gs/gs_cpu_backend.h"
#include "runtime/gs/gs_opengl_coherence.h"

#include <condition_variable>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

// Keeps the canonical 4 MiB GS image GPU-resident across supported draws and
// falls back to the existing CPU backend at explicit ordered barriers.
class GSOpenGLHybridBackend final : public GSRasterBackend
{
public:
    GSOpenGLHybridBackend();
    ~GSOpenGLHybridBackend() override;

    void Initialize(uint8_t *vram, uint32_t vramSize) override;
    void Reset() override;
    void Submit(const GSPrimitiveBatch &batch) override;
    void BeginTransfer(const GSTransferCommand &command) override;
    void UploadImage(const uint8_t *data, uint32_t sizeBytes) override;
    void Flush() override;
    void TextureFlush() override;
    void Sync(GSSyncReason reason) override;
    PresentationFrame Present(const GSPresentationRequest &request) override;
    bool ClearFramebuffer(const GSContext &context, uint32_t rgba) override;
    uint32_t ConsumeLocalToHostBytes(uint8_t *dst, uint32_t maxBytes) override;
    uint32_t ReadVram(uint32_t psm, uint32_t base, uint32_t bw, uint32_t x, uint32_t y) const override;
    void WriteVram(uint32_t psm, uint32_t base, uint32_t bw, uint32_t x, uint32_t y, uint32_t value) override;
    void SnapshotVram(std::vector<uint8_t> &out) const override;
    GSTransferSnapshot GetTransferSnapshot() const override;

    void ProcessHostWork() override;
    void ShutdownHostResources() override;

private:
    struct Completion { bool done = false; };
    struct Command
    {
        enum class Kind : uint8_t { Draw, Readback, Fence } kind = Kind::Draw;
        GSPrimitiveBatch batch{};
        std::shared_ptr<Completion> completion;
    };

    void EnsureCpuCurrent();
    void WaitForGpuQueue();
    void InvalidateGpuCopy();
    void ProcessCommand(Command &command);
    void ProcessDrawBatch(const std::vector<GSPrimitiveBatch> &batches);
    bool EnsureGpuInitialized(std::string &detail);
    bool UploadCpuDirty(std::string &detail);
    bool ReadbackGpuDirty(std::string &detail);
    bool TrySubmitCpuDisjoint(const GSPrimitiveBatch &batch);

    mutable std::mutex m_queueMutex;
    std::condition_variable m_queueChanged;
    std::deque<Command> m_queue;
    GSCpuBackend m_cpu;
    uint8_t *m_vram = nullptr;
    uint32_t m_vramSize = 0u;
    std::thread::id m_hostThread;
    bool m_gpuEnabled = true;
    bool m_rangeCoherenceEnabled = false;
    bool m_gpuInitialized = false;
    bool m_gpuCopyValid = false;
    bool m_gpuDirty = false;
    bool m_shuttingDown = false;
    size_t m_commandsInFlight = 0u;
    GSOpenGLCoherence::BlockMask m_gpuDirtyBlocks{};
    GSOpenGLCoherence::BlockMask m_cpuDirtyBlocks{};
    uint64_t m_gpuDraws = 0u;
    uint64_t m_gpuBatches = 0u;
    uint64_t m_gpuDispatchNanoseconds = 0u;
    uint64_t m_gpuUploadNanoseconds = 0u;
    uint64_t m_gpuReadbackNanoseconds = 0u;
    uint64_t m_gpuUploadBytes = 0u;
    uint64_t m_gpuReadbackBytes = 0u;
    uint64_t m_cpuFallbacks = 0u;
    uint64_t m_disjointCpuFallbacks = 0u;
    uint64_t m_readbacks = 0u;
};
