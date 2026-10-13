// Twilight Princess's monsters in WW: their models, textures and animations (game/tp_<monster>.bin, made from the TP HD
// dump by tools/tp_monsters.py), posed here as TP's own actors pose them and drawn by the renderer (gfx/beast.mm) in
// place of a WW actor that is their body. The Shadow stalker's Shadow Beast (shadow_beast.cpp) and the Darknuts
// (darknut.cpp) are built on this.
#pragma once
#include <cstdint>
#include <string>
#include <vector>

struct Cpu;

namespace mods::tpm {

enum Kind { kShadowBeast, kDarknut, kKinds };

struct Asset {
    struct Group {
        uint32_t texture, first, count, flags, part;  // flags: bits 0-1 TEV set-up (0 lit, 1 unlit C2 x tex, 2 C2-to-white
        float light;                                   // x lit), bit 2 cut out, bits 8+ cull; part 0 the body, k piece k-1
        uint32_t c0[5], c2[5];                         // TEV colours C0 and C2 (RGBA8) for each colour variant
    };
    struct Part { char name[32]; int32_t joint; };
    struct Texture { uint32_t w, h; std::vector<uint8_t> rgba; };
    struct Anim { std::string name; uint32_t frames, loop; std::vector<float> keys; };  // frames x joints x 10
    std::vector<int32_t> parent;
    std::vector<float> inv_bind;  // 12 per joint (3x4 row-major)
    std::vector<uint8_t> verts;   // kVertexSize each: pos3 nrm3 uv2 (f32), joint4 (u8), weight4 (f32)
    std::vector<uint16_t> indices;
    std::vector<Group> groups;
    std::vector<Part> parts;
    std::vector<Texture> textures;
    std::vector<Anim> anims;
    int anim(const char* name) const;  // its index (0 when missing)
};
constexpr uint32_t kVertexSize = 52;
const Asset* asset(Kind k);  // null when game/tp_<monster>.bin is missing or bad

// 3x4 row-major matrices
void mul34(const float* a, const float* b, float* c);
void inv34(const float* m, float* out);

// what is played: an animation at a frame, at a rate in frames per logic step; one-shots (strikes, hits, deaths) play
// through once and hold; the one played before blends out over `morf` steps (J3D's morf, what TP's anm_init gives)
struct Animator {
    struct Play { int anim = -1; float frame = 0, rate = 1; bool once = false; };
    const Asset* a = nullptr;
    Play cur, from;
    float blend = 0, morf = 1;
    bool locked = false;  // a one-shot is playing
    void play(const char* name, float rate, bool lock, float morf);
    void advance();
    bool finished() const;  // a one-shot at its end
    bool is(const char* name) const { return a && cur.anim == a->anim(name); }
    // the pose `back` steps before now (0..1): each joint's model-space matrix (`world`) and skinning one (`skin`, or null)
    void pose(float back, float* world, float* skin) const;
};

// one TP monster in the world, drawn each frame while out. Its armour and weapons (the asset's parts) each are on their
// joint, or held elsewhere (a weapon in its hand: `held` the joint, `offset` from it), hidden, or loose (`m`, a world
// matrix, falling about on its own)
struct Monster {
    struct Piece {
        enum Mode : uint8_t { kOn, kHeld, kHidden, kLoose } mode = kOn;
        int held = -1;
        float offset[12] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0};
        float m[12] = {}, pm[12] = {};  // loose: its world matrix now and the last step
    };
    Kind kind = kShadowBeast;
    bool out = false;
    float fade = 1, pfade = 1, fade_to = 1;
    double x = 0, y = 0, z = 0, px = 0, py = 0, pz = 0;  // this step's place and the last's
    float yaw = 0, pyaw = 0, lift = 0, plift = 0;        // lift: above its body (falling from the sky)
    float scale = 1.3f;
    int variant = 0;                                     // its colour variant (TP's brk frame)
    float light[14] = {};                                // the scene's light where it is (read_light)
    Animator anim;
    std::vector<Piece> pieces;
    int hair_strands = 0, hair_points = 0;
    std::vector<float> hair, phair;                      // x, y, z, width a point (the Shadow Beast's)
    void begin(Kind k);                                  // fresh, out, with its asset and pieces
    void step_start();                                   // the last step's state kept for the frames between
    void base(float t, float* m) const;                  // its model matrix, `t` of the way from the last step
    void joint(int j, float* world) const;               // joint j's world matrix now
};

// the monsters drawn: each mod adds its own while they are out
void add(Monster* m);
void remove(Monster* m);
// the light WW gives an actor where it stands (its dKy_tevstr_c at +0x110: settingTevStruct each execute)
void read_light(uint32_t actor, float* light);
// each drawn pass, after camera_draw: every monster's pose, and TP's particles, to the renderer (OP_BEAST)
void camera_drawn(uint32_t cam);
// TP's sounds (game/tp_sounds/<id>.wav, tools/tp_sounds.py): one at a place, for at most `seconds` (0 all)
void sound(uint32_t id, float x, float y, float z, float seconds = 0);

// OP_BEAST payload (floats unless noted): view 3x4, projection 4x4, u32 monster count, then each monster: u32 kind,
// fade, u32 variant, ground y, world 3x4, light[14], u32 joints, u32 pieces, u32 hair points, the joints' skinning
// matrices (3x4 each), each piece (u32 mode: 0 on its joint, 1 world matrix given, 2 hidden; world 3x4), the hair
// points (x, y, z, width); then the particles: u32 vertices, u32 groups, the groups (4 u32), the vertices (tp_fx.h)
constexpr uint32_t kMonsterHeader = 4 + 12 + 14 + 3, kPieceFloats = 13;

}  // namespace mods::tpm
