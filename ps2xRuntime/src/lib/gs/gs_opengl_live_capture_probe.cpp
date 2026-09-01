#include "runtime/gs/gs_opengl_compute.h"

#include "runtime/gs/gs_types.h"
#include "runtime/gs/gs_cpu_backend.h"
#include "runtime/gs/ps2_gs_common.h"
#include "runtime/gs/ps2_gs_memory.h"
#include "runtime/gs/ps2_gs_psmct32.h"
#include "runtime/gs/ps2_gs_psmt8.h"

#include "rlgl.h"
#include "external/glad.h"

#include <array>
#include <bit>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <algorithm>
#include <cmath>
#include <sstream>
#include <string>
#include <vector>

namespace
{
    constexpr std::array<std::array<uint8_t, 8>, 4> kBlockTableZ32{{
        {{24, 25, 28, 29, 8, 9, 12, 13}},
        {{26, 27, 30, 31, 10, 11, 14, 15}},
        {{16, 17, 20, 21, 0, 1, 4, 5}},
        {{18, 19, 22, 23, 2, 3, 6, 7}},
    }};

    std::array<uint32_t, 416> makeAddressTables()
    {
        std::array<uint32_t, 416> result{};
        uint32_t cursor = 0u;
        for (const auto &row : GSPSMCT32::blockTable32)
            for (uint8_t value : row) result[cursor++] = value;
        for (const auto &row : kBlockTableZ32)
            for (uint8_t value : row) result[cursor++] = value;
        for (const auto &row : GSPSMCT32::columnTable32)
            for (uint8_t value : row) result[cursor++] = value;
        for (const auto &row : GSPSMT8::blockTable8)
            for (uint8_t value : row) result[cursor++] = value;
        for (const auto &row : GSPSMT8::columnTable8)
            for (uint8_t value : row) result[cursor++] = value;
        return result;
    }

    bool readFile(const std::string &path, void *data, size_t size)
    {
        std::ifstream input(path, std::ios::binary | std::ios::ate);
        if (!input || static_cast<size_t>(input.tellg()) != size)
            return false;
        input.seekg(0);
        input.read(static_cast<char *>(data), static_cast<std::streamsize>(size));
        return input.good();
    }

    uint32_t floatBits(float value) { return std::bit_cast<uint32_t>(value); }

