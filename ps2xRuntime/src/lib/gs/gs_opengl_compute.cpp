#include "runtime/gs/gs_opengl_compute.h"

#include "rlgl.h"
#include "external/glad.h"

#include <array>
#include <cstdint>
#include <cstdlib>
#include <sstream>

GSOpenGLComputeProbeResult ProbeGSOpenGLCompute()
{
    GSOpenGLComputeProbeResult result{};
    if (rlGetVersion() != RL_OPENGL_43)
    {
        result.detail = "OpenGL 4.3 context is unavailable; CPU GS fallback remains active";
        return result;
    }
    result.available = true;

    static constexpr const char *shaderSource = R"glsl(
#version 430
layout(local_size_x = 64) in;
layout(std430, binding = 0) buffer ProbeValues { uint values[]; };
void main()
{
    uint index = gl_GlobalInvocationID.x;
    values[index] = values[index] * 1664525u + 1013904223u;
}
)glsl";

    const unsigned int shader = rlCompileShader(shaderSource, RL_COMPUTE_SHADER);
    if (shader == 0u)
    {
        result.detail = "OpenGL 4.3 compute shader compilation failed";
        return result;
    }
    const unsigned int program = rlLoadComputeShaderProgram(shader);
    if (program == 0u)
    {
        result.detail = "OpenGL 4.3 compute shader link failed";
        return result;
    }

    std::array<uint32_t, 256> values{};
    std::array<uint32_t, 256> expected{};
    for (uint32_t index = 0u; index < values.size(); ++index)
    {
        values[index] = index ^ 0x5A5AA5A5u;
        expected[index] = values[index] * 1664525u + 1013904223u;
    }

    const unsigned int buffer = rlLoadShaderBuffer(
        static_cast<unsigned int>(sizeof(values)), values.data(), RL_DYNAMIC_COPY);
    if (buffer == 0u)
    {
        rlUnloadShaderProgram(program);
        result.detail = "OpenGL 4.3 SSBO allocation failed";
        return result;
    }

    rlEnableShader(program);
    rlBindShaderBuffer(buffer, 0u);
    rlComputeShaderDispatch(4u, 1u, 1u);
    glMemoryBarrier(GL_SHADER_STORAGE_BARRIER_BIT | GL_BUFFER_UPDATE_BARRIER_BIT);
    rlDisableShader();
    rlReadShaderBuffer(buffer, values.data(), static_cast<unsigned int>(sizeof(values)), 0u);
    rlUnloadShaderBuffer(buffer);
    rlUnloadShaderProgram(program);

    for (uint32_t index = 0u; index < values.size(); ++index)
    {
        if (values[index] != expected[index])
        {
            std::ostringstream detail;
            detail << "OpenGL compute/SSBO mismatch at " << index
                   << ": expected 0x" << std::hex << expected[index]
                   << ", got 0x" << values[index];
            result.detail = detail.str();
            return result;
        }
    }

    std::string addressDetail;
    if (!ProbeGSOpenGLAddressMapping(addressDetail))
    {
        result.detail = "OpenGL 4.3 compute shader and SSBO roundtrip passed; " + addressDetail;
        return result;
    }

    std::string triangleDetail;
    if (!ProbeGSOpenGLT8Triangle(triangleDetail))
    {
        result.detail = "OpenGL 4.3 compute shader and SSBO roundtrip passed; " +
                        addressDetail + "; " + triangleDetail;
        return result;
    }

    std::string capturedDetail;
    if (const char *prefix = std::getenv("PS2X_GS_OPENGL_LIVE_CAPTURE_PREFIX"))
    {
        if (!ProbeGSOpenGLCapturedT8(prefix, capturedDetail))
        {
            result.detail = "OpenGL 4.3 compute shader and SSBO roundtrip passed; " +
                            addressDetail + "; " + triangleDetail + "; " + capturedDetail;
            return result;
        }
    }

    result.passed = true;
    result.detail = "OpenGL 4.3 compute shader and SSBO roundtrip passed; " +
                    addressDetail + "; " + triangleDetail;
    if (!capturedDetail.empty())
        result.detail += "; " + capturedDetail;
    return result;
}
