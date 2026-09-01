#include "runtime/gs/gs_opengl_hybrid_backend.h"

#include "runtime/gs/gs_opengl_compute.h"

#include <cstdlib>
#include <cstring>
#include <algorithm>
#include <cmath>
#include <chrono>
#include <iostream>
#include <utility>

namespace
{
    bool traceOpenGlSynchronization()
    {
        static const bool enabled =
            std::getenv("PS2X_GS_OPENGL_SYNC_TRACE") != nullptr;
        return enabled;
    }

    struct DrawBounds
    {
        int minX, maxX, minY, maxY;
        uint64_t area() const
        {
            return static_cast<uint64_t>(std::max(maxX - minX + 1, 0)) *
                   static_cast<uint64_t>(std::max(maxY - minY + 1, 0));
        }
    };

    DrawBounds boundsFor(const GSPrimitiveBatch &batch)
    {
        const GSContext &context = batch.state.context;
        const float ofx = static_cast<float>(context.xyoffset.ofx >> 4);
        const float ofy = static_cast<float>(context.xyoffset.ofy >> 4);
        const bool sprite = batch.state.prim.type == GS_PRIM_SPRITE;
        const float minVertexX = sprite
            ? std::min(batch.vertices[0].x, batch.vertices[1].x)
            : std::min({batch.vertices[0].x, batch.vertices[1].x, batch.vertices[2].x});
        const float maxVertexX = sprite
            ? std::max(batch.vertices[0].x, batch.vertices[1].x)
            : std::max({batch.vertices[0].x, batch.vertices[1].x, batch.vertices[2].x});
        const float minVertexY = sprite
            ? std::min(batch.vertices[0].y, batch.vertices[1].y)
            : std::min({batch.vertices[0].y, batch.vertices[1].y, batch.vertices[2].y});
        const float maxVertexY = sprite
            ? std::max(batch.vertices[0].y, batch.vertices[1].y)
            : std::max({batch.vertices[0].y, batch.vertices[1].y, batch.vertices[2].y});
        DrawBounds result{
            static_cast<int>(std::floor(minVertexX - ofx)),
            static_cast<int>(std::ceil(maxVertexX - ofx)) - (sprite ? 1 : 0),
            static_cast<int>(std::floor(minVertexY - ofy)),
            static_cast<int>(std::ceil(maxVertexY - ofy)) - (sprite ? 1 : 0)};
        result.minX = std::clamp(result.minX, static_cast<int>(context.scissor.x0), static_cast<int>(context.scissor.x1));
        result.maxX = std::clamp(result.maxX, static_cast<int>(context.scissor.x0), static_cast<int>(context.scissor.x1));
        result.minY = std::clamp(result.minY, static_cast<int>(context.scissor.y0), static_cast<int>(context.scissor.y1));
        result.maxY = std::clamp(result.maxY, static_cast<int>(context.scissor.y0), static_cast<int>(context.scissor.y1));
        return result;
    }
}

GSOpenGLHybridBackend::GSOpenGLHybridBackend()
    : m_hostThread(std::this_thread::get_id())
{
}

GSOpenGLHybridBackend::~GSOpenGLHybridBackend()
{
    if (std::this_thread::get_id() == m_hostThread)
        ShutdownHostResources();
}

void GSOpenGLHybridBackend::Initialize(uint8_t *vram, uint32_t vramSize)
{
    m_hostThread = std::this_thread::get_id();
    m_vram = vram;
    m_vramSize = vramSize;
    m_cpu.Initialize(vram, vramSize);
    m_gpuEnabled = std::getenv("PS2X_DISABLE_OPENGL_GS") == nullptr;
    m_rangeCoherenceEnabled =
        std::getenv("PS2X_ENABLE_OPENGL_GS_DIRTY_RANGES") != nullptr;
    m_gpuInitialized = false;
    m_gpuCopyValid = false;
    m_gpuDirty = false;
    m_gpuDirtyBlocks.reset();
    m_cpuDirtyBlocks.reset();
    m_commandsInFlight = 0u;
    m_shuttingDown = false;
}

void GSOpenGLHybridBackend::Reset()
{
    EnsureCpuCurrent();
    m_cpu.Reset();
    InvalidateGpuCopy();
}