    std::array<uint32_t, 64> packParameters(const GSPrimitiveBatch &batch)
    {
        std::array<uint32_t, 64> p{};
        for (uint32_t i = 0u; i < 3u; ++i)
        {
            const GSVertex &v = batch.vertices[i];
            const uint64_t z = std::bit_cast<uint64_t>(v.z);
            const uint32_t base = i * 9u;
            p[base + 0u] = floatBits(v.x);
            p[base + 1u] = floatBits(v.y);
            p[base + 2u] = static_cast<uint32_t>(z);
            p[base + 3u] = static_cast<uint32_t>(z >> 32u);
            p[base + 4u] = v.r | (static_cast<uint32_t>(v.g) << 8u) |
                            (static_cast<uint32_t>(v.b) << 16u) |
                            (static_cast<uint32_t>(v.a) << 24u);
            p[base + 5u] = floatBits(v.q);
            p[base + 6u] = floatBits(v.s);
            p[base + 7u] = floatBits(v.t);
            p[base + 8u] = v.u | (static_cast<uint32_t>(v.v) << 16u);
        }
        const GSDrawState &s = batch.state;
        const GSContext &c = s.context;
        p[32] = GSInternal::framePageBaseToBlock(c.frame.fbp);
        p[33] = c.frame.fbw;
        p[34] = GSInternal::framePageBaseToBlock(c.zbuf.zbp);
        p[35] = c.zbuf.psm;
        p[36] = c.tex0.tbp0;
        p[37] = c.tex0.tbw;
        p[38] = s.textureWidth;
        p[39] = s.textureHeight;
        p[40] = c.tex0.cbp;
        p[41] = s.texclut.cbw != 0u ? s.texclut.cbw : 1u;
        p[42] = s.texclut.cou;
        p[43] = s.texclut.cov;
        p[44] = c.tex0.csa;
        p[45] = static_cast<uint32_t>(c.clamp);
        p[46] = static_cast<uint32_t>(c.clamp >> 32u);
        p[47] = c.scissor.x0 | (static_cast<uint32_t>(c.scissor.x1) << 16u);
        p[48] = c.scissor.y0 | (static_cast<uint32_t>(c.scissor.y1) << 16u);
        p[49] = c.xyoffset.ofx | (static_cast<uint32_t>(c.xyoffset.ofy) << 16u);
        p[50] = static_cast<uint32_t>(s.prim.iip) |
                (static_cast<uint32_t>(s.prim.fst) << 1u) |
                (static_cast<uint32_t>(c.tex0.tcc) << 2u) |
                (static_cast<uint32_t>(c.tex0.tfx) << 3u) |
                (static_cast<uint32_t>(c.fba & 1u) << 5u) |
                (static_cast<uint32_t>(s.linearFilter) << 6u);
        const int ofx = c.xyoffset.ofx >> 4;
        const int ofy = c.xyoffset.ofy >> 4;
        const bool sprite = s.prim.type == GS_PRIM_SPRITE;
        int minX = sprite
                       ? std::min(static_cast<int>(batch.vertices[0].x),
                                  static_cast<int>(batch.vertices[1].x)) - ofx
                       : static_cast<int>(std::floor(std::min({batch.vertices[0].x,
                                                               batch.vertices[1].x,
                                                               batch.vertices[2].x}) -
                                                     static_cast<float>(ofx)));
        int maxX = sprite
                       ? std::max(static_cast<int>(batch.vertices[0].x),
                                  static_cast<int>(batch.vertices[1].x)) - ofx - 1
                       : static_cast<int>(std::ceil(std::max({batch.vertices[0].x,
                                                              batch.vertices[1].x,
                                                              batch.vertices[2].x}) -
                                                    static_cast<float>(ofx)));
        int minY = sprite
                       ? std::min(static_cast<int>(batch.vertices[0].y),
                                  static_cast<int>(batch.vertices[1].y)) - ofy
                       : static_cast<int>(std::floor(std::min({batch.vertices[0].y,
                                                               batch.vertices[1].y,
                                                               batch.vertices[2].y}) -
                                                     static_cast<float>(ofy)));
        int maxY = sprite
                       ? std::max(static_cast<int>(batch.vertices[0].y),
                                  static_cast<int>(batch.vertices[1].y)) - ofy - 1
                       : static_cast<int>(std::ceil(std::max({batch.vertices[0].y,
                                                              batch.vertices[1].y,
                                                              batch.vertices[2].y}) -
                                                    static_cast<float>(ofy)));
        minX = std::clamp(minX, static_cast<int>(c.scissor.x0), static_cast<int>(c.scissor.x1));
        maxX = std::clamp(maxX, static_cast<int>(c.scissor.x0), static_cast<int>(c.scissor.x1));
        minY = std::clamp(minY, static_cast<int>(c.scissor.y0), static_cast<int>(c.scissor.y1));
        maxY = std::clamp(maxY, static_cast<int>(c.scissor.y0), static_cast<int>(c.scissor.y1));
        p[51] = static_cast<uint32_t>(minX) | (static_cast<uint32_t>(maxX) << 16u);
        p[52] = static_cast<uint32_t>(minY) | (static_cast<uint32_t>(maxY) << 16u);
        p[53] = static_cast<uint32_t>(s.prim.type) |
                (static_cast<uint32_t>(s.prim.abe) << 3u) |
                (static_cast<uint32_t>(s.pabe) << 4u) |
                (static_cast<uint32_t>(s.colclamp) << 5u);
        p[54] = static_cast<uint32_t>(c.alpha);
        p[55] = static_cast<uint32_t>(c.alpha >> 32u);
        p[56] = static_cast<uint32_t>(c.test);
        p[57] = static_cast<uint32_t>(c.test >> 32u);
        p[58] = c.tex0.psm;
        return p;
    }

