// Twilight Princess's monsters (mods/tp_monsters.h) and their particles (mods/tp_fx.h), drawn by us rather than by the
// game: skinned meshes with their own small shaders, into the 3D scene's pass as that pass ends (end_encoder),
// depth-tested against the scene with the game's camera, viewport and clip-space convention, so the world hides them
// and they hide the world.
#import <Metal/Metal.h>

#include <cstring>
#include <vector>

#include "metal.h"
#include "runtime.h"
#include "mods/tp_fx.h"
#include "mods/tp_monsters.h"

namespace gfx {

MTLViewport g_scene_vp{};
bool g_scene_half_z = false;

namespace {

const char* kShader = R"(
#include <metal_stdlib>
using namespace metal;
struct Vert { packed_float3 p; packed_float3 n; packed_float2 uv; uchar4 j; packed_float4 w; };
// clip from world; the monster's world matrix; misc: fade, half z; the scene's light (ambient, light colour, its
// position), fog (rgb, start) and the eye (xyz, fog end)
struct U { float4x4 clip; float4 world[3]; float4 misc, ambient, light, light_pos, fog, eye; };
// a material's draw: the TEV set-up (mode 0 lit, 1 unlit C2 x tex, 2 C2-to-white x lit), cut out, light scale, and a
// piece given its own world matrix (loose or held) in place of its joint's
struct M { float4 c0, c2, params; float4 piece[3]; };  // params: mode, cut out, light scale, piece matrix given
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
float3 xfc(constant float4* r, float4 p) { return float3(dot(r[0], p), dot(r[1], p), dot(r[2], p)); }
vertex Out vs(uint id [[vertex_id]], device const Vert* v [[buffer(0)]], constant U& u [[buffer(1)]],
              constant float4* joints [[buffer(2)]], constant M& m [[buffer(3)]]) {
    Vert x = v[id];
    float4 p = float4(float3(x.p), 1), n = float4(float3(x.n), 0);
    Out o;
    if (m.params.w > 0) {  // a piece with its own world matrix
        o.wp = xfc(m.piece, p);
        o.n = normalize(xfc(m.piece, n));
    } else {
        float3 sp = 0, sn = 0;
        for (int k = 0; k < 4; k++) {
            float w = x.w[k];
            if (w <= 0) continue;
            int j = x.j[k] * 3;
            sp += w * xfc(joints + j, p);
            sn += w * xfc(joints + j, n);
        }
        o.wp = xfc(u.world, float4(sp, 1));
        o.n = normalize(xfc(u.world, float4(sn, 0)));
    }
    o.pos = to_clip(u, o.wp);
    o.uv = x.uv;
    return o;
}
// the game's fog for an actor (dKy_tevstr_c's colour and range), by distance from the eye
float3 fogged(constant U& u, float3 c, float3 wp) {
    float k = saturate((distance(wp, u.eye.xyz) - u.fog.w) / max(u.eye.w - u.fog.w, 1.0));
    return mix(c, u.fog.rgb, k);
}
// TP's materials (tools/tp_monsters.py reads their TEV): lit as the game lights its own actors there (from the shadow
// colour C0 to the light colour K0 as the surface faces the light), times the stage's scale, plus TEV colour C0;
// the mark unlit (C2 x the texture); armour C2 to white by the texture, then lit
fragment float4 fs(Out i [[stage_in]], constant U& u [[buffer(1)]], texture2d<float> t [[texture(0)]],
                   constant M& m [[buffer(3)]]) {
    if (faded(i.pos.xy, u.misc.x)) discard_fragment();
    constexpr sampler s(filter::linear, mip_filter::linear, address::repeat, max_anisotropy(8));
    float4 tex = t.sample(s, i.uv);
    int mode = int(m.params.x + 0.5);
    if ((m.params.y > 0 || mode == 1) && tex.a < 0.5) discard_fragment();
    if (mode == 1) return float4(fogged(u, m.c2.rgb * tex.rgb, i.wp), 1);
    float3 light = mix(u.ambient.rgb, u.light.rgb, saturate(dot(i.n, normalize(u.light_pos.xyz - i.wp))));
    float3 base = mode == 2 ? mix(m.c2.rgb, float3(1), tex.rgb) : tex.rgb;
    return float4(fogged(u, base * light * m.params.z + m.c0.rgb, i.wp), 1);
}
// hair (lines of points, built facing the camera) and the shadow (a disc on the ground): world-space triangles
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
// TP's particles (mods/tp_fx.h): JPA's colour modes on the texture (BSP1: 0 tex, 1 prm x tex, 2 lerp(prm, 1, tex),
// 3 lerp(env, prm, tex), 4 tex x prm + env, 5 prm), alpha tex.a x A0 (or A0 alone); cut out below the alpha ref when
// not blended
struct FxV { float4 p; float4 uv; float4 prm; float4 env; };
struct Fx { float4 pos [[position]]; float2 uv; float4 prm; float3 env; float mode, a0, ref; float3 wp; };
vertex Fx vs_fx(uint id [[vertex_id]], device const FxV* v [[buffer(0)]], constant U& u [[buffer(1)]]) {
    FxV x = v[id];
    Fx o;
    o.pos = to_clip(u, x.p.xyz);
    o.uv = x.uv.xy, o.prm = x.prm, o.env = x.env.rgb, o.mode = x.p.w, o.a0 = x.uv.z, o.ref = x.uv.w, o.wp = x.p.xyz;
    return o;
}
float4 fx_colour(Fx i, texture2d<float> t) {
    constexpr sampler s(filter::linear, mip_filter::linear, address::clamp_to_edge);
    float4 tex = t.sample(s, i.uv);
    int m = int(i.mode + 0.5);
    float3 c = m == 0 ? tex.rgb : m == 1 ? i.prm.rgb * tex.rgb : m == 2 ? mix(i.prm.rgb, float3(1), tex.rgb)
             : m == 3 ? mix(i.env, i.prm.rgb, tex.rgb) : m == 4 ? tex.rgb * i.prm.rgb + i.env : i.prm.rgb;
    return float4(c, i.a0 > 0.5 ? i.prm.a : tex.a * i.prm.a);
}
fragment float4 fs_fx_blend(Fx i [[stage_in]], constant U& u [[buffer(1)]], texture2d<float> t [[texture(0)]]) {
    float4 c = fx_colour(i, t);
    return float4(fogged(u, c.rgb, i.wp), c.a);
}
fragment float4 fs_fx_cut(Fx i [[stage_in]], constant U& u [[buffer(1)]], texture2d<float> t [[texture(0)]]) {
    float4 c = fx_colour(i, t);
    if (c.a <= i.ref) discard_fragment();
    return float4(fogged(u, c.rgb, i.wp), 1);
}
fragment float4 fs_shadow(Flat i [[stage_in]], constant U& u [[buffer(1)]]) {
    float r = length(i.uv);
    return float4(0, 0, 0, 0.5 * u.misc.x * saturate(1 - r * r));
}
)";

struct Model { id<MTLBuffer> verts = nil, indices = nil; std::vector<id<MTLTexture>> textures; };
struct Gpu {
    Model models[mods::tpm::kKinds];
    std::vector<id<MTLTexture>> fx_textures;
    id<MTLFunction> vs = nil, fs = nil, vs_flat = nil, fs_hair = nil, fs_shadow = nil, vs_fx = nil, fs_fx_blend = nil, fs_fx_cut = nil;
    id<MTLDepthStencilState> depth = nil, depth_test = nil;
    std::vector<std::pair<uint64_t, id<MTLRenderPipelineState>>> pipes;  // by kind and attachment formats
    id<MTLBuffer> flat[3] = {}, fx[3] = {};  // hair and shadow triangles; particles: rings the GPU may still be reading
    size_t flat_size = 0, fx_size = 0;
    int next = 0;
    bool failed = false;
} g_gpu;

std::vector<float> g_pose;   // the frame's OP_BEAST payload
uint64_t g_pose_frame = ~0ull;

id<MTLTexture> make_texture(id<MTLBlitCommandEncoder> blit, uint32_t w, uint32_t h, const uint8_t* rgba) {
    MTLTextureDescriptor* d = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm
                                                                                 width:w height:h mipmapped:YES];
    id<MTLTexture> tex = [R.device newTextureWithDescriptor:d];
    [tex replaceRegion:MTLRegionMake2D(0, 0, w, h) mipmapLevel:0 withBytes:rgba bytesPerRow:w * 4];
    [blit generateMipmapsForTexture:tex];
    return tex;
}