void GSOpenGLHybridBackend::Submit(const GSPrimitiveBatch &batch)
{
    if (m_gpuEnabled && GSOpenGLT8CanDispatch(batch))
    {
        if (traceOpenGlSynchronization())
            std::cerr << "[gs-opengl-sync] eligible draw begin prim="
                      << static_cast<uint32_t>(batch.state.prim.type) << std::endl;
        // A CPU triangle run may still be deferred inside the fallback backend.
        // Retire it before the GPU snapshots canonical local memory.
        m_cpu.Flush();
        if (traceOpenGlSynchronization())
            std::cerr << "[gs-opengl-sync] eligible draw CPU flush complete" << std::endl;
        std::lock_guard lock(m_queueMutex);
        m_queue.push_back(Command{Command::Kind::Draw, batch, {}});
        if (traceOpenGlSynchronization())
            std::cerr << "[gs-opengl-sync] eligible draw queued size="
                      << m_queue.size() << std::endl;
        return;
    }

    if (m_gpuEnabled && m_rangeCoherenceEnabled && TrySubmitCpuDisjoint(batch))
        return;

    EnsureCpuCurrent();
    m_cpu.Submit(batch);
    ++m_cpuFallbacks;
    const auto usage = GSOpenGLCoherence::DescribeDraw(batch);
    if (m_rangeCoherenceEnabled && usage.exact)
    {
        std::lock_guard lock(m_queueMutex);
        m_cpuDirtyBlocks |= usage.writes;
    }
    else
    {
        InvalidateGpuCopy();
    }
}

void GSOpenGLHybridBackend::BeginTransfer(const GSTransferCommand &command)
{
    EnsureCpuCurrent();
    m_cpu.BeginTransfer(command);
    InvalidateGpuCopy();
}

void GSOpenGLHybridBackend::UploadImage(const uint8_t *data, uint32_t sizeBytes)
{
    EnsureCpuCurrent();
    m_cpu.UploadImage(data, sizeBytes);
    InvalidateGpuCopy();
}

void GSOpenGLHybridBackend::Flush()
{
    EnsureCpuCurrent();
    m_cpu.Flush();
}

void GSOpenGLHybridBackend::TextureFlush()
{
    // Consecutive compute dispatches already carry an SSBO visibility barrier.
    // This still retires a possible CPU-side triangle run before a later upload.
    m_cpu.TextureFlush();
}

void GSOpenGLHybridBackend::Sync(GSSyncReason reason)
{
    EnsureCpuCurrent();
    m_cpu.Sync(reason);
}

PresentationFrame GSOpenGLHybridBackend::Present(const GSPresentationRequest &request)
{
    EnsureCpuCurrent();
    return m_cpu.Present(request);
}

bool GSOpenGLHybridBackend::ClearFramebuffer(const GSContext &context, uint32_t rgba)
{
    EnsureCpuCurrent();
    const bool result = m_cpu.ClearFramebuffer(context, rgba);
    InvalidateGpuCopy();
    return result;
}

uint32_t GSOpenGLHybridBackend::ConsumeLocalToHostBytes(uint8_t *dst, uint32_t maxBytes)
{
    EnsureCpuCurrent();
    return m_cpu.ConsumeLocalToHostBytes(dst, maxBytes);
}

uint32_t GSOpenGLHybridBackend::ReadVram(uint32_t psm, uint32_t base, uint32_t bw,
                                        uint32_t x, uint32_t y) const
{
    const_cast<GSOpenGLHybridBackend *>(this)->EnsureCpuCurrent();
    return m_cpu.ReadVram(psm, base, bw, x, y);
}

void GSOpenGLHybridBackend::WriteVram(uint32_t psm, uint32_t base, uint32_t bw,
                                     uint32_t x, uint32_t y, uint32_t value)
{
    EnsureCpuCurrent();
    m_cpu.WriteVram(psm, base, bw, x, y, value);
    InvalidateGpuCopy();
}

void GSOpenGLHybridBackend::SnapshotVram(std::vector<uint8_t> &out) const
{
    const_cast<GSOpenGLHybridBackend *>(this)->EnsureCpuCurrent();
    m_cpu.SnapshotVram(out);
}