    constexpr const char *kShader = R"glsl(
#version 430
layout(local_size_x = 8, local_size_y = 8) in;
layout(std430, binding = 0) buffer RawVram { uint vram[]; };
layout(std430, binding = 1) readonly buffer AddressTables { uint tables[]; };
layout(std430, binding = 2) readonly buffer DrawParameters { uint p[]; };
uniform uint drawCount;
uniform uvec2 dispatchOrigin;
uint drawBase = 0u;
uint P(uint index) { return p[drawBase + index]; }

uint address32(uint block, uint width, uint x, uint y, bool depth)
{
    uint page = (block >> 5u) + (y >> 5u) * max(width, 1u) + (x >> 6u);
    uint tableBase = depth ? 32u : 0u;
    uint blockId = (block & 31u) + tables[tableBase + (((y >> 3u) & 3u) * 8u) + ((x >> 3u) & 7u)];
    uint column = tables[64u + (y & 7u) * 8u + (x & 7u)];
    return ((page << 13u) + ((blockId >> 5u) << 13u) + (blockId & 31u) * 256u + column * 4u) & 0x003ffffcu;
}
uint addressT8(uint block, uint width, uint x, uint y)
{
    uint page = (block >> 5u) + (y >> 6u) * max(width >> 1u, 1u) + (x >> 7u);
    uint blockId = (block & 31u) + tables[128u + (((y >> 4u) & 3u) * 8u) + ((x >> 4u) & 7u)];
    uint column = tables[160u + (y & 15u) * 16u + (x & 15u)];
    return ((page << 13u) + ((blockId >> 5u) << 13u) + (blockId & 31u) * 256u + column) & 0x003fffffu;
}
uint readByte(uint address) { return (vram[address >> 2u] >> ((address & 3u) * 8u)) & 255u; }
uint channel(uint value, uint shift) { return (value >> shift) & 255u; }
uint resolveClut(uint index)
{
    uint combined = (((P(44u) & 15u) << 4u) + index) & 255u;
    return (combined & ~0x18u) | ((combined & 8u) << 1u) | ((combined & 16u) >> 1u);
}
int wrapCoord(int coordinate, uint size, uint mode, uint regionMin, uint regionMax)
{
    if (mode == 0u) return int(uint(coordinate) & (size - 1u));
    if (mode == 1u) return clamp(coordinate, 0, int(size) - 1);
    if (mode == 2u) return clamp(coordinate, int(regionMin), int(regionMax));
    return int((uint(coordinate) & regionMin) | regionMax);
}
uint samplePoint(int inputU, int inputV)
{
    uint clampLow = P(45u), clampHigh = P(46u);
    int u = wrapCoord(inputU, P(38u), clampLow & 3u,
                      (clampLow >> 4u) & 1023u, (clampLow >> 14u) & 1023u);
    int v = wrapCoord(inputV, P(39u), (clampLow >> 2u) & 3u,
                      ((clampLow >> 24u) | ((clampHigh & 3u) << 8u)) & 1023u,
                      (clampHigh >> 2u) & 1023u);
    if (P(58u) == 0u)
        return vram[address32(P(36u), P(37u), uint(u), uint(v), false) >> 2u];
    uint index = readByte(addressT8(P(36u), P(37u), uint(u), uint(v)));
    uint ci = resolveClut(index);
    uint cx = P(42u) + (ci & 15u), cy = P(43u) + (ci >> 4u);
    return vram[address32(P(40u), P(41u), cx, cy, false) >> 2u];
}
uint lerpChannel(uint c00, uint c10, uint c01, uint c11, float fx, float fy)
{
    precise float top = float(c00) + (float(c10) - float(c00)) * fx;
    precise float bottom = float(c01) + (float(c11) - float(c01)) * fx;
    precise float filtered = top + (bottom - top) * fy;
    return uint(clamp(floor(filtered + 0.5), 0.0, 255.0));
}
uint sampleBilinear(float sampleU, float sampleV)
{
    int u0 = int(floor(sampleU)), v0 = int(floor(sampleV));
    float fx = sampleU - float(u0), fy = sampleV - float(v0);
    uint c00 = samplePoint(u0, v0), c10 = samplePoint(u0 + 1, v0);
    uint c01 = samplePoint(u0, v0 + 1), c11 = samplePoint(u0 + 1, v0 + 1);
    return lerpChannel(channel(c00,0u),channel(c10,0u),channel(c01,0u),channel(c11,0u),fx,fy) |
          (lerpChannel(channel(c00,8u),channel(c10,8u),channel(c01,8u),channel(c11,8u),fx,fy)<<8u) |
          (lerpChannel(channel(c00,16u),channel(c10,16u),channel(c01,16u),channel(c11,16u),fx,fy)<<16u) |
          (lerpChannel(channel(c00,24u),channel(c10,24u),channel(c01,24u),channel(c11,24u),fx,fy)<<24u);
}
uint sampleLinear(float texU, float texV) { return sampleBilinear(texU - 0.5, texV - 0.5); }
float vf(uint vertex, uint field) { return uintBitsToFloat(P(vertex * 9u + field)); }
uint vc(uint vertex, uint shift) { return channel(P(vertex * 9u + 4u), shift); }
double vz(uint vertex) { return packDouble2x32(uvec2(P(vertex*9u+2u),P(vertex*9u+3u))); }
uint clampChannel(int value) { return uint(clamp(value, 0, 255)); }
int blendInput(uint selector, uint source, uint destination)
{
    return selector == 0u ? int(source) : selector == 1u ? int(destination) : 0;
}
void drawSpritePixel(uint x, uint y, vec2 offset)
{
    int x0 = int(vf(0u, 0u)) - int(offset.x);
    int y0 = int(vf(0u, 1u)) - int(offset.y);
    int x1 = int(vf(1u, 0u)) - int(offset.x);
    int y1 = int(vf(1u, 1u)) - int(offset.y);
    if (x0 > x1) { int swapX = x0; x0 = x1; x1 = swapX; }
    if (y0 > y1) { int swapY = y0; y0 = y1; y1 = swapY; }
    int spanX = max(1, x1 - x0), spanY = max(1, y1 - y0);
    precise float tx = (float(int(x) - x0) + 0.5) / float(spanX);
    precise float ty = (float(int(y) - y0) + 0.5) / float(spanY);
    uint uv0 = P(8u), uv1 = P(17u);
    precise float u0 = float((uv0 & 65535u) >> 4u);
    precise float v0 = float((uv0 >> 16u) >> 4u);
    precise float u1 = float((uv1 & 65535u) >> 4u);
    precise float v1 = float((uv1 >> 16u) >> 4u);
    precise float texU = u0 + (u1 - u0) * tx;
    precise float texV = v0 + (v1 - v0) * ty;
    uint texel;
    if (((P(50u) >> 6u) & 1u) != 0u)
    {
        int fixedU = clamp(int(texU * 16.0 + 0.5), 0, 65535);
        int fixedV = clamp(int(texV * 16.0 + 0.5), 0, 65535);
        texel = sampleBilinear(float(fixedU) / 16.0 - 0.5,
                               float(fixedV) / 16.0 - 0.5);
    }
    else
    {
        texel = samplePoint(int(texU), int(texV));
    }

    uint source = 0u;
    for (uint shift = 0u; shift < 32u; shift += 8u)
        source |= min((channel(texel, shift) * vc(1u, shift)) >> 7u, 255u) << shift;

    uint destinationAddress = address32(P(32u), P(33u), x, y, false);
    uint destination = vram[destinationAddress >> 2u];
    uint sourceAlpha = channel(source, 24u);
    bool blend = ((P(53u) >> 3u) & 1u) != 0u;
    bool pabeBypass = ((P(53u) >> 4u) & 1u) != 0u && (sourceAlpha & 128u) == 0u;
    if (blend && !pabeBypass)
    {
        uint alphaMode = P(54u) & 255u;
        uint asel = alphaMode & 3u, bsel = (alphaMode >> 2u) & 3u;
        uint csel = (alphaMode >> 4u) & 3u, dsel = (alphaMode >> 6u) & 3u;
        uint destinationAlpha = channel(destination, 24u);
        int factor = csel == 0u ? int(sourceAlpha) :
                     csel == 1u ? int(destinationAlpha) : int(P(55u) & 255u);
        uint blended = sourceAlpha << 24u;
        for (uint shift = 0u; shift < 24u; shift += 8u)
        {
            uint src = channel(source, shift), dst = channel(destination, shift);
            int value = ((blendInput(asel, src, dst) - blendInput(bsel, src, dst)) * factor >> 7) +
                        blendInput(dsel, src, dst);
            blended |= clampChannel(value) << shift;
        }
        source = blended;
    }
    vram[destinationAddress >> 2u] = source;
}
void drawOne()
{
    uint x = gl_GlobalInvocationID.x + dispatchOrigin.x;
    uint y = gl_GlobalInvocationID.y + dispatchOrigin.y;
    uint bx = P(51u), by = P(52u);
    if (x < (bx & 65535u) || x > (bx >> 16u) ||
        y < (by & 65535u) || y > (by >> 16u)) return;
    uint sx = P(47u), sy = P(48u);
    if (x < (sx & 65535u) || x > (sx >> 16u) || y < (sy & 65535u) || y > (sy >> 16u)) return;
    vec2 offset = vec2(float(P(49u) & 65535u), float(P(49u) >> 16u)) / 16.0;
    if ((P(53u) & 7u) == 6u)
    {
        drawSpritePixel(x, y, offset);
        return;
    }
    vec2 a = vec2(vf(0u,0u),vf(0u,1u))-offset;
    vec2 b = vec2(vf(1u,0u),vf(1u,1u))-offset;
    vec2 c = vec2(vf(2u,0u),vf(2u,1u))-offset;
    vec2 q = vec2(float(x)+0.5,float(y)+0.5);
    precise float denom=(b.y-c.y)*(a.x-c.x)+(c.x-b.x)*(a.y-c.y);
    if (abs(denom)<0.001) return;
    precise float winding=denom<0.0?-1.0:1.0, inv=1.0/abs(denom);
    precise float n0=(b.y-c.y)*(q.x-c.x)+(c.x-b.x)*(q.y-c.y);
    precise float n1=(c.y-a.y)*(q.x-c.x)+(a.x-c.x)*(q.y-c.y);
    precise float w0=(n0*winding)*inv, w1=(n1*winding)*inv, w2=1.0-w0-w1;
    if(w0 < -0.0001 || w1 < -0.0001 || w2 < -0.0001) return;
    precise float is=vf(0u,6u)*w0+vf(1u,6u)*w1+vf(2u,6u)*w2;
    precise float it=vf(0u,7u)*w0+vf(1u,7u)*w1+vf(2u,7u)*w2;
    precise float iq=vf(0u,5u)*w0+vf(1u,5u)*w1+vf(2u,5u)*w2;
    precise float safeQ=abs(iq)>1.0e-8?iq:1.0;
    uint texel=sampleLinear(is/safeQ*float(P(38u)),it/safeQ*float(P(39u)));
    uint rgba=0u;
    for(uint shift=0u;shift<32u;shift+=8u)
    {
        precise float shade=float(vc(0u,shift))*w0+float(vc(1u,shift))*w1+float(vc(2u,shift))*w2;
        uint vertexChannel=uint(clamp(int(shade),0,255));
        uint combined=min((channel(texel,shift)*vertexChannel)>>7u,255u);
        rgba|=combined<<shift;
    }
    if(((P(50u)>>5u)&1u)!=0u) rgba|=0x80000000u;
    precise double d0=vz(0u)*double(w0), d1=vz(1u)*double(w1), d2=vz(2u)*double(w2);
    precise double depth=d0+d1+d2;
    uint writeDepth=uint(depth+double(0.5));
    uint depthAddress=address32(P(34u),P(33u),x,y,true);
    uint rawDepth=vram[depthAddress>>2u], storedDepth=rawDepth&0x00ffffffu;
    if(writeDepth<storedDepth) return;
    vram[address32(P(32u),P(33u),x,y,false)>>2u]=rgba;
    vram[depthAddress>>2u]=(rawDepth&0xff000000u)|(writeDepth&0x00ffffffu);
}
void main()
{
    uint count = max(drawCount, 1u);
    for (uint draw = 0u; draw < count; ++draw)
    {
        drawBase = draw * 64u;
        drawOne();
    }
}
)glsl";

    struct LiveT8State
    {
        unsigned int program = 0u;
        unsigned int vramBuffer = 0u;
        unsigned int tableBuffer = 0u;
        unsigned int parameterBuffer = 0u;
        int drawCountLocation = -1;
        int dispatchOriginLocation = -1;
    };

    LiveT8State g_liveT8;
}

