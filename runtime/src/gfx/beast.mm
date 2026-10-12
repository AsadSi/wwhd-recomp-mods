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
struct U { float4x4 clip; float4 world[3]; float4 misc, ambient, light, light_pos, fog, eye; };  // misc: fade, half z; fog: rgb, start; eye: xyz, fog end
struct Out { float4 pos [[position]]; float3 n; float2 uv; float3 wp; };
float4 to_clip(constant U& u, float3 p) {
    float4 o = u.clip * float4(p, 1);
    if (u.misc.y == 0) o.z = (o.z + o.w) * 0.5;  // the decompiled shaders' GL clip space (SET_POSITION)
    return o;
}
bool faded(float2 xy, float fade) {  // fading out: a dither, so it stays an opaque draw
    const float bayer[16] = {0, 8, 2, 10, 12, 4, 14, 6, 3, 11, 1, 9, 15, 7, 13, 5};
    uint2 c = uint2(xy) & 3;
    return (bayer[c.y * 4 + c.x] + 0.5) / 16 > fade;
}
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
    o.pos = to_clip(u, float3(dot(u.world[0], float4(sp, 1)), dot(u.world[1], float4(sp, 1)), dot(u.world[2], float4(sp, 1))));
    float4 wn = float4(sn, 0);
    o.n = normalize(float3(dot(u.world[0], wn), dot(u.world[1], wn), dot(u.world[2], wn)));
    o.uv = x.uv;
    o.wp = float3(dot(u.world[0], float4(sp, 1)), dot(u.world[1], float4(sp, 1)), dot(u.world[2], float4(sp, 1)));
    return o;
}
// the game's fog for an actor (dKy_tevstr_c's colour and range), by distance from the eye
float3 fogged(constant U& u, float3 c, float3 wp) {
    float k = saturate((distance(wp, u.eye.xyz) - u.fog.w) / max(u.eye.w - u.fog.w, 1.0));
    return mix(c, u.fog.rgb, k);
}
// its two materials, as s2.bmd's TEV has them: s2_mat_body the texture times the lit vertex colour, lit as the game
// lights its own actors there (from the shadow colour C0 to the light colour K0 as it faces the light); s2_mat_mark unlit, TEV
// colour C2 (255, 0, 30) times the texture, cut out below alpha 128
fragment float4 fs(Out i [[stage_in]], constant U& u [[buffer(1)]], texture2d<float> t [[texture(0)]],
                   constant uint& mark [[buffer(2)]]) {
    if (faded(i.pos.xy, u.misc.x)) discard_fragment();
    constexpr sampler s(filter::linear, mip_filter::linear, address::repeat, max_anisotropy(8));
    float4 tex = t.sample(s, i.uv);
    if (mark != 0) {
        if (tex.a < 0.5) discard_fragment();
        return float4(fogged(u, float3(1, 0, 30.0 / 255) * tex.rgb, i.wp), 1);
    }
    float3 light = mix(u.ambient.rgb, u.light.rgb, saturate(dot(i.n, normalize(u.light_pos.xyz - i.wp))));
    return float4(fogged(u, tex.rgb * light, i.wp), 1);
}
// its hair (lines of points, built facing the camera) and its shadow (a disc on the ground): world-space triangles
struct Flat { float4 pos [[position]]; float2 uv; float3 wp; };
vertex Flat vs_flat(uint id [[vertex_id]], device const float4* v [[buffer(0)]], constant U& u [[buffer(1)]]) {
    Flat o;
    o.pos = to_clip(u, v[id * 2].xyz);
    o.uv = v[id * 2 + 1].xy;
    o.wp = v[id * 2].xyz;
    return o;
}
fragment float4 fs_hair(Flat i [[stage_in]], constant U& u [[buffer(1)]]) {
    if (faded(i.pos.xy, u.misc.x)) discard_fragment();
    return float4(fogged(u, float3(5, 10, 10) / 255, i.wp), 1);  // daE_S1_Draw's line colour
}
fragment float4 fs_shadow(Flat i [[stage_in]], constant U& u [[buffer(1)]]) {
    float r = length(i.uv);
    return float4(0, 0, 0, 0.5 * u.misc.x * saturate(1 - r * r));
}
)";