GSTransferSnapshot GSOpenGLHybridBackend::GetTransferSnapshot() const
{
    const_cast<GSOpenGLHybridBackend *>(this)->EnsureCpuCurrent();
    return m_cpu.GetTransferSnapshot();
}

bool GSOpenGLHybridBackend::EnsureGpuInitialized(std::string &detail)
{
    const auto uploadStart = std::chrono::steady_clock::now();
    if (!m_gpuInitialized)
    {
        if (!GSOpenGLT8Initialize(m_vram, m_vramSize, detail))
            return false;
        m_gpuInitialized = true;
        m_gpuCopyValid = true;
        {
            std::lock_guard lock(m_queueMutex);
            m_cpuDirtyBlocks.reset();
        }
        m_gpuUploadBytes += m_vramSize;
        m_gpuUploadNanoseconds += static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(
                std::chrono::steady_clock::now() - uploadStart).count());
        return true;
    }
    if (!m_gpuCopyValid)
    {
        {
            std::lock_guard lock(m_queueMutex);
            if (m_gpuDirtyBlocks.any())
            {
                detail = "full GS upload would overwrite GPU-dirty blocks";
                return false;
            }
        }
        if (!GSOpenGLT8Upload(m_vram, m_vramSize, detail))
            return false;
        m_gpuCopyValid = true;
        {
            std::lock_guard lock(m_queueMutex);
            m_cpuDirtyBlocks.reset();
        }
        m_gpuUploadBytes += m_vramSize;
        m_gpuUploadNanoseconds += static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(
                std::chrono::steady_clock::now() - uploadStart).count());
    }
    return m_rangeCoherenceEnabled ? UploadCpuDirty(detail) : true;
}

bool GSOpenGLHybridBackend::UploadCpuDirty(std::string &detail)
{
    GSOpenGLCoherence::BlockMask dirty;
    {
        std::lock_guard lock(m_queueMutex);
        if ((m_cpuDirtyBlocks & m_gpuDirtyBlocks).any())
        {
            detail = "CPU/GPU dirty GS block sets overlap";
            return false;
        }
        dirty = m_cpuDirtyBlocks;
    }
    if (dirty.none())
        return true;

    const auto uploadStart = std::chrono::steady_clock::now();
    bool uploaded = true;
    uint64_t uploadedBytes = 0u;
    GSOpenGLCoherence::ForEachRun(dirty, [&](uint32_t offsetBytes, uint32_t sizeBytes) {
        if (uploaded)
        {
            uploaded = GSOpenGLT8UploadRange(m_vram, m_vramSize, offsetBytes,
                                              sizeBytes, detail);
            if (uploaded)
                uploadedBytes += sizeBytes;
        }
    });
    if (!uploaded)
        return false;

    {
        std::lock_guard lock(m_queueMutex);
        m_cpuDirtyBlocks &= ~dirty;
    }
    m_gpuUploadBytes += uploadedBytes;
    m_gpuUploadNanoseconds += static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now() - uploadStart).count());
    return true;
}

bool GSOpenGLHybridBackend::ReadbackGpuDirty(std::string &detail)
{
    GSOpenGLCoherence::BlockMask dirty;
    {
        std::lock_guard lock(m_queueMutex);
        dirty = m_gpuDirtyBlocks;
    }
    if (dirty.none())
        return true;

    bool downloaded = true;
    uint64_t downloadedBytes = 0u;
    GSOpenGLCoherence::ForEachRun(dirty, [&](uint32_t offsetBytes, uint32_t sizeBytes) {
        if (downloaded)
        {
            downloaded = GSOpenGLT8ReadbackRange(m_vram, m_vramSize, offsetBytes,
                                                  sizeBytes, detail);
            if (downloaded)
                downloadedBytes += sizeBytes;
        }
    });
    if (!downloaded)
        return false;

    {
        std::lock_guard lock(m_queueMutex);
        m_gpuDirtyBlocks &= ~dirty;
        m_gpuDirty = m_gpuDirtyBlocks.any();
    }
    m_gpuReadbackBytes += downloadedBytes;
    return true;
}

