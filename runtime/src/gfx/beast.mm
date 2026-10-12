// The Shadow stalker's Shadow Beast (mods/shadow_beast.h), drawn by us rather than by the game: its skinned mesh with
// its own small shader, into the 3D scene's pass as that pass ends (end_encoder), depth-tested against the scene with
// the game's camera, viewport and clip-space convention, so the world hides it and it hides the world.
#import <Metal/Metal.h>

#include <cstring>
#include <vector>

#include "metal.h"
#include "runtime.h"
#include "mods/shadow_beast.h"

namespace gfx {

MTLViewport g_scene_vp{};
bool g_scene_half_z = false;

namespace {

const char* kShader = R"(
#include <metal_stdlib>
using namespace metal;
struct Vert { packed_float3 p; packed_float3 n; packed_float2 uv; uchar4 j; packed_float4 w; };
struct U { float4x4 clip; float4 world[3]; float4 misc; };  // misc: fade, half z
struct Out { float4 pos [[position]]; float3 n; float2 uv; };
vertex Out vs(uint id [[vertex_id]], device const Vert* v [[buffer(0)]], constant U& u [[buffer(1)]],
              constant float4* joints [[buffer(2)]]) {
    Vert x = v[id];
    float4 p = float4(float3(x.p), 1), n = float4(float3(x.n), 0);
    float3 sp = 0, sn = 0;
    for (int k = 0; k < 4; k++) {
        float w = x.w[k];
        if (w <= 0) continue;
        int j = x.j[k] * 3;
        sp += w * float3(dot(joints[j], p), dot(joints[j + 1], p), dot(joints[j + 2], p));
        sn += w * float3(dot(joints[j], n), dot(joints[j + 1], n), dot(joints[j + 2], n));
    }
    Out o;
    o.pos = u.clip * float4(sp, 1);
    if (u.misc.y == 0) o.pos.z = (o.pos.z + o.pos.w) * 0.5;  // the decompiled shaders' GL clip space (SET_POSITION)
    float4 wn = float4(sn, 0);
    o.n = normalize(float3(dot(u.world[0], wn), dot(u.world[1], wn), dot(u.world[2], wn)));
    o.uv = x.uv;
    return o;
}
fragment float4 fs(Out i [[stage_in]], constant U& u [[buffer(1)]], texture2d<float> t [[texture(0)]],
                   constant uint& glow [[buffer(2)]]) {
    // fading in and out: a dither, so it stays an opaque draw
    const float bayer[16] = {0, 8, 2, 10, 12, 4, 14, 6, 3, 11, 1, 9, 15, 7, 13, 5};
    uint2 c = uint2(i.pos.xy) & 3;
    if ((bayer[c.y * 4 + c.x] + 0.5) / 16 > u.misc.x) discard_fragment();
    constexpr sampler s(filter::linear, address::repeat, mip_filter::none);
    float3 base = t.sample(s, i.uv).rgb;
    float light = 0.45 + 0.55 * saturate(dot(i.n, normalize(float3(0.3, 0.8, 0.5))));
    float3 col = base * light;
    if (glow != 0) col += base * float3(0.6, 1.0, 0.9) * 0.9;  // the twilight lines glow
    return float4(col, 1);
}
)";

struct Gpu {
    id<MTLBuffer> verts = nil, indices = nil;
    std::vector<id<MTLTexture>> textures;
    id<MTLFunction> vs = nil, fs = nil;
    id<MTLDepthStencilState> depth = nil;
    std::vector<std::pair<uint64_t, id<MTLRenderPipelineState>>> pipes;  // by attachment formats
    bool failed = false;
} g_gpu;

std::vector<float> g_pose;   // the frame's OP_BEAST payload
uint64_t g_pose_frame = ~0ull;

bool ready() {
    if (g_gpu.vs || g_gpu.failed) return !g_gpu.failed;
    const auto* a = mods::beast::asset();
    NSError* err = nil;
    id<MTLLibrary> lib = a ? [R.device newLibraryWithSource:[NSString stringWithUTF8String:kShader] options:nil error:&err] : nil;
    if (!lib) {
        if (a) LOG("[beast] shader: %s", err.localizedDescription.UTF8String);
        g_gpu.failed = true;
        return false;
    }
    g_gpu.vs = [lib newFunctionWithName:@"vs"];
    g_gpu.fs = [lib newFunctionWithName:@"fs"];
    g_gpu.verts = [R.device newBufferWithBytes:a->verts.data() length:a->verts.size() options:MTLResourceStorageModeShared];
    g_gpu.indices = [R.device newBufferWithBytes:a->indices.data() length:a->indices.size() * 2 options:MTLResourceStorageModeShared];
    for (const auto& t : a->textures) {
        MTLTextureDescriptor* d = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm
                                                                                     width:t.w height:t.h mipmapped:NO];
        id<MTLTexture> tex = [R.device newTextureWithDescriptor:d];
        [tex replaceRegion:MTLRegionMake2D(0, 0, t.w, t.h) mipmapLevel:0 withBytes:t.rgba.data() bytesPerRow:t.w * 4];
        g_gpu.textures.push_back(tex);
    }
    MTLDepthStencilDescriptor* dd = [MTLDepthStencilDescriptor new];
    dd.depthCompareFunction = MTLCompareFunctionLessEqual;
    dd.depthWriteEnabled = YES;
    g_gpu.depth = [R.device newDepthStencilStateWithDescriptor:dd];
    return true;
}