struct Gpu {
    id<MTLBuffer> verts = nil, indices = nil;
    std::vector<id<MTLTexture>> textures;
    id<MTLFunction> vs = nil, fs = nil, vs_flat = nil, fs_hair = nil, fs_shadow = nil;
    id<MTLDepthStencilState> depth = nil, depth_test = nil;
    std::vector<std::pair<uint64_t, id<MTLRenderPipelineState>>> pipes;  // by kind and attachment formats
    id<MTLBuffer> flat[3] = {};  // hair and shadow triangles, a ring the GPU may still be reading the last of
    int flat_next = 0;
    bool failed = false;
} g_gpu;
constexpr size_t kFlatFloats = (mods::beast::kHairStrands * (mods::beast::kHairPoints - 1) + 2) * 6 * 8;

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
    g_gpu.vs_flat = [lib newFunctionWithName:@"vs_flat"];
    g_gpu.fs_hair = [lib newFunctionWithName:@"fs_hair"];
    g_gpu.fs_shadow = [lib newFunctionWithName:@"fs_shadow"];
    g_gpu.verts = [R.device newBufferWithBytes:a->verts.data() length:a->verts.size() options:MTLResourceStorageModeShared];
    g_gpu.indices = [R.device newBufferWithBytes:a->indices.data() length:a->indices.size() * 2 options:MTLResourceStorageModeShared];
    for (auto& b : g_gpu.flat) b = [R.device newBufferWithLength:kFlatFloats * 4 options:MTLResourceStorageModeShared];
    // the textures with their mipmaps (the body's is 1024 square), made on a command buffer of their own
    id<MTLCommandBuffer> cb = [R.queue commandBuffer];
    id<MTLBlitCommandEncoder> blit = [cb blitCommandEncoder];
    for (const auto& t : a->textures) {
        MTLTextureDescriptor* d = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm
                                                                                     width:t.w height:t.h mipmapped:YES];
        id<MTLTexture> tex = [R.device newTextureWithDescriptor:d];
        [tex replaceRegion:MTLRegionMake2D(0, 0, t.w, t.h) mipmapLevel:0 withBytes:t.rgba.data() bytesPerRow:t.w * 4];
        [blit generateMipmapsForTexture:tex];
        g_gpu.textures.push_back(tex);
    }
    [blit endEncoding];
    [cb commit];
    MTLDepthStencilDescriptor* dd = [MTLDepthStencilDescriptor new];
    dd.depthCompareFunction = MTLCompareFunctionLessEqual;
    dd.depthWriteEnabled = YES;
    g_gpu.depth = [R.device newDepthStencilStateWithDescriptor:dd];
    dd.depthWriteEnabled = NO;
    g_gpu.depth_test = [R.device newDepthStencilStateWithDescriptor:dd];
    return true;
}

