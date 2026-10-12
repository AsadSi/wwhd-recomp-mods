// Shadow stalker: Twilight Princess's Shadow Beast as the Phantom stalker's hunter (mods/extras.cpp sends it out).
// A Bokoblin is its body: the Bokoblin's own AI chases and strikes Link, takes his hits and dies; its model and weapon
// are not drawn, and the Shadow Beast is drawn in its place (gfx/beast.mm), posed from its own TP animations by what
// the Bokoblin does. The beast comes from the TP HD dump: tools/shadow_beast.py writes <game>/shadow_beast.bin.
#pragma once
#include <cstdint>
#include <string>
#include <vector>

struct Cpu;

namespace mods::beast {

struct Asset {
    struct Group { uint32_t texture, first, count, flags; };  // flags bit 0: the glowing body texture
    struct Texture { uint32_t w, h; std::vector<uint8_t> rgba; };
    struct Anim { std::string name; uint32_t frames, loop; std::vector<float> keys; };  // frames x joints x 10
    std::vector<int32_t> parent;
    std::vector<float> inv_bind;  // 12 per joint (3x4 row-major)
    std::vector<uint8_t> verts;   // kVertexSize each: pos3 nrm3 uv2 (f32), joint4 (u8), weight4 (f32)
    std::vector<uint16_t> indices;
    std::vector<Group> groups;
    std::vector<Texture> textures;
    std::vector<Anim> anims;
};
constexpr uint32_t kVertexSize = 52;
// its hair, as TP's (daE_S1's ke_set): 22 strands of 16 points, drawn as lines of the points' widths
constexpr int kHairStrands = 22, kHairPoints = 16;
// OP_BEAST payload: fade, world 3x4, view 3x4, projection 4x4, the ground's height under it, the scene's light there
// (ambient rgb, light rgb, light position, fog rgb, fog start, fog end); then each joint's skinning matrix 3x4 (model
// space); then the hair's points (world x, y, z, width)
constexpr uint32_t kLight = 42, kPoseHeader = kLight + 14;

const Asset* asset();  // null when game/shadow_beast.bin is missing or bad

// the Bokoblin that is its body (0 none): its draw and its weapon's are skipped (hooks in shadow_beast.cpp)
void set_body(uint32_t body);
// each logic step while the stalker is out: follows its body (0 once it is gone: the beast dies and fades)
void step(Cpu* c, uint32_t link, double now);
// each drawn pass, after camera_draw: this pass's pose to the renderer
void camera_drawn(uint32_t cam);

}  // namespace mods::beast