bool GSOpenGLT8CanDispatch(const GSPrimitiveBatch &batch)
{
    const auto &s = batch.state;
    const auto &c = s.context;
    const bool triangle = batch.vertexCount == 3u && s.linearFilter && s.prim.tme && !s.prim.abe &&
           !s.prim.fst && !s.prim.fge && c.tex0.psm == GS_PSM_T8 &&
           c.tex0.cpsm == GS_PSM_CT32 && c.tex0.csm == 0u && c.tex0.tfx == 0u &&
           c.frame.psm == GS_PSM_CT32 && c.zbuf.psm == GS_PSM_Z24 &&
           (c.test & 0x1u) != 0u && ((c.test >> 1u) & 7u) == 7u &&
           ((c.test >> 14u) & 1u) == 0u && ((c.test >> 16u) & 1u) != 0u &&
           ((c.test >> 17u) & 3u) == 2u && c.frame.fbmsk == 0u && !c.zbuf.zmask;
    if (triangle)
        return true;
    static const bool spriteEnabled =
        std::getenv("PS2X_ENABLE_OPENGL_GS_SPRITE") != nullptr;
    if (!spriteEnabled)
        return false;
    const bool sprite = batch.vertexCount >= 2u && s.prim.type == GS_PRIM_SPRITE &&
                         s.prim.tme && s.prim.fst && !s.prim.fge &&
                         s.prim.abe && !s.pabe && s.colclamp != 0u &&
                         c.tex0.psm == GS_PSM_CT32 && c.tex0.tfx == 0u &&
                         c.tex0.tcc != 0u && c.frame.psm == GS_PSM_CT32 &&
                        (c.test & 0x1u) == 0u && ((c.test >> 14u) & 1u) == 0u &&
                        ((c.test >> 17u) & 3u) == 1u && c.frame.fbmsk == 0u &&
                         c.zbuf.zmask && (c.fba & 1u) == 0u &&
                         c.clamp == 0x0000037c003fc00aull &&
                         ((!s.linearFilter && c.alpha == 0x0000001000000068ull) ||
                          (s.linearFilter && c.alpha == 0x0000002000000068ull));
    if (!sprite)
        return false;
    const uint32_t sourceWidth = std::max<uint32_t>(c.tex0.tbw, 1u);
    const uint32_t sourceEnd = c.tex0.tbp0 +
        static_cast<uint32_t>((s.textureHeight + 31u) / 32u) * sourceWidth * 32u - 1u;
    const uint32_t destBase = GSInternal::framePageBaseToBlock(c.frame.fbp);
    const uint32_t destWidth = std::max<uint32_t>(c.frame.fbw, 1u);
    const uint32_t destEnd = destBase +
        (static_cast<uint32_t>(c.scissor.y1 / 32u + 1u) * destWidth * 32u) - 1u;
    constexpr uint32_t blockCount = 16384u;
    return sourceEnd < blockCount && destEnd < blockCount &&
           (sourceEnd < destBase || c.tex0.tbp0 > destEnd);
}

