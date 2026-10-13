// Twilight Princess's monsters (tp_monsters.h): their assets, TP's animation playing, and the frame's poses to the
// renderer.
#include "tp_monsters.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <unordered_map>

#include "mods.h"
#include "../gx2/gx2.h"
#include "../gx2/gx2_cmd.h"
#include "../sfx.h"
#include "runtime.h"
#include "tp_fx.h"

namespace interp { float pass_t(); }

namespace mods::tpm {
namespace {

const char* const kFile[kKinds] = {"tp_shadow_beast.bin", "tp_darknut.bin"};
Asset g_assets[kKinds];
int g_state[kKinds];  // 0 not loaded yet, 1 loaded, -1 missing or bad
std::vector<Monster*> g_monsters;

bool load(Kind k) {
    const std::string path = config::game_dir + "/" + kFile[k];
    FILE* f = fopen(path.c_str(), "rb");
    if (!f) { trace("tpm: %s missing (tools/tp_monsters.py makes it from the TP HD dump)", path.c_str()); return false; }
    std::vector<uint8_t> d;
    uint8_t buf[65536];
    for (size_t n; (n = fread(buf, 1, sizeof buf, f)) > 0;) d.insert(d.end(), buf, buf + n);
    fclose(f);
    size_t o = 0;
    bool bad = false;
    auto take = [&](void* out, size_t n) {
        if (o + n > d.size()) { bad = true; memset(out, 0, n); return; }
        memcpy(out, d.data() + o, n), o += n;
    };
    auto u32 = [&] { uint32_t v; take(&v, 4); return v; };
    char magic[4];
    take(magic, 4);
    if (memcmp(magic, "SBST", 4) != 0 || u32() != 2) { trace("tpm: %s is not a v2 monster", path.c_str()); return false; }
    Asset& a = g_assets[k];
    const uint32_t joints = u32();
    a.parent.resize(joints), a.inv_bind.resize(joints * 12);
    for (uint32_t j = 0; j < joints && !bad; j++) take(&a.parent[j], 4), take(&a.inv_bind[j * 12], 48);
    const uint32_t verts = u32();
    a.verts.resize((size_t)verts * kVertexSize);
    take(a.verts.data(), a.verts.size());
    a.indices.resize(u32());
    take(a.indices.data(), a.indices.size() * 2);
    a.groups.resize(u32());
    take(a.groups.data(), a.groups.size() * sizeof(Asset::Group));
    a.parts.resize(u32());
    take(a.parts.data(), a.parts.size() * sizeof(Asset::Part));
    a.textures.resize(u32());
    for (auto& t : a.textures) t.w = u32(), t.h = u32(), t.rgba.resize((size_t)t.w * t.h * 4), take(t.rgba.data(), t.rgba.size());
    a.anims.resize(u32());
    for (auto& an : a.anims) {
        char name[32];
        take(name, 32);
        an.name.assign(name, strnlen(name, 32));
        an.frames = u32(), an.loop = u32();
        an.keys.resize((size_t)an.frames * joints * 10);
        take(an.keys.data(), an.keys.size() * 4);
    }
    if (bad || joints == 0 || a.anims.empty()) { trace("tpm: %s is cut short", path.c_str()); return false; }
    trace("tpm: %s: %u joints, %zu pieces, %u vertices, %zu animations", kFile[k], joints, a.parts.size(), verts, a.anims.size());
    return true;
}

// joint j of play `p` at `frame`: scale, rotation (quaternion), translation
void sample(const Asset& as, const Animator::Play& p, float frame, int j, float s[3], float q[4], float t[3]) {
    const auto& an = as.anims[p.anim];
    const int joints = (int)as.parent.size();
    const bool loop = an.loop && !p.once;
    frame = loop ? std::fmod(frame + an.frames, (float)an.frames) : std::clamp(frame, 0.f, (float)(an.frames - 1));
    const int f0 = (int)frame, f1 = loop ? (f0 + 1) % an.frames : std::min<int>(f0 + 1, an.frames - 1);
    const float k = frame - f0;
    const float* a = &an.keys[((size_t)f0 * joints + j) * 10];
    const float* b = &an.keys[((size_t)f1 * joints + j) * 10];
    const float sign = a[3] * b[3] + a[4] * b[4] + a[5] * b[5] + a[6] * b[6] < 0 ? -1.f : 1.f;
    for (int i = 0; i < 3; i++) s[i] = a[i] + (b[i] - a[i]) * k, t[i] = a[7 + i] + (b[7 + i] - a[7 + i]) * k;
    for (int i = 0; i < 4; i++) q[i] = a[3 + i] + (sign * b[3 + i] - a[3 + i]) * k;
}

}  // namespace

int Asset::anim(const char* name) const {
    for (size_t i = 0; i < anims.size(); i++)
        if (anims[i].name == name) return (int)i;
    return 0;
}

const Asset* asset(Kind k) {
    if (!g_state[k]) g_state[k] = load(k) ? 1 : -1;
    return g_state[k] > 0 ? &g_assets[k] : nullptr;
}

void mul34(const float* a, const float* b, float* c) {
    float o[12];
    for (int r = 0; r < 3; r++)
        for (int k = 0; k < 4; k++)
            o[r * 4 + k] = a[r * 4] * b[k] + a[r * 4 + 1] * b[4 + k] + a[r * 4 + 2] * b[8 + k] + (k == 3 ? a[r * 4 + 3] : 0);
    memcpy(c, o, sizeof o);
}

void inv34(const float* m, float* out) {
    const float a = m[0], b = m[1], c = m[2], d = m[4], e = m[5], f = m[6], g = m[8], h = m[9], i = m[10];
    const float det = a * (e * i - f * h) - b * (d * i - f * g) + c * (d * h - e * g), k = det != 0 ? 1 / det : 0;
    const float r[9] = {(e * i - f * h) * k, (c * h - b * i) * k, (b * f - c * e) * k,
                        (f * g - d * i) * k, (a * i - c * g) * k, (c * d - a * f) * k,
                        (d * h - e * g) * k, (b * g - a * h) * k, (a * e - b * d) * k};
    for (int row = 0; row < 3; row++) {
        out[row * 4] = r[row * 3], out[row * 4 + 1] = r[row * 3 + 1], out[row * 4 + 2] = r[row * 3 + 2];
        out[row * 4 + 3] = -(r[row * 3] * m[3] + r[row * 3 + 1] * m[7] + r[row * 3 + 2] * m[11]);
    }
}

void Animator::play(const char* name, float rate, bool lock, float m) {
    const int i = a->anim(name);
    if (cur.anim == i && !lock) { cur.rate = rate; return; }
    from = cur, blend = cur.anim >= 0 ? 1.f : 0.f, morf = std::max(1.f, m);
    cur = {i, 0, rate, lock}, locked = lock;
}

void Animator::advance() {
    for (Play* p : {&cur, &from}) {
        if (p->anim < 0) continue;
        const auto& an = a->anims[p->anim];
        p->frame += p->rate;
        if (an.loop && !p->once) p->frame = std::fmod(p->frame, (float)an.frames);
        else p->frame = std::min(p->frame, (float)(an.frames - 1));
    }
    blend = std::max(0.f, blend - 1 / morf);
}

bool Animator::finished() const {
    return cur.anim >= 0 && (cur.once || !a->anims[cur.anim].loop) && cur.frame >= a->anims[cur.anim].frames - 1;
}

void Animator::pose(float back, float* world, float* skin) const {
    const int joints = (int)a->parent.size();
    for (int j = 0; j < joints; j++) {
        float s[3], q[4], t[3];
        sample(*a, cur, cur.frame - back * cur.rate, j, s, q, t);
        const float w = std::clamp(blend + back / morf, 0.f, 1.f);
        if (w > 0 && from.anim >= 0) {
            float s2[3], q2[4], t2[3];
            sample(*a, from, from.frame - back * from.rate, j, s2, q2, t2);
            const float sign = q[0] * q2[0] + q[1] * q2[1] + q[2] * q2[2] + q[3] * q2[3] < 0 ? -1.f : 1.f;
            for (int i = 0; i < 3; i++) s[i] += (s2[i] - s[i]) * w, t[i] += (t2[i] - t[i]) * w;
            for (int i = 0; i < 4; i++) q[i] += (sign * q2[i] - q[i]) * w;
        }
        const float n = std::sqrt(q[0] * q[0] + q[1] * q[1] + q[2] * q[2] + q[3] * q[3]);
        const float x = q[0] / n, y = q[1] / n, z = q[2] / n, ww = q[3] / n;
        const float local[12] = {(1 - 2 * (y * y + z * z)) * s[0], 2 * (x * y - z * ww) * s[1], 2 * (x * z + y * ww) * s[2], t[0],
                                 2 * (x * y + z * ww) * s[0], (1 - 2 * (x * x + z * z)) * s[1], 2 * (y * z - x * ww) * s[2], t[1],
                                 2 * (x * z - y * ww) * s[0], 2 * (y * z + x * ww) * s[1], (1 - 2 * (x * x + y * y)) * s[2], t[2]};
        const int p = a->parent[j];
        if (p >= 0) mul34(&world[p * 12], local, &world[j * 12]);
        else memcpy(&world[j * 12], local, sizeof local);
        if (skin) mul34(&world[j * 12], &a->inv_bind[j * 12], skin + j * 12);
    }
}

void Monster::begin(Kind k) {
    *this = Monster{};
    kind = k, out = true;
    anim.a = asset(k);
    pieces.resize(anim.a ? anim.a->parts.size() : 0);
}

void Monster::step_start() {
    pfade = fade, plift = lift, px = x, py = y, pz = z, pyaw = yaw;
    phair = hair;
    for (auto& p : pieces) memcpy(p.pm, p.m, sizeof p.m);
}

void Monster::base(float t, float* m) const {
    const float a = pyaw + (yaw - pyaw) * t, cs = std::cos(a) * scale, sn = std::sin(a) * scale;
    const float w[12] = {cs, 0, sn, (float)(px + (x - px) * t),
                         0, scale, 0, (float)(py + (y - py) * t + plift + (lift - plift) * t),
                         -sn, 0, cs, (float)(pz + (z - pz) * t)};
    memcpy(m, w, sizeof w);
}

void Monster::joint(int j, float* out) const {
    std::vector<float> world(anim.a->parent.size() * 12);
    float m[12];
    anim.pose(0, world.data(), nullptr), base(1, m), mul34(m, &world[j * 12], out);
}

void add(Monster* m) {
    if (std::find(g_monsters.begin(), g_monsters.end(), m) == g_monsters.end()) g_monsters.push_back(m);
}
void remove(Monster* m) { std::erase(g_monsters, m); }

void read_light(uint32_t actor, float* l) {
    // HD dKy_tevstr_c: light position in the world +0x84, ambient C0 s16 +0x90, light colour K0 +0x98, fog colour s16
    // +0xA0, fog start/end +0xA8/+0xAC (WW's own models get these through setLightTevColorType)
    const uint32_t tv = actor + 0x110;
    for (int i = 0; i < 3; i++)
        l[i] = (int16_t)ld16(tv + 0x90 + 2 * i) / 255.f, l[3 + i] = ld8(tv + 0x98 + i) / 255.f, l[6 + i] = (float)ldf32(tv + 0x84 + 4 * i),
        l[9 + i] = (int16_t)ld16(tv + 0xA0 + 2 * i) / 255.f;
    l[12] = (float)ldf32(tv + 0xA8), l[13] = (float)ldf32(tv + 0xAC);
}

void sound(uint32_t id, float x, float y, float z, float seconds) {
    static std::unordered_map<uint32_t, int> clips;
    auto it = clips.find(id);
    if (it == clips.end()) {
        char name[48];
        snprintf(name, sizeof name, "/tp_sounds/%X.wav", id);
        it = clips.emplace(id, sfx::load(config::game_dir + name)).first;
    }
    sfx::play(it->second, x, y, z, 1, false, seconds);
}

void camera_drawn(uint32_t cam) {
    if (g_monsters.empty() && !tpfx::any()) return;
    static std::vector<uint32_t> words;
    static std::vector<float> fx, world;
    static std::vector<uint32_t> groups;
    words.assign(29, 0);
    auto f = [&](size_t i) -> float& { return reinterpret_cast<float*>(words.data())[i]; };
    // camera_draw's matrices (HD view_class): the view (3x4) at +0x144, the projection (GL style, 4x4) at +0x104
    for (int i = 0; i < 12; i++) f(i) = (float)ldf32(cam + 0x144 + 4 * i);
    for (int i = 0; i < 16; i++) f(12 + i) = (float)ldf32(cam + 0x104 + 4 * i);
    const float V[12] = {f(0), f(1), f(2), f(3), f(4), f(5), f(6), f(7), f(8), f(9), f(10), f(11)};
    const float eye[3] = {-(V[0] * V[3] + V[4] * V[7] + V[8] * V[11]), -(V[1] * V[3] + V[5] * V[7] + V[9] * V[11]),
                          -(V[2] * V[3] + V[6] * V[7] + V[10] * V[11])};
    sfx::listener(eye, V);  // the view matrix's first row: the camera's right
    const float t = interp::pass_t(), back = 1 - t;
    uint32_t count = 0;
    for (const Monster* m : g_monsters) {
        const Asset* a = m->anim.a;
        if (!m->out || !a || m->anim.cur.anim < 0) continue;
        count++;
        const uint32_t joints = (uint32_t)a->parent.size(), pieces = (uint32_t)m->pieces.size();
        const uint32_t hair = (uint32_t)(m->phair.size() == m->hair.size() ? m->hair.size() / 4 : 0);
        const size_t at = words.size();
        words.resize(at + kMonsterHeader + joints * 12 + pieces * kPieceFloats + hair * 4);
        float* o = reinterpret_cast<float*>(words.data() + at);
        float mm[12];
        m->base(t, mm);
        words[at] = (uint32_t)m->kind, o[1] = m->pfade + (m->fade - m->pfade) * t, words[at + 2] = (uint32_t)m->variant;
        o[3] = (float)(m->py + (m->y - m->py) * t);
        memcpy(o + 4, mm, 48);
        memcpy(o + 16, m->light, sizeof m->light);
        words[at + 30] = joints, words[at + 31] = pieces, words[at + 32] = hair;
        world.resize(joints * 12);
        float* skin = o + kMonsterHeader;
        m->anim.pose(back, world.data(), skin);
        float* p = skin + joints * 12;
        for (uint32_t i = 0; i < pieces; i++, p += kPieceFloats) {
            const Monster::Piece& pc = m->pieces[i];
            const int joint = a->parts[i].joint;
            p[0] = pc.mode == Monster::Piece::kHidden ? 2.f : pc.mode == Monster::Piece::kOn ? 0.f : 1.f;
            if (pc.mode == Monster::Piece::kHeld) {  // in its hand: world = base x held joint x offset, from its bind pose
                float w[12], jo[12];
                mul34(&world[pc.held * 12], pc.offset, jo);
                mul34(mm, jo, w);
                mul34(w, &a->inv_bind[joint * 12], p + 1);
            } else if (pc.mode == Monster::Piece::kLoose) {
                float w[12];
                for (int k = 0; k < 12; k++) w[k] = pc.pm[k] + (pc.m[k] - pc.pm[k]) * t;
                mul34(w, &a->inv_bind[joint * 12], p + 1);
            }
        }
        for (uint32_t i = 0; i < hair * 4; i++) p[i] = (i % 4 == 3) ? m->hair[i] : m->phair[i] + (m->hair[i] - m->phair[i]) * t;
    }
    words[28] = count;
    tpfx::build(t, eye, fx, groups);
    words.push_back((uint32_t)(fx.size() / tpfx::kVertexFloats));
    words.push_back((uint32_t)(groups.size() / 4));
    words.insert(words.end(), groups.begin(), groups.end());
    const uint32_t* raw = reinterpret_cast<const uint32_t*>(fx.data());
    words.insert(words.end(), raw, raw + fx.size());
    gx2::emit_host(gx2::OP_BEAST, words.data(), (uint32_t)words.size());
}

}  // namespace mods::tpm