bool ready() {
    if (g_gpu.vs || g_gpu.failed) return !g_gpu.failed;
    NSError* err = nil;
    id<MTLLibrary> lib = [R.device newLibraryWithSource:[NSString stringWithUTF8String:kShader] options:nil error:&err];
    if (!lib) {
        LOG("[beast] shader: %s", err.localizedDescription.UTF8String);
        g_gpu.failed = true;
        return false;
    }
    g_gpu.vs = [lib newFunctionWithName:@"vs"];
    g_gpu.fs = [lib newFunctionWithName:@"fs"];
    g_gpu.vs_flat = [lib newFunctionWithName:@"vs_flat"];
    g_gpu.fs_hair = [lib newFunctionWithName:@"fs_hair"];
    g_gpu.fs_shadow = [lib newFunctionWithName:@"fs_shadow"];
    g_gpu.vs_fx = [lib newFunctionWithName:@"vs_fx"];
    g_gpu.fs_fx_blend = [lib newFunctionWithName:@"fs_fx_blend"];
    g_gpu.fs_fx_cut = [lib newFunctionWithName:@"fs_fx_cut"];
    // the meshes, and the textures with their mipmaps (made on a command buffer of their own)
    id<MTLCommandBuffer> cb = [R.queue commandBuffer];
    id<MTLBlitCommandEncoder> blit = [cb blitCommandEncoder];
    for (int k = 0; k < mods::tpm::kKinds; k++)
        if (const auto* a = mods::tpm::asset((mods::tpm::Kind)k)) {
            Model& m = g_gpu.models[k];
            m.verts = [R.device newBufferWithBytes:a->verts.data() length:a->verts.size() options:MTLResourceStorageModeShared];
            m.indices = [R.device newBufferWithBytes:a->indices.data() length:a->indices.size() * 2 options:MTLResourceStorageModeShared];
            for (const auto& t : a->textures) m.textures.push_back(make_texture(blit, t.w, t.h, t.rgba.data()));
        }
    if (const auto* fx = mods::tpfx::textures())
        for (const auto& t : *fx) g_gpu.fx_textures.push_back(make_texture(blit, t.w, t.h, t.rgba.data()));
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

enum Pipe { kMesh, kHair, kShadow, kFxBlend, kFxCut };
id<MTLRenderPipelineState> pipeline(Pipe kind) {
    uint64_t key = (R.passDepth ? (uint64_t)R.passDepth->tex.pixelFormat : 0) * 8 + kind;
    for (int i = 0; i < 8; i++) key = key * 1000003 + (R.passColor[i] ? (uint64_t)R.passColor[i]->tex.pixelFormat : 0);
    for (auto& [k, p] : g_gpu.pipes)
        if (k == key) return p;
    MTLRenderPipelineDescriptor* d = [MTLRenderPipelineDescriptor new];
    d.vertexFunction = kind == kMesh ? g_gpu.vs : kind >= kFxBlend ? g_gpu.vs_fx : g_gpu.vs_flat;
    d.fragmentFunction = kind == kMesh ? g_gpu.fs : kind == kHair ? g_gpu.fs_hair : kind == kShadow ? g_gpu.fs_shadow
                       : kind == kFxBlend ? g_gpu.fs_fx_blend : g_gpu.fs_fx_cut;
    for (int i = 0; i < 8; i++) {
        if (!R.passColor[i]) continue;
        d.colorAttachments[i].pixelFormat = R.passColor[i]->tex.pixelFormat;
        d.colorAttachments[i].writeMask = i == 0 ? MTLColorWriteMaskAll : MTLColorWriteMaskNone;
    }
    if (kind == kShadow || kind == kFxBlend) {
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

id<MTLBuffer> ring(id<MTLBuffer> __strong* bufs, size_t& size, size_t bytes) {
    if (bytes > size) {
        for (int i = 0; i < 3; i++) bufs[i] = [R.device newBufferWithLength:bytes * 2 options:MTLResourceStorageModeShared];
        size = bytes * 2;
    }
    return bufs[g_gpu.next % 3];
}

}  // namespace

// OP_BEAST (render thread, in the guest's command order): this frame's monsters and particles
void beast(const uint32_t* p, uint32_t n) {
    g_pose.assign(reinterpret_cast<const float*>(p), reinterpret_cast<const float*>(p) + n);
    g_pose_frame = R.frame;
}

// the open pass is about to end: when it draws the 3D scene (the main depth buffer and a colour target), the monsters
// go in
void beast_pass() {
    using namespace mods::tpm;
    // (the game sends a frame's poses before it swaps the frame before: the swap that starts this one)
    if (g_pose_frame + 1 < R.frame || !R.enc || !R.passDepth || !R.passColor[0] || R.passDepth->addr != R.mainDepthAddr) return;
    if (g_pose.size() < 29 || !ready()) return;
    id<MTLRenderPipelineState> mesh = pipeline(kMesh), hair = pipeline(kHair), shadow = pipeline(kShadow);
    id<MTLRenderPipelineState> fx_blend = pipeline(kFxBlend), fx_cut = pipeline(kFxCut);
    if (!mesh || !hair || !shadow || !fx_blend || !fx_cut) return;
    const float* f = g_pose.data();
    const uint32_t* w = reinterpret_cast<const uint32_t*>(f);
    float V[16] = {}, P[16], C[16];
    memcpy(V, f, 48), V[15] = 1;
    memcpy(P, f + 12, 64);
    row4(P, V, C);  // clip from world
    const float eye[3] = {-(V[0] * V[3] + V[4] * V[7] + V[8] * V[11]), -(V[1] * V[3] + V[5] * V[7] + V[9] * V[11]),
                          -(V[2] * V[3] + V[6] * V[7] + V[10] * V[11])};
    struct Uniforms { float clip[16], world[12], misc[4], ambient[4], light[4], light_pos[4], fog[4], eye[4]; } u{};
    for (int r = 0; r < 4; r++)
        for (int k = 0; k < 4; k++) u.clip[k * 4 + r] = C[r * 4 + k];
    u.misc[1] = g_scene_half_z ? 1.f : 0.f;
    for (int i = 0; i < 3; i++) u.eye[i] = eye[i];
    id<MTLRenderCommandEncoder> e = R.enc;
    [e setViewport:g_scene_vp];
    [e setScissorRect:MTLScissorRect{0, 0, R.passColor[0]->tex.width, R.passColor[0]->tex.height}];
    [e setCullMode:MTLCullModeNone];
    [e setDepthClipMode:MTLDepthClipModeClip];
    // the monsters; their hair and shadows gathered as world-space triangles for after
    const uint32_t count = w[28];
    size_t o = 29;
    std::vector<float> flat;
    struct Flats { size_t hair_first, hair_count, shadow_first; Uniforms u; };
    std::vector<Flats> flats;
    auto put = [&](const float* p, float s, float t) { flat.insert(flat.end(), {p[0], p[1], p[2], 1, s, t, 0, 0}); };
    g_gpu.next++;
    for (uint32_t k = 0; k < count && o + kMonsterHeader <= g_pose.size(); k++) {
        const uint32_t kind = w[o], variant = std::min<uint32_t>(w[o + 2], 4), joints = w[o + 30], pieces = w[o + 31], hairs = w[o + 32];
        const size_t size = kMonsterHeader + joints * 12 + pieces * kPieceFloats + hairs * 4;
        const Asset* a = kind < kKinds ? asset((Kind)kind) : nullptr;
        if (o + size > g_pose.size() || !a || joints != a->parent.size() || pieces != a->parts.size()) { o = g_pose.size(); break; }
        const float* m = f + o;
        u.misc[0] = m[1];
        memcpy(u.world, m + 4, 48);
        for (int i = 0; i < 3; i++)
            u.ambient[i] = m[16 + i], u.light[i] = m[19 + i], u.light_pos[i] = m[22 + i], u.fog[i] = m[25 + i];
        u.fog[3] = m[28], u.eye[3] = m[29];
        const float* skin = m + kMonsterHeader;
        const float* piece = skin + joints * 12;
        const float* hp = piece + pieces * kPieceFloats;
        o += size;
        if (m[1] > 0) {
            const Model& md = g_gpu.models[kind];
            [e setRenderPipelineState:mesh];
            [e setDepthStencilState:g_gpu.depth];
            [e setDepthBias:0 slopeScale:0 clamp:0];
            [e setVertexBuffer:md.verts offset:0 atIndex:0];
            [e setVertexBytes:&u length:sizeof u atIndex:1];
            [e setFragmentBytes:&u length:sizeof u atIndex:1];
            [e setVertexBytes:skin length:joints * 48 atIndex:2];
            for (const auto& grp : a->groups) {
                if (grp.texture >= md.textures.size()) continue;
                struct { float c0[4], c2[4], params[4], piece[12]; } mat{};
                const float* pc = grp.part ? piece + (grp.part - 1) * kPieceFloats : nullptr;
                if (pc && pc[0] == 2) continue;  // a piece not there
                for (int c = 0; c < 4; c++)
                    mat.c0[c] = (grp.c0[variant] >> (8 * c) & 255) / 255.f, mat.c2[c] = (grp.c2[variant] >> (8 * c) & 255) / 255.f;
                mat.params[0] = (float)(grp.flags & 3), mat.params[1] = (grp.flags & 4) ? 1.f : 0.f, mat.params[2] = grp.light;
                if (pc && pc[0] == 1) mat.params[3] = 1, memcpy(mat.piece, pc + 1, 48);
                [e setVertexBytes:&mat length:sizeof mat atIndex:3];
                [e setFragmentBytes:&mat length:sizeof mat atIndex:3];
                [e setFragmentTexture:md.textures[grp.texture] atIndex:0];
                [e drawIndexedPrimitives:MTLPrimitiveTypeTriangle indexCount:grp.count indexType:MTLIndexTypeUInt16
                             indexBuffer:md.indices indexBufferOffset:grp.first * 2];
            }
        }
        // its hair: each point's width across the line, square to the line and to the way the camera sees it (16-point
        // strands); its shadow: a disc on the ground under it
        Flats fl{flat.size() / 8, 0, 0, u};
        if (m[1] > 0) {
            constexpr int kPts = 16;
            for (uint32_t s = 0; s < hairs / kPts; s++) {
                float side[kPts][2][3];
                for (int j = 0; j < kPts; j++) {
                    const float* p = hp + (s * kPts + j) * 4;
                    const float* q = hp + (s * kPts + std::min(j + 1, kPts - 1)) * 4;
                    const float* r = hp + (s * kPts + std::max(j - 1, 0)) * 4;
                    const float d[3] = {q[0] - r[0], q[1] - r[1], q[2] - r[2]}, v[3] = {eye[0] - p[0], eye[1] - p[1], eye[2] - p[2]};
                    float x[3] = {d[1] * v[2] - d[2] * v[1], d[2] * v[0] - d[0] * v[2], d[0] * v[1] - d[1] * v[0]};
                    const float l = std::sqrt(x[0] * x[0] + x[1] * x[1] + x[2] * x[2]);
                    for (int c = 0; c < 3; c++) x[c] = l > 1e-4f ? x[c] / l * p[3] : 0, side[j][0][c] = p[c] - x[c], side[j][1][c] = p[c] + x[c];
                }
                for (int j = 0; j + 1 < kPts; j++) {
                    put(side[j][0], 0, 0), put(side[j][1], 0, 0), put(side[j + 1][0], 0, 0);
                    put(side[j + 1][0], 0, 0), put(side[j][1], 0, 0), put(side[j + 1][1], 0, 0);
                }
            }
        }
        fl.hair_count = flat.size() / 8 - fl.hair_first;
        fl.shadow_first = flat.size() / 8;
        {
            const float cx = m[4 + 3], cz = m[4 + 11], gy = m[3] + 2, rad = kind == kDarknut ? 120 : 90;
            const float c[4][3] = {{cx - rad, gy, cz - rad}, {cx + rad, gy, cz - rad}, {cx - rad, gy, cz + rad}, {cx + rad, gy, cz + rad}};
            put(c[0], -1, -1), put(c[1], 1, -1), put(c[2], -1, 1), put(c[2], -1, 1), put(c[1], 1, -1), put(c[3], 1, 1);
        }
        flats.push_back(fl);
    }
    if (!flat.empty()) {
        id<MTLBuffer> buf = ring(g_gpu.flat, g_gpu.flat_size, flat.size() * 4);
        memcpy(buf.contents, flat.data(), flat.size() * 4);
        [e setVertexBuffer:buf offset:0 atIndex:0];
        for (const Flats& fl : flats) {
            [e setVertexBytes:&fl.u length:sizeof fl.u atIndex:1];
            [e setFragmentBytes:&fl.u length:sizeof fl.u atIndex:1];
            if (fl.hair_count) {
                [e setRenderPipelineState:hair];
                [e setDepthStencilState:g_gpu.depth];
                [e setDepthBias:0 slopeScale:0 clamp:0];
                [e drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:fl.hair_first vertexCount:fl.hair_count];
            }
            [e setRenderPipelineState:shadow];
            [e setDepthStencilState:g_gpu.depth_test];
            [e setDepthBias:-1 slopeScale:-1 clamp:0];
            [e drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:fl.shadow_first vertexCount:6];
        }
    }
    // the particles (mods/tp_fx.h's vertices, by texture and blend): blended ones tested against the scene's depth
    // but not writing it, cut-out ones as solid
    if (o + 2 > g_pose.size()) return;
    const size_t nverts = w[o], ngroups = w[o + 1], bytes = nverts * mods::tpfx::kVertexFloats * 4;
    if (!nverts || g_pose.size() < o + 2 + ngroups * 4 + nverts * mods::tpfx::kVertexFloats) return;
    id<MTLBuffer> fb = ring(g_gpu.fx, g_gpu.fx_size, bytes);
    memcpy(fb.contents, f + o + 2 + ngroups * 4, bytes);
    u.misc[0] = 1;
    [e setVertexBytes:&u length:sizeof u atIndex:1];
    [e setFragmentBytes:&u length:sizeof u atIndex:1];
    [e setVertexBuffer:fb offset:0 atIndex:0];
    [e setDepthBias:0 slopeScale:0 clamp:0];
    for (size_t k = 0; k < ngroups; k++) {
        const uint32_t* grp = w + o + 2 + k * 4;  // texture, blend, first vertex, count
        if (grp[0] >= g_gpu.fx_textures.size()) continue;
        [e setRenderPipelineState:grp[1] ? fx_blend : fx_cut];
        [e setDepthStencilState:grp[1] ? g_gpu.depth_test : g_gpu.depth];
        [e setFragmentTexture:g_gpu.fx_textures[grp[0]] atIndex:0];
        [e drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:grp[2] vertexCount:grp[3]];
    }
}

}  // namespace gfx