id<MTLRenderPipelineState> pipeline() {
    uint64_t key = R.passDepth ? (uint64_t)R.passDepth->tex.pixelFormat : 0;
    for (int i = 0; i < 8; i++) key = key * 1000003 + (R.passColor[i] ? (uint64_t)R.passColor[i]->tex.pixelFormat : 0);
    for (auto& [k, p] : g_gpu.pipes)
        if (k == key) return p;
    MTLRenderPipelineDescriptor* d = [MTLRenderPipelineDescriptor new];
    d.vertexFunction = g_gpu.vs;
    d.fragmentFunction = g_gpu.fs;
    for (int i = 0; i < 8; i++) {
        if (!R.passColor[i]) continue;
        d.colorAttachments[i].pixelFormat = R.passColor[i]->tex.pixelFormat;
        d.colorAttachments[i].writeMask = i == 0 ? MTLColorWriteMaskAll : MTLColorWriteMaskNone;
    }
    const MTLPixelFormat df = R.passDepth->tex.pixelFormat;
    d.depthAttachmentPixelFormat = df;
    if (df == MTLPixelFormatDepth32Float_Stencil8 || df == MTLPixelFormatDepth24Unorm_Stencil8) d.stencilAttachmentPixelFormat = df;
    NSError* err = nil;
    id<MTLRenderPipelineState> p = [R.device newRenderPipelineStateWithDescriptor:d error:&err];
    if (!p) LOG("[beast] pipeline: %s", err.localizedDescription.UTF8String);
    g_gpu.pipes.push_back({key, p});
    return p;
}

}  // namespace

// OP_BEAST (render thread, in the guest's command order): this frame's pose
void beast(const uint32_t* p, uint32_t n) {
    g_pose.assign(reinterpret_cast<const float*>(p), reinterpret_cast<const float*>(p) + n);
    g_pose_frame = R.frame;
}

// the open pass is about to end: when it draws the 3D scene (the main depth buffer and a colour target), the beast goes in
void beast_pass() {
    // (the game sends a frame's pose before it swaps the frame before: the swap that starts this one)
    if (g_pose_frame + 1 < R.frame || !R.enc || !R.passDepth || !R.passColor[0] || R.passDepth->addr != R.mainDepthAddr) return;
    const auto* a = mods::beast::asset();
    if (!a || g_pose.size() < mods::beast::kPoseHeader + a->parent.size() * 12 || g_pose[0] <= 0 || !ready()) return;
    id<MTLRenderPipelineState> pipe = pipeline();
    if (!pipe) return;
    const float* f = g_pose.data();
    // clip = projection * view * world (row-major 4x4 from the payload's 3x4s), as a column-major float4x4
    float W[16] = {}, V[16] = {}, P[16], VW[16], C[16];
    memcpy(W, f + 1, 48), W[15] = 1;
    memcpy(V, f + 13, 48), V[15] = 1;
    memcpy(P, f + 25, 64);
    auto mul = [](const float* a, const float* b, float* c) {
        for (int r = 0; r < 4; r++)
            for (int k = 0; k < 4; k++) c[r * 4 + k] = a[r * 4] * b[k] + a[r * 4 + 1] * b[4 + k] + a[r * 4 + 2] * b[8 + k] + a[r * 4 + 3] * b[12 + k];
    };
    mul(V, W, VW);
    mul(P, VW, C);
    struct { float clip[16]; float world[12]; float misc[4]; } u;
    for (int r = 0; r < 4; r++)
        for (int k = 0; k < 4; k++) u.clip[k * 4 + r] = C[r * 4 + k];
    memcpy(u.world, W, 48);
    u.misc[0] = f[0], u.misc[1] = g_scene_half_z ? 1.f : 0.f, u.misc[2] = u.misc[3] = 0;
    id<MTLRenderCommandEncoder> e = R.enc;
    [e setRenderPipelineState:pipe];
    [e setDepthStencilState:g_gpu.depth];
    [e setViewport:g_scene_vp];
    [e setScissorRect:MTLScissorRect{0, 0, R.passColor[0]->tex.width, R.passColor[0]->tex.height}];
    [e setCullMode:MTLCullModeNone];
    [e setDepthBias:0 slopeScale:0 clamp:0];
    [e setDepthClipMode:MTLDepthClipModeClip];
    [e setVertexBuffer:g_gpu.verts offset:0 atIndex:0];
    [e setVertexBytes:&u length:sizeof u atIndex:1];
    [e setVertexBytes:f + mods::beast::kPoseHeader length:a->parent.size() * 48 atIndex:2];
    [e setFragmentBytes:&u length:sizeof u atIndex:1];
    for (const auto& grp : a->groups) {
        if (grp.texture >= g_gpu.textures.size()) continue;
        const uint32_t glow = grp.flags & 1;
        [e setFragmentTexture:g_gpu.textures[grp.texture] atIndex:0];
        [e setFragmentBytes:&glow length:4 atIndex:2];
        [e drawIndexedPrimitives:MTLPrimitiveTypeTriangle indexCount:grp.count indexType:MTLIndexTypeUInt16
                     indexBuffer:g_gpu.indices indexBufferOffset:grp.first * 2];
    }
}

}  // namespace gfx