void GSOpenGLHybridBackend::ProcessCommand(Command &command)
{
    if (command.kind == Command::Kind::Draw)
    {
        ProcessDrawBatch(std::vector<GSPrimitiveBatch>{command.batch});
        return;
    }

    if (command.kind == Command::Kind::Fence)
    {
        if (command.completion)
        {
            std::lock_guard lock(m_queueMutex);
            command.completion->done = true;
            m_queueChanged.notify_all();
        }
        return;
    }

    if (traceOpenGlSynchronization())
        std::cerr << "[gs-opengl-sync] readback begin dirty=" << m_gpuDirty
                  << " initialized=" << m_gpuInitialized << std::endl;
    if (m_gpuDirty && m_gpuInitialized)
    {
        const auto readbackStart = std::chrono::steady_clock::now();
        std::string detail;
        const bool readback = m_rangeCoherenceEnabled
            ? ReadbackGpuDirty(detail)
            : GSOpenGLT8Readback(m_vram, m_vramSize, detail);
        if (!readback)
        {
            m_gpuEnabled = false;
            std::cerr << "[gs-opengl-live] readback failure: " << detail << std::endl;
        }
        ++m_readbacks;
        if (!m_rangeCoherenceEnabled)
        {
            m_gpuReadbackBytes += m_vramSize;
            std::lock_guard lock(m_queueMutex);
            m_gpuDirtyBlocks.reset();
            m_gpuDirty = false;
        }
        m_gpuReadbackNanoseconds += static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(
                std::chrono::steady_clock::now() - readbackStart).count());
        m_gpuCopyValid = true;
    }
    if (command.completion)
    {
        std::lock_guard lock(m_queueMutex);
        command.completion->done = true;
        m_queueChanged.notify_all();
    }
    if (traceOpenGlSynchronization())
        std::cerr << "[gs-opengl-sync] readback end completion="
                  << static_cast<bool>(command.completion) << std::endl;
}

void GSOpenGLHybridBackend::ProcessDrawBatch(const std::vector<GSPrimitiveBatch> &batches)
{
    std::string detail;
    GSOpenGLCoherence::BlockMask writes;
    bool rangesValid = true;
    for (const GSPrimitiveBatch &batch : batches)
    {
        const auto usage = GSOpenGLCoherence::DescribeDraw(batch);
        if (!usage.exact)
        {
            rangesValid = false;
            detail = "draw has a wrapping or unsupported GS coherence range";
            break;
        }
        writes |= usage.writes;
    }
    if (traceOpenGlSynchronization())
        std::cerr << "[gs-opengl-sync] dispatch batch begin draws="
                  << batches.size() << std::endl;
    const auto dispatchStart = std::chrono::steady_clock::now();
    const bool dispatched = (!m_rangeCoherenceEnabled || rangesValid) &&
                            m_gpuEnabled && EnsureGpuInitialized(detail) &&
                            GSOpenGLT8DispatchBatch(batches.data(), batches.size(), detail);
    const uint64_t dispatchNanoseconds = static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now() - dispatchStart).count());
    if (dispatched)
    {
        std::lock_guard lock(m_queueMutex);
        if (m_rangeCoherenceEnabled)
        {
            m_gpuDirtyBlocks |= writes;
            m_gpuDirty = m_gpuDirtyBlocks.any();
        }
        else
        {
            m_gpuDirty = true;
        }
        const uint64_t previous = m_gpuDraws;
        m_gpuDraws += batches.size();
        ++m_gpuBatches;
        m_gpuDispatchNanoseconds += dispatchNanoseconds;
        static const bool detailedProgress =
            std::getenv("PS2X_GS_OPENGL_PROGRESS_DETAIL") != nullptr;
        if (previous == 0u || previous / 100000u != m_gpuDraws / 100000u ||
            (detailedProgress &&
             (m_gpuBatches & (m_gpuBatches - 1u)) == 0u))
            std::cerr << "[gs-opengl-live] completed_draws=" << m_gpuDraws
                      << " dirty_ranges=" << m_rangeCoherenceEnabled
                      << " batches=" << m_gpuBatches
                      << " dispatch_ms=" << (m_gpuDispatchNanoseconds / 1000000.0)
                      << " upload_ms=" << (m_gpuUploadNanoseconds / 1000000.0)
                      << " readback_ms=" << (m_gpuReadbackNanoseconds / 1000000.0)
                      << " upload_mib=" << (m_gpuUploadBytes / (1024.0 * 1024.0))
                      << " readback_mib=" << (m_gpuReadbackBytes / (1024.0 * 1024.0))
                      << " cpu_fallbacks=" << m_cpuFallbacks
                      << " disjoint_cpu_fallbacks=" << m_disjointCpuFallbacks
                      << " readbacks=" << m_readbacks
                      << std::endl;
        return;
    }

    if (m_gpuDirty && m_gpuInitialized)
    {
        std::string readbackDetail;
        const bool readback = m_rangeCoherenceEnabled
            ? ReadbackGpuDirty(readbackDetail)
            : GSOpenGLT8Readback(m_vram, m_vramSize, readbackDetail);
        if (!readback)
            std::cerr << "[gs-opengl-live] readback failure: " << readbackDetail << std::endl;
        else if (!m_rangeCoherenceEnabled)
            m_gpuReadbackBytes += m_vramSize;
        ++m_readbacks;
    }
    m_gpuEnabled = false;
    m_gpuDirty = false;
    m_gpuCopyValid = false;
    for (const GSPrimitiveBatch &batch : batches)
        m_cpu.Submit(batch);
    m_cpuFallbacks += batches.size();
    std::cerr << "[gs-opengl-live] disabling live kernel: " << detail << std::endl;
}