enum Kind { kMesh, kHair, kShadow };
id<MTLRenderPipelineState> pipeline(Kind kind) {
    uint64_t key = (R.passDepth ? (uint64_t)R.passDepth->tex.pixelFormat : 0) * 4 + kind;
    for (int i = 0; i < 8; i++) key = key * 1000003 + (R.passColor[i] ? (uint64_t)R.passColor[i]->tex.pixelFormat : 0);
    for (auto& [k, p] : g_gpu.pipes)
        if (k == key) return p;
    MTLRenderPipelineDescriptor* d = [MTLRenderPipelineDescriptor new];
    d.vertexFunction = kind == kMesh ? g_gpu.vs : g_gpu.vs_flat;
    d.fragmentFunction = kind == kMesh ? g_gpu.fs : kind == kHair ? g_gpu.fs_hair : g_gpu.fs_shadow;
    for (int i = 0; i < 8; i++) {
        if (!R.passColor[i]) continue;
        d.colorAttachments[i].pixelFormat = R.passColor[i]->tex.pixelFormat;
        d.colorAttachments[i].writeMask = i == 0 ? MTLColorWriteMaskAll : MTLColorWriteMaskNone;
    }
    if (kind == kShadow) {
        auto* c = d.colorAttachments[0];
        c.blendingEnabled = YES;
        c.sourceRGBBlendFactor = MTLBlendFactorSourceAlpha, c.destinationRGBBlendFactor = MTLBlendFactorOneMinusSourceAlpha;
        c.sourceAlphaBlendFactor = MTLBlendFactorZero, c.destinationAlphaBlendFactor = MTLBlendFactorOne;
        c.writeMask = MTLColorWriteMaskRed | MTLColorWriteMaskGreen | MTLColorWriteMaskBlue;
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

void row4(const float* a, const float* b, float* c) {  // row-major 4x4: c = a * b
    for (int r = 0; r < 4; r++)
        for (int k = 0; k < 4; k++) c[r * 4 + k] = a[r * 4] * b[k] + a[r * 4 + 1] * b[4 + k] + a[r * 4 + 2] * b[8 + k] + a[r * 4 + 3] * b[12 + k];
}

}  // namespace

// OP_BEAST (render thread, in the guest's command order): this frame's pose
void beast(const uint32_t* p, uint32_t n) {
    g_pose.assign(reinterpret_cast<const float*>(p), reinterpret_cast<const float*>(p) + n);
    g_pose_frame = R.frame;
}

// the open pass is about to end: when it draws the 3D scene (the main depth buffer and a colour target), the beast goes in
void beast_pass() {
    using namespace mods::beast;
    // (the game sends a frame's pose before it swaps the frame before: the swap that starts this one)
    if (g_pose_frame + 1 < R.frame || !R.enc || !R.passDepth || !R.passColor[0] || R.passDepth->addr != R.mainDepthAddr) return;
    const auto* a = asset();
    const size_t joints = a ? a->parent.size() : 0;
    if (!a || g_pose.size() < kPoseHeader + joints * 12 + kHairStrands * kHairPoints * 4 || g_pose[0] <= 0 || !ready()) return;
    id<MTLRenderPipelineState> mesh = pipeline(kMesh), hair = pipeline(kHair), shadow = pipeline(kShadow);
    if (!mesh || !hair || !shadow) return;
    const float* f = g_pose.data();
    float V[16] = {}, P[16], C[16];
    memcpy(V, f + 13, 48), V[15] = 1;
    memcpy(P, f + 25, 64);
    row4(P, V, C);  // clip from world; the model's own world matrix goes in separately (normals)
    struct { float clip[16], world[12], misc[4], ambient[4], light[4], light_pos[4], fog[4], eye[4]; } u{};
    for (int r = 0; r < 4; r++)
        for (int k = 0; k < 4; k++) u.clip[k * 4 + r] = C[r * 4 + k];
    memcpy(u.world, f + 1, 48);
    u.misc[0] = f[0], u.misc[1] = g_scene_half_z ? 1.f : 0.f;
    const float* l = f + kLight;
    const float eye[3] = {-(V[0] * V[3] + V[4] * V[7] + V[8] * V[11]), -(V[1] * V[3] + V[5] * V[7] + V[9] * V[11]),
                          -(V[2] * V[3] + V[6] * V[7] + V[10] * V[11])};
    for (int i = 0; i < 3; i++)
        u.ambient[i] = l[i], u.light[i] = l[3 + i], u.light_pos[i] = l[6 + i], u.fog[i] = l[9 + i], u.eye[i] = eye[i];
    u.fog[3] = l[12], u.eye[3] = l[13];
    id<MTLRenderCommandEncoder> e = R.enc;
    [e setViewport:g_scene_vp];
    [e setScissorRect:MTLScissorRect{0, 0, R.passColor[0]->tex.width, R.passColor[0]->tex.height}];
    [e setCullMode:MTLCullModeNone];
    [e setDepthBias:0 slopeScale:0 clamp:0];
    [e setDepthClipMode:MTLDepthClipModeClip];
    [e setVertexBytes:&u length:sizeof u atIndex:1];
    [e setFragmentBytes:&u length:sizeof u atIndex:1];
    // the body
    [e setRenderPipelineState:mesh];
    [e setDepthStencilState:g_gpu.depth];
    [e setVertexBuffer:g_gpu.verts offset:0 atIndex:0];
    [e setVertexBytes:f + kPoseHeader length:joints * 48 atIndex:2];
    for (const auto& grp : a->groups) {
        if (grp.texture >= g_gpu.textures.size()) continue;
        const uint32_t mark = grp.flags & 1;
        [e setFragmentTexture:g_gpu.textures[grp.texture] atIndex:0];
        [e setFragmentBytes:&mark length:4 atIndex:2];
        [e drawIndexedPrimitives:MTLPrimitiveTypeTriangle indexCount:grp.count indexType:MTLIndexTypeUInt16
                     indexBuffer:g_gpu.indices indexBufferOffset:grp.first * 2];
    }
    // the hair: each point's width across the line, square to the line and to the way the camera sees it; then the
    // shadow: a disc on the ground under it
    id<MTLBuffer> buf = g_gpu.flat[g_gpu.flat_next++ % 3];
    float* o = (float*)buf.contents;
    size_t n = 0;
    auto put = [&](const float* p, float s, float t) { float* w = o + n++ * 8; w[0] = p[0], w[1] = p[1], w[2] = p[2], w[3] = 1, w[4] = s, w[5] = t, w[6] = w[7] = 0; };
    const float* h = f + kPoseHeader + joints * 12;
    for (int s = 0; s < kHairStrands; s++) {
        float side[kHairPoints][2][3];
        for (int j = 0; j < kHairPoints; j++) {
            const float* p = h + (s * kHairPoints + j) * 4;
            const float* q = h + (s * kHairPoints + std::min(j + 1, kHairPoints - 1)) * 4;
            const float* r = h + (s * kHairPoints + std::max(j - 1, 0)) * 4;
            const float d[3] = {q[0] - r[0], q[1] - r[1], q[2] - r[2]}, v[3] = {eye[0] - p[0], eye[1] - p[1], eye[2] - p[2]};
            float x[3] = {d[1] * v[2] - d[2] * v[1], d[2] * v[0] - d[0] * v[2], d[0] * v[1] - d[1] * v[0]};
            const float l = std::sqrt(x[0] * x[0] + x[1] * x[1] + x[2] * x[2]);
            for (int c = 0; c < 3; c++) x[c] = l > 1e-4f ? x[c] / l * p[3] : 0, side[j][0][c] = p[c] - x[c], side[j][1][c] = p[c] + x[c];
        }
        for (int j = 0; j + 1 < kHairPoints; j++) {
            put(side[j][0], 0, 0), put(side[j][1], 0, 0), put(side[j + 1][0], 0, 0);
            put(side[j + 1][0], 0, 0), put(side[j][1], 0, 0), put(side[j + 1][1], 0, 0);
        }
    }
    const size_t hair_verts = n;
    {
        const float cx = f[4], cz = f[12], gy = f[41] + 2, rad = 90;  // under its base matrix, on its body's ground
        const float c[4][3] = {{cx - rad, gy, cz - rad}, {cx + rad, gy, cz - rad}, {cx - rad, gy, cz + rad}, {cx + rad, gy, cz + rad}};
        put(c[0], -1, -1), put(c[1], 1, -1), put(c[2], -1, 1), put(c[2], -1, 1), put(c[1], 1, -1), put(c[3], 1, 1);
    }
    [e setRenderPipelineState:hair];
    [e setVertexBuffer:buf offset:0 atIndex:0];
    [e drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:hair_verts];
    [e setRenderPipelineState:shadow];
    [e setDepthStencilState:g_gpu.depth_test];
    [e setDepthBias:-1 slopeScale:-1 clamp:0];
    [e drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:hair_verts vertexCount:6];
}

}  // namespace gfx