bool GSOpenGLT8Initialize(const uint8_t *vram, uint32_t vramSize, std::string &detail)
{
    if (rlGetVersion() != RL_OPENGL_43)
    {
        detail = "OpenGL 4.3 context is unavailable";
        return false;
    }
    if (!vram || vramSize != GSMem::MEMORY_SIZE)
    {
        detail = "live T8 kernel requires the exact 4 MiB GS memory image";
        return false;
    }
    if (g_liveT8.program != 0u)
        return GSOpenGLT8Upload(vram, vramSize, detail);

    const unsigned int shader = rlCompileShader(kShader, RL_COMPUTE_SHADER);
    g_liveT8.program = shader ? rlLoadComputeShaderProgram(shader) : 0u;
    const auto tables = makeAddressTables();
    std::array<uint32_t, 64u * 256u> emptyParameters{};
    g_liveT8.vramBuffer = rlLoadShaderBuffer(vramSize, vram, RL_DYNAMIC_COPY);
    g_liveT8.tableBuffer = rlLoadShaderBuffer(
        static_cast<unsigned int>(tables.size() * sizeof(uint32_t)), tables.data(), RL_STATIC_DRAW);
    g_liveT8.parameterBuffer = rlLoadShaderBuffer(
        static_cast<unsigned int>(sizeof(emptyParameters)), emptyParameters.data(), RL_DYNAMIC_COPY);
    if (!g_liveT8.program || !g_liveT8.vramBuffer || !g_liveT8.tableBuffer ||
        !g_liveT8.parameterBuffer)
    {
        GSOpenGLT8Shutdown();
        detail = "live T8 shader compilation or SSBO allocation failed";
        return false;
    }
    g_liveT8.drawCountLocation = glGetUniformLocation(g_liveT8.program, "drawCount");
    g_liveT8.dispatchOriginLocation = glGetUniformLocation(g_liveT8.program, "dispatchOrigin");
    detail = "live T8 GPU state initialized";
    return true;
}

