#pragma once

#include <cstdint>
#include <cstddef>
#include <string>

struct GSPrimitiveBatch;

struct GSOpenGLComputeProbeResult
{
    bool available{false};
    bool passed{false};
    std::string detail;
};

// Must be called on the host thread after the OpenGL context is created.
GSOpenGLComputeProbeResult ProbeGSOpenGLCompute();

// Runs exact CPU/GPU address-map comparisons and a completed-dispatch
// throughput measurement. Must be called with an OpenGL 4.3 context current.
bool ProbeGSOpenGLAddressMapping(std::string &detail);

// Differentially renders the measured linear-T8/CT32/Z32 triangle class
// against the CPU backend and reports completed GPU pixel throughput.
bool ProbeGSOpenGLT8Triangle(std::string &detail);

// Replays one locally captured real-game T8 draw through the parameterized
// compute path. Capture files are diagnostic evidence and never shipped.
bool ProbeGSOpenGLCapturedT8(const char *prefix, std::string &detail);

// Narrow, byte-exact live kernel currently proven for the game's measured
// linear T8/STQ/MODULATE/CT32/Z24 triangle class. All functions below must be
// called on the thread that owns the current OpenGL context.
bool GSOpenGLT8CanDispatch(const GSPrimitiveBatch &batch);
bool GSOpenGLT8Initialize(const uint8_t *vram, uint32_t vramSize, std::string &detail);
bool GSOpenGLT8Upload(const uint8_t *vram, uint32_t vramSize, std::string &detail);
bool GSOpenGLT8UploadRange(const uint8_t *vram, uint32_t vramSize,
                          uint32_t offsetBytes, uint32_t sizeBytes, std::string &detail);
bool GSOpenGLT8Dispatch(const GSPrimitiveBatch &batch, std::string &detail);
bool GSOpenGLT8DispatchBatch(const GSPrimitiveBatch *batches, size_t count, std::string &detail);
bool GSOpenGLT8Readback(uint8_t *vram, uint32_t vramSize, std::string &detail);
bool GSOpenGLT8ReadbackRange(uint8_t *vram, uint32_t vramSize,
                            uint32_t offsetBytes, uint32_t sizeBytes, std::string &detail);
void GSOpenGLT8Shutdown();