void GSOpenGLHybridBackend::ProcessHostWork()
{
    if (std::this_thread::get_id() != m_hostThread)
        return;
    for (;;)
    {
        Command command;
        {
            std::lock_guard lock(m_queueMutex);
            if (m_queue.empty())
                break;
            command = std::move(m_queue.front());
            m_queue.pop_front();
            ++m_commandsInFlight;
        }
        if (command.kind == Command::Kind::Draw)
        {
            std::vector<GSPrimitiveBatch> batches;
            batches.reserve(256u);
            batches.push_back(command.batch);
            DrawBounds unionBounds = boundsFor(command.batch);
            uint64_t summedArea = unionBounds.area();
            {
                std::lock_guard lock(m_queueMutex);
                while (batches.size() < 256u && !m_queue.empty() &&
                       m_queue.front().kind == Command::Kind::Draw &&
                       std::memcmp(&batches.front().state, &m_queue.front().batch.state,
                                   sizeof(GSDrawState)) == 0)
                {
                    const DrawBounds nextBounds = boundsFor(m_queue.front().batch);
                    const DrawBounds combined{
                        std::min(unionBounds.minX, nextBounds.minX),
                        std::max(unionBounds.maxX, nextBounds.maxX),
                        std::min(unionBounds.minY, nextBounds.minY),
                        std::max(unionBounds.maxY, nextBounds.maxY)};
                    const uint64_t nextSummedArea = summedArea + nextBounds.area();
                    const uint64_t batchedWork = combined.area() * (batches.size() + 1u);
                    if (batchedWork > nextSummedArea * 2u + 4096u)
                        break;
                    batches.push_back(m_queue.front().batch);
                    m_queue.pop_front();
                    ++m_commandsInFlight;
                    unionBounds = combined;
                    summedArea = nextSummedArea;
                }
            }
            ProcessDrawBatch(batches);
            {
                std::lock_guard lock(m_queueMutex);
                m_commandsInFlight -= batches.size();
                m_queueChanged.notify_all();
            }
            if (traceOpenGlSynchronization())
                std::cerr << "[gs-opengl-sync] dispatch batch end draws="
                          << batches.size() << std::endl;
            continue;
        }
        if (traceOpenGlSynchronization())
            std::cerr << "[gs-opengl-sync] host servicing readback" << std::endl;
        ProcessCommand(command);
        {
            std::lock_guard lock(m_queueMutex);
            --m_commandsInFlight;
            m_queueChanged.notify_all();
        }
    }
}

void GSOpenGLHybridBackend::WaitForGpuQueue()
{
    std::shared_ptr<Completion> completion;
    {
        std::lock_guard lock(m_queueMutex);
        if (m_queue.empty() && m_commandsInFlight == 0u)
            return;
        if (std::this_thread::get_id() != m_hostThread)
        {
            completion = std::make_shared<Completion>();
            m_queue.push_back(Command{Command::Kind::Fence, {}, completion});
        }
    }

    if (std::this_thread::get_id() == m_hostThread)
    {
        ProcessHostWork();
        return;
    }

    std::unique_lock lock(m_queueMutex);
    m_queueChanged.wait(lock, [&] { return completion->done || m_shuttingDown; });
}