bool GSOpenGLT8Upload(const uint8_t *vram, uint32_t vramSize, std::string &detail)
{
    return GSOpenGLT8UploadRange(vram, vramSize, 0u, vramSize, detail);
}

bool GSOpenGLT8UploadRange(const uint8_t *vram, uint32_t vramSize,
                          uint32_t offsetBytes, uint32_t sizeBytes, std::string &detail)
{
    if (!vram || vramSize != GSMem::MEMORY_SIZE)
    {
        detail = "live T8 upload requires the exact 4 MiB GS memory image";
        return false;
    }
    if (sizeBytes == 0u || offsetBytes > vramSize || sizeBytes > vramSize - offsetBytes)
    {
        detail = "live T8 upload range is empty or outside GS memory";
        return false;
    }
    if (!g_liveT8.program)
    {
        if (offsetBytes != 0u || sizeBytes != vramSize)
        {
            detail = "partial live T8 upload requires initialized GPU state";
            return false;
        }
        return GSOpenGLT8Initialize(vram, vramSize, detail);
    }
    rlUpdateShaderBuffer(g_liveT8.vramBuffer, vram + offsetBytes, sizeBytes, offsetBytes);
    detail = "live T8 GS memory range uploaded";
    return true;
}

bool GSOpenGLT8Dispatch(const GSPrimitiveBatch &batch, std::string &detail)
{
    return GSOpenGLT8DispatchBatch(&batch, 1u, detail);
}

bool GSOpenGLT8DispatchBatch(const GSPrimitiveBatch *batches, size_t count, std::string &detail)
{
    if (!g_liveT8.program || !batches || count == 0u || count > 256u)
    {
        detail = "live T8 batch is unavailable or outside the 256-draw limit";
        return false;
    }
    std::vector<uint32_t> parameters(count * 64u);
    uint32_t minX = UINT32_MAX, minY = UINT32_MAX, maxX = 0u, maxY = 0u;
    for (size_t index = 0u; index < count; ++index)
    {
        if (!GSOpenGLT8CanDispatch(batches[index]))
        {
            detail = "draw is outside the proven live T8 kernel state";
            return false;
        }
        const auto packed = packParameters(batches[index]);
        minX = std::min(minX, packed[51] & 0xffffu);
        maxX = std::max(maxX, packed[51] >> 16u);
        minY = std::min(minY, packed[52] & 0xffffu);
        maxY = std::max(maxY, packed[52] >> 16u);
        std::memcpy(parameters.data() + index * 64u, packed.data(), sizeof(packed));
    }
    rlUpdateShaderBuffer(g_liveT8.parameterBuffer, parameters.data(),
                         static_cast<unsigned int>(parameters.size() * sizeof(uint32_t)), 0u);
    rlEnableShader(g_liveT8.program);
    if (g_liveT8.drawCountLocation >= 0)
        glUniform1ui(g_liveT8.drawCountLocation, static_cast<unsigned int>(count));
    if (g_liveT8.dispatchOriginLocation >= 0)
        glUniform2ui(g_liveT8.dispatchOriginLocation, minX, minY);
    rlBindShaderBuffer(g_liveT8.vramBuffer, 0u);
    rlBindShaderBuffer(g_liveT8.tableBuffer, 1u);
    rlBindShaderBuffer(g_liveT8.parameterBuffer, 2u);
    rlComputeShaderDispatch((maxX - minX + 8u) / 8u,
                            (maxY - minY + 8u) / 8u, 1u);
    glMemoryBarrier(GL_SHADER_STORAGE_BARRIER_BIT | GL_BUFFER_UPDATE_BARRIER_BIT);
    rlDisableShader();
    detail.clear();
    return true;
}

