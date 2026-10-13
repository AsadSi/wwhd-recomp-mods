// Twilight Princess's particle effects for its monsters in WW (game/tp_particles.bin, tools/tp_particles.py): TP's
// JParticle emitters (JPA2, which WW's own particle system cannot read) simulated here at the game's 30 steps a
// second, and drawn by gfx/beast.mm as quads in the 3D scene.
#pragma once
#include <cstdint>
#include <vector>

namespace mods::tpfx {

struct Texture { uint32_t w, h; std::vector<uint8_t> rgba; };
const std::vector<Texture>* textures();  // null when game/tp_particles.bin is missing or bad

// an effect by its TP id, at `pos`, `scale` its JPA global scale; a handle (0: none). Emitters that keep emitting
// (TP's 0x387 body effect) go on until stop(); `mtx` (3x4, world) moves one with a joint, as setGlobalRTMatrix does
uint32_t spawn(uint32_t id, const float pos[3], float scale = 1);
void follow(uint32_t handle, const float mtx[12]);
void stop(uint32_t handle);  // no more particles; those out live on
void step();                 // a logic step
bool any();                  // anything to draw

// the particles as drawn `t` of the way from the last step to this one, kVertexFloats a vertex, 6 vertices a quad
// (triangles), grouped by texture and blend: each group (texture, blend, first vertex, vertex count) in `groups`
constexpr int kVertexFloats = 16;  // pos xyz, colour mode; uv, A0 only, alpha ref; prm rgba; env rgb, unused
void build(float t, const float eye[3], std::vector<float>& verts, std::vector<uint32_t>& groups);

}  // namespace mods::tpfx