bool GSOpenGLHybridBackend::TrySubmitCpuDisjoint(const GSPrimitiveBatch &batch)
{
    const auto usage = GSOpenGLCoherence::DescribeDraw(batch);
    if (!usage.exact)
        return false;

    WaitForGpuQueue();
    bool avoidsReadback = false;
    {
        std::lock_guard lock(m_queueMutex);
        if ((usage.access & m_gpuDirtyBlocks).any())
            return false;
        avoidsReadback = m_gpuDirtyBlocks.any();
    }

    m_cpu.Submit(batch);
    {
        std::lock_guard lock(m_queueMutex);
        m_cpuDirtyBlocks |= usage.writes;
    }
    ++m_cpuFallbacks;
    if (avoidsReadback)
        ++m_disjointCpuFallbacks;
    return true;
}

void GSOpenGLHybridBackend::EnsureCpuCurrent()
{
    std::shared_ptr<Completion> completion;
    {
        std::lock_guard lock(m_queueMutex);
        if (m_queue.empty() && m_commandsInFlight == 0u && !m_gpuDirty)
            return;
        if (std::this_thread::get_id() != m_hostThread)
        {
            completion = std::make_shared<Completion>();
            m_queue.push_back(Command{Command::Kind::Readback, {}, completion});
            if (traceOpenGlSynchronization())
                std::cerr << "[gs-opengl-sync] guest queued readback queue="
                          << m_queue.size() << " dirty=" << m_gpuDirty << std::endl;
        }
    }

    if (std::this_thread::get_id() == m_hostThread)
    {
        ProcessHostWork();
        if (m_gpuDirty)
        {
            Command command{Command::Kind::Readback, {}, {}};
            ProcessCommand(command);
        }
        return;
    }

    if (traceOpenGlSynchronization())
        std::cerr << "[gs-opengl-sync] guest waiting readback" << std::endl;
    std::unique_lock lock(m_queueMutex);
    m_queueChanged.wait(lock, [&] { return completion->done || m_shuttingDown; });
    if (traceOpenGlSynchronization())
        std::cerr << "[gs-opengl-sync] guest completed readback" << std::endl;
}

void GSOpenGLHybridBackend::InvalidateGpuCopy()
{
    std::lock_guard lock(m_queueMutex);
    m_gpuCopyValid = false;
    m_gpuDirty = false;
    m_gpuDirtyBlocks.reset();
    m_cpuDirtyBlocks.reset();
}

void GSOpenGLHybridBackend::ShutdownHostResources()
{
    if (std::this_thread::get_id() != m_hostThread)
        return;
    EnsureCpuCurrent();
    if (m_gpuInitialized)
        GSOpenGLT8Shutdown();
    m_gpuInitialized = false;
    m_gpuCopyValid = false;
    m_gpuDirtyBlocks.reset();
    m_cpuDirtyBlocks.reset();
    {
        std::lock_guard lock(m_queueMutex);
        m_shuttingDown = true;
        m_queueChanged.notify_all();
    }
    std::cerr << "[gs-opengl-live] gpu_draws=" << m_gpuDraws
              << " dirty_ranges=" << m_rangeCoherenceEnabled
              << " gpu_batches=" << m_gpuBatches
              << " dispatch_ms=" << (m_gpuDispatchNanoseconds / 1000000.0)
              << " upload_ms=" << (m_gpuUploadNanoseconds / 1000000.0)
              << " readback_ms=" << (m_gpuReadbackNanoseconds / 1000000.0)
              << " upload_mib=" << (m_gpuUploadBytes / (1024.0 * 1024.0))
              << " readback_mib=" << (m_gpuReadbackBytes / (1024.0 * 1024.0))
              << " cpu_fallbacks=" << m_cpuFallbacks
              << " disjoint_cpu_fallbacks=" << m_disjointCpuFallbacks
              << " readbacks=" << m_readbacks << std::endl;
}