bool GSOpenGLT8Readback(uint8_t *vram, uint32_t vramSize, std::string &detail)
{
    return GSOpenGLT8ReadbackRange(vram, vramSize, 0u, vramSize, detail);
}

bool GSOpenGLT8ReadbackRange(uint8_t *vram, uint32_t vramSize,
                            uint32_t offsetBytes, uint32_t sizeBytes, std::string &detail)
{
    if (!g_liveT8.program || !vram || vramSize != GSMem::MEMORY_SIZE)
    {
        detail = "live T8 readback state is unavailable";
        return false;
    }
    if (sizeBytes == 0u || offsetBytes > vramSize || sizeBytes > vramSize - offsetBytes)
    {
        detail = "live T8 readback range is empty or outside GS memory";
        return false;
    }
    glMemoryBarrier(GL_SHADER_STORAGE_BARRIER_BIT | GL_BUFFER_UPDATE_BARRIER_BIT);
    rlReadShaderBuffer(g_liveT8.vramBuffer, vram + offsetBytes, sizeBytes, offsetBytes);
    detail = "live T8 GS memory range read back";
    return true;
}

void GSOpenGLT8Shutdown()
{
    if (g_liveT8.vramBuffer) rlUnloadShaderBuffer(g_liveT8.vramBuffer);
    if (g_liveT8.tableBuffer) rlUnloadShaderBuffer(g_liveT8.tableBuffer);
    if (g_liveT8.parameterBuffer) rlUnloadShaderBuffer(g_liveT8.parameterBuffer);
    if (g_liveT8.program) rlUnloadShaderProgram(g_liveT8.program);
    g_liveT8 = {};
}

bool ProbeGSOpenGLCapturedT8(const char *prefix, std::string &detail)
{
    if (!prefix || prefix[0] == '\0') { detail = "live T8 capture prefix is empty"; return false; }
    std::vector<uint8_t> before(static_cast<size_t>(GSMem::MEMORY_SIZE));
    std::vector<uint8_t> expected(before.size()), actual(before.size());
    GSPrimitiveBatch batch{};
    const std::string base(prefix);
    if (!readFile(base + ".before.bin", before.data(), before.size()) ||
        !readFile(base + ".after.bin", expected.data(), expected.size()) ||
        !readFile(base + ".batch.bin", &batch, sizeof(batch)))
    { detail = "live T8 capture files are missing or have the wrong size"; return false; }
    const auto &s=batch.state; const auto &c=s.context;
    const bool sprite = s.prim.type == GS_PRIM_SPRITE;
    if (!GSOpenGLT8CanDispatch(batch))
    {
        std::ostringstream rejected;
        rejected << "captured draw is outside the parameterized kernel's proven state: vertices="
                 << static_cast<uint32_t>(batch.vertexCount)
                 << " prim=" << static_cast<uint32_t>(s.prim.type)
                 << " tme=" << s.prim.tme << " fst=" << s.prim.fst
                 << " abe=" << s.prim.abe << " fge=" << s.prim.fge
                 << " linear=" << s.linearFilter
                 << " tpsm=0x" << std::hex << static_cast<uint32_t>(c.tex0.psm)
                 << " tfx=" << std::dec << static_cast<uint32_t>(c.tex0.tfx)
                 << " tcc=" << static_cast<uint32_t>(c.tex0.tcc)
                 << " fpsm=0x" << std::hex << static_cast<uint32_t>(c.frame.psm)
                 << " test=0x" << c.test << " fbmsk=0x" << c.frame.fbmsk
                 << " zmask=" << std::dec << c.zbuf.zmask << " fba=" << (c.fba & 1u)
                 << " source=0x" << std::hex << c.tex0.tbp0 << '/' << std::dec
                 << static_cast<uint32_t>(c.tex0.tbw) << " size="
                 << s.textureWidth << 'x' << s.textureHeight << " dest=0x"
                 << std::hex << c.frame.fbp << '/' << std::dec
                 << static_cast<uint32_t>(c.frame.fbw);
        detail = rejected.str();
        return false;
    }
    const unsigned int shader=rlCompileShader(kShader,RL_COMPUTE_SHADER);
    const unsigned int program=shader?rlLoadComputeShaderProgram(shader):0u;
    if(!program){ detail="captured T8 shader compile/link failed"; return false; }
    const auto tables=makeAddressTables(); const auto parameters=packParameters(batch);
    const unsigned int vb=rlLoadShaderBuffer(static_cast<unsigned int>(before.size()),before.data(),RL_DYNAMIC_COPY);
    const unsigned int tb=rlLoadShaderBuffer(static_cast<unsigned int>(tables.size()*4u),tables.data(),RL_STATIC_DRAW);
    const unsigned int pb=rlLoadShaderBuffer(static_cast<unsigned int>(parameters.size()*4u),parameters.data(),RL_STATIC_DRAW);
    auto cleanup=[&]{if(vb)rlUnloadShaderBuffer(vb);if(tb)rlUnloadShaderBuffer(tb);if(pb)rlUnloadShaderBuffer(pb);rlUnloadShaderProgram(program);};
    if(!vb||!tb||!pb){cleanup();detail="captured T8 SSBO allocation failed";return false;}
    rlEnableShader(program);rlBindShaderBuffer(vb,0u);rlBindShaderBuffer(tb,1u);rlBindShaderBuffer(pb,2u);
    const uint32_t gx=(static_cast<uint32_t>(c.scissor.x1)+8u)/8u;
    const uint32_t gy=(static_cast<uint32_t>(c.scissor.y1)+8u)/8u;
    rlComputeShaderDispatch(gx,gy,1u);glMemoryBarrier(GL_SHADER_STORAGE_BARRIER_BIT|GL_BUFFER_UPDATE_BARRIER_BIT);
    rlReadShaderBuffer(vb,actual.data(),static_cast<unsigned int>(actual.size()),0u);rlDisableShader();cleanup();
    size_t mismatches=0u,first=0u,changed=0u;
    for(size_t i=0;i<actual.size();++i){if(before[i]!=expected[i])++changed;if(actual[i]!=expected[i]){if(!mismatches)first=i;++mismatches;}}
    if(mismatches){std::ostringstream m;m<<"captured T8 VRAM mismatch: "<<mismatches<<" bytes, first 0x"<<std::hex<<first<<" CPU=0x"<<static_cast<uint32_t>(expected[first])<<" GPU=0x"<<static_cast<uint32_t>(actual[first]);detail=m.str();return false;}
    std::vector<GSPrimitiveBatch> batchSequence(64u, batch);
    for (size_t index = 0u; index < batchSequence.size(); ++index)
    {
        const float dx = static_cast<float>(index & 7u) * 1.25f;
        const float dy = static_cast<float>(index >> 3u) * 1.25f;
        for (uint32_t vertex = 0u; vertex < 3u; ++vertex)
        {
            batchSequence[index].vertices[vertex].x += dx;
            batchSequence[index].vertices[vertex].y += dy;
        }
    }
    std::vector<uint8_t> batchCpu = before;
    std::vector<uint8_t> batchGpu = before;
    GSCpuBackend cpu;
    cpu.Initialize(batchCpu.data(), static_cast<uint32_t>(batchCpu.size()));
    for (const GSPrimitiveBatch &draw : batchSequence) cpu.Submit(draw);
    cpu.Flush();
    std::string batchDetail;
    const bool batchOk = GSOpenGLT8Initialize(batchGpu.data(), static_cast<uint32_t>(batchGpu.size()), batchDetail) &&
                         GSOpenGLT8DispatchBatch(batchSequence.data(), batchSequence.size(), batchDetail) &&
                         GSOpenGLT8Readback(batchGpu.data(), static_cast<uint32_t>(batchGpu.size()), batchDetail);
    GSOpenGLT8Shutdown();
    if (!batchOk || batchCpu != batchGpu)
    {
        size_t firstBatchMismatch = 0u;
        while (firstBatchMismatch < batchCpu.size() && batchCpu[firstBatchMismatch] == batchGpu[firstBatchMismatch])
            ++firstBatchMismatch;
        std::ostringstream error;
        error << "captured 64-draw batch mismatch at 0x" << std::hex << firstBatchMismatch
              << ": " << batchDetail;
        detail = error.str();
        return false;
    }
    std::ostringstream m;
    if (sprite)
    {
        m << "captured real-game CT32/FST/MODULATE sprite matched all 4 MiB exactly ("
          << changed << " changed bytes); 64-draw ordered batch matched exactly; abe="
          << s.prim.abe << " linear=" << s.linearFilter << " pabe=" << s.pabe
          << " colclamp=" << s.colclamp << " alpha=0x" << std::hex << c.alpha
          << " clamp=0x" << c.clamp;
    }
    else
    {
        m << "captured real-game STQ/MODULATE/Z24 T8 draw matched all 4 MiB exactly ("
          << changed << " changed bytes); 64-draw ordered batch matched exactly";
    }
    detail=m.str();return true;
}
