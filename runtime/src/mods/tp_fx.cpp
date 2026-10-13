// Twilight Princess's particle effects (tp_fx.h): a small JPA2 emitter simulation with the emitters' own parameters
// (BEM1 volume and velocities, FLD1 fields, BSP1 shape and colours, ESP1 scale/alpha/rotation curves).
#include "tp_fx.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <random>
#include <string>

#include "mods.h"
#include "runtime.h"

namespace mods::tpfx {
namespace {

// tools/tp_particles.py's 96 floats an emitter
struct Def {
    float volume, fixed_density, fixed_interval, scale[3], trs[3], dir[3];
    float vel_omni, vel_axis, vel_rndm, vel_dir, spread, ratio;
    float rate, rate_rndm, life_rndm, sweep, min_rad, air, moment;
    float max_frame, start, life, vol_size, div;
    float shape, dir_type, size_x, size_y, blend, alpha_ref, colour, a0_only, prm[4], env[4];
    float keys, key[4][5];
    float texture;
    float scale_anm, s_in_t, s_in_x, s_in_y, s_out_t, s_out_x, s_out_y, s_rndm;
    float alpha_anm, a_in_t, a_in_v, a_base, a_out_t, a_out_v;
    float rot_anm, rot_rndm, rot_speed, rot_speed_rndm;
    float field[2][5];
};
static_assert(sizeof(Def) == 96 * 4, "Def is tools/tp_particles.py's kFloats");
enum Volume { kCube, kSphere, kCylinder, kTorus, kPoint, kCircle, kLine };
enum Shape { kPointShape, kLineShape, kBillboard, kDirection, kDirCross, kStripe, kStripeCross, kRotation, kRotCross, kDirBB, kYBB };

struct Particle {
    float p[3], pp[3], v[3];  // place, last step's, velocity
    float age, life, sx, sy, rot, rot_speed;
    uint32_t emitter;         // its emitter (stripes join one emitter's particles)
};
struct Emitter { uint32_t handle; const Def* def; float m[12], scale, age, carry; bool on; };

std::vector<Texture> g_tex;
std::vector<std::pair<uint32_t, Def>> g_defs;
std::vector<Emitter> g_emitters;
std::vector<Particle> g_parts;
uint32_t g_next = 1;
bool g_loaded = false, g_ok = false;
std::mt19937 g_rng{12345};
float rnd() { return std::uniform_real_distribution<float>(0, 1)(g_rng); }
float rnd2() { return rnd() * 2 - 1; }

bool load() {
    g_loaded = true;
    const std::string path = config::game_dir + "/tp_particles.bin";
    FILE* f = fopen(path.c_str(), "rb");
    if (!f) { trace("tpfx: %s missing (tools/tp_particles.py makes it from the TP HD dump)", path.c_str()); return false; }
    std::vector<uint8_t> d;
    uint8_t buf[65536];
    for (size_t n; (n = fread(buf, 1, sizeof buf, f)) > 0;) d.insert(d.end(), buf, buf + n);
    fclose(f);
    size_t o = 0;
    auto take = [&](void* out, size_t n) {
        if (o + n > d.size()) return false;
        memcpy(out, d.data() + o, n), o += n;
        return true;
    };
    char magic[4];
    uint32_t version = 0, n = 0;
    if (!take(magic, 4) || memcmp(magic, "TPFX", 4) || !take(&version, 4) || version != 1 || !take(&n, 4)) return false;
    g_tex.resize(n);
    for (auto& t : g_tex) {
        if (!take(&t.w, 4) || !take(&t.h, 4)) return false;
        t.rgba.resize((size_t)t.w * t.h * 4);
        if (!take(t.rgba.data(), t.rgba.size())) return false;
    }
    if (!take(&n, 4)) return false;
    g_defs.resize(n);
    for (auto& [id, def] : g_defs)
        if (!take(&id, 4) || !take(&def, sizeof def)) return false;
    trace("tpfx: %zu emitters, %zu textures", g_defs.size(), g_tex.size());
    return true;
}

void xf(const float* m, const float* v, float* o, float w = 1) {
    for (int r = 0; r < 3; r++) o[r] = m[r * 4] * v[0] + m[r * 4 + 1] * v[1] + m[r * 4 + 2] * v[2] + m[r * 4 + 3] * w;
}
void unit(float* v) { do v[0] = rnd2(), v[1] = rnd2(), v[2] = rnd2(); while (v[0] * v[0] + v[1] * v[1] + v[2] * v[2] > 1 || v[0] * v[0] + v[1] * v[1] + v[2] * v[2] < 1e-4f);
    const float l = std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]); for (int i = 0; i < 3; i++) v[i] /= l; }

// a particle of emitter `e`: its place in the emitter's volume, its velocity (JPA: omni + axis + direction within the
// spread, the whole times 1 +- ratio, plus a random part), size and spin
void emit(Emitter& e, int index, int count) {
    const Def& d = *e.def;
    Particle p{};
    float local[3] = {0, 0, 0}, radial[3] = {0, 0, 0};
    const float r = d.vol_size;
    switch ((int)d.volume) {
    case kSphere: case kCube: case kCylinder: case kTorus: {
        unit(radial);
        const float k = d.volume == kCube ? 1 : std::cbrt(rnd()) * (1 - d.min_rad) + d.min_rad;
        for (int i = 0; i < 3; i++) local[i] = radial[i] * r * k;
        if (d.volume == kCylinder) local[1] = rnd2() * r;
        break;
    }
    case kCircle: {
        const float a = (d.fixed_interval && count > 1 ? (float)index / count : rnd()) * 2 * (float)M_PI * d.sweep;
        radial[0] = std::sin(a), radial[2] = std::cos(a);
        const float k = d.fixed_density ? 1 : rnd() * (1 - d.min_rad) + d.min_rad;
        local[0] = radial[0] * r * k, local[2] = radial[2] * r * k;
        break;
    }
    case kLine:
        local[0] = d.fixed_interval && count > 1 ? r * ((float)index / (count - 1) - 0.5f) : r * rnd2() * 0.5f;
        radial[0] = local[0] < 0 ? -1.f : 1.f;
        break;
    default: break;
    }
    for (int i = 0; i < 3; i++) local[i] = local[i] * d.scale[i] + d.trs[i];
    float v[3], omni[3];
    unit(omni);
    float dir[3] = {d.dir[0], d.dir[1], d.dir[2]};
    if (d.spread > 0) {
        float o[3];
        unit(o);
        for (int i = 0; i < 3; i++) dir[i] += o[i] * d.spread * rnd();
        const float l = std::sqrt(dir[0] * dir[0] + dir[1] * dir[1] + dir[2] * dir[2]);
        for (float& c : dir) c /= l;
    }
    for (int i = 0; i < 3; i++) v[i] = omni[i] * d.vel_omni + radial[i] * d.vel_axis + dir[i] * d.vel_dir;
    const float k = 1 + d.ratio * rnd2();
    float jitter[3];
    unit(jitter);
    for (int i = 0; i < 3; i++) v[i] = v[i] * k + jitter[i] * d.vel_rndm * rnd();
    xf(e.m, local, p.p);
    xf(e.m, v, p.v, 0);
    memcpy(p.pp, p.p, sizeof p.p);
    p.life = std::max(1.f, d.life * (1 - d.life_rndm * rnd()));
    const float s = 1 + d.s_rndm * rnd2();
    p.sx = d.size_x * s, p.sy = d.size_y * s;
    if (d.rot_anm) {
        p.rot = rnd() * d.rot_rndm * 2 * (float)M_PI / 65536;
        p.rot_speed = d.rot_speed * (1 + d.rot_speed_rndm * rnd2()) * 2 * (float)M_PI / 65536 * (rnd() < 0.5f ? -1 : 1);
    }
    p.emitter = e.handle;
    g_parts.push_back(p);
}

float curve(float t, float in_t, float in_v, float out_t, float out_v, float base = 1) {
    if (t < in_t) return in_v + (base - in_v) * t / in_t;
    if (t > out_t && out_t < 1) return base + (out_v - base) * (t - out_t) / (1 - out_t);
    return base;
}

}  // namespace

const std::vector<Texture>* textures() {
    if (!g_loaded) g_ok = load();
    return g_ok ? &g_tex : nullptr;
}

uint32_t spawn(uint32_t id, const float pos[3], float scale) {
    if (!textures()) return 0;
    for (auto& [i, def] : g_defs)
        if (i == id) {
            Emitter e{g_next++, &def, {scale, 0, 0, pos[0], 0, scale, 0, pos[1], 0, 0, scale, pos[2]}, scale, 0, 0, true};
            g_emitters.push_back(e);
            return e.handle;
        }
    return 0;
}

void follow(uint32_t h, const float mtx[12]) {
    for (auto& e : g_emitters)
        if (e.handle == h) memcpy(e.m, mtx, sizeof e.m);
}

void stop(uint32_t h) {
    for (auto& e : g_emitters)
        if (e.handle == h) e.on = false;
}

bool any() { return !g_parts.empty(); }

void step() {
    for (auto& e : g_emitters) {
        const Def& d = *e.def;
        const float since = e.age - d.start;
        if (e.on && since >= 0 && (d.max_frame <= 0 || since < d.max_frame)) {
            // particles this step: the rate (with its randomness, carried over as a fraction); a line with a fixed
            // interval lays `div` of them along itself at once, a circle its rate's worth evenly round it
            if (d.fixed_interval && (d.volume == kLine || d.volume == kCircle) && d.rate > 0) {
                const int n = d.volume == kLine ? std::max(2, (int)d.div) : std::max(1, (int)(d.rate + 0.5f));
                for (int i = 0; i < n; i++) emit(e, i, n);
            } else {
                e.carry += d.rate * (1 + d.rate_rndm * rnd2());
                for (; e.carry >= 1; e.carry -= 1) emit(e, 0, 1);
            }
        }
        e.age += 1;
        if (d.max_frame > 0 && since >= d.max_frame) e.on = false;
    }
    std::erase_if(g_emitters, [](const Emitter& e) {
        return !e.on && std::none_of(g_parts.begin(), g_parts.end(), [&](const Particle& p) { return p.emitter == e.handle; });
    });
    for (auto& p : g_parts) {
        const Emitter* e = nullptr;
        for (auto& x : g_emitters)
            if (x.handle == p.emitter) e = &x;
        if (!e) { p.age = p.life; continue; }
        const Def& d = *e->def;
        memcpy(p.pp, p.p, sizeof p.p);
        for (const auto& fl : d.field) {  // FLD1: 0 gravity (along its direction), 5 random
            if (fl[0] == 0) for (int i = 0; i < 3; i++) p.v[i] += fl[1 + i] * fl[4] * e->scale;
            else if (fl[0] == 5) { float r[3]; unit(r); for (int i = 0; i < 3; i++) p.v[i] += r[i] * fl[4] * 0.3f; }
        }
        for (int i = 0; i < 3; i++) p.v[i] *= d.air, p.p[i] += p.v[i];
        p.rot += p.rot_speed;
        p.age += 1;
    }
    std::erase_if(g_parts, [](const Particle& p) { return p.age >= p.life; });
}

void build(float t, const float eye[3], std::vector<float>& verts, std::vector<uint32_t>& groups) {
    verts.clear(), groups.clear();
    // by texture and blend, so each is one draw
    std::vector<const Particle*> order;
    for (auto& p : g_parts) order.push_back(&p);
    auto def_of = [](const Particle* p) -> const Def* {
        for (auto& e : g_emitters)
            if (e.handle == p->emitter) return e.def;
        return nullptr;
    };
    auto scale_of = [](const Particle* p) {
        for (auto& e : g_emitters)
            if (e.handle == p->emitter) return e.scale;
        return 1.f;
    };
    std::stable_sort(order.begin(), order.end(), [&](const Particle* a, const Particle* b) {
        const Def *da = def_of(a), *db = def_of(b);
        return std::make_pair(da ? da->texture : 0, a->emitter) < std::make_pair(db ? db->texture : 0, b->emitter);
    });
    auto put = [&](const float* p, float u, float v, const Def& d, const float* prm) {
        const float w[kVertexFloats] = {p[0], p[1], p[2], d.colour, u, v, d.a0_only, d.alpha_ref,
                                        prm[0], prm[1], prm[2], prm[3], d.env[0], d.env[1], d.env[2], 0};
        verts.insert(verts.end(), w, w + kVertexFloats);
    };
    size_t i = 0;
    while (i < order.size()) {
        const Def* d = def_of(order[i]);
        size_t j = i;
        while (j < order.size() && def_of(order[j]) == d) j++;
        if (!d) { i = j; continue; }
        const uint32_t first = (uint32_t)(verts.size() / kVertexFloats);
        const float gs = scale_of(order[i]);
        // each particle's place, colour and size now
        struct Now { float p[3], dir[3], hx, hy, rot, prm[4]; };
        std::vector<Now> now;
        for (size_t k = i; k < j; k++) {
            const Particle& p = *order[k];
            Now n{};
            for (int c = 0; c < 3; c++) n.p[c] = p.pp[c] + (p.p[c] - p.pp[c]) * t, n.dir[c] = p.v[c];
            const float age = std::max(0.f, p.age - 1 + t), life_t = std::min(1.f, age / p.life);
            const float kx = d->scale_anm ? curve(life_t, d->s_in_t, d->s_in_x, d->s_out_t, d->s_out_x) : 1;
            const float ky = d->scale_anm ? curve(life_t, d->s_in_t, d->s_in_y, d->s_out_t, d->s_out_y) : 1;
            n.hx = 25 * p.sx * kx * gs, n.hy = 25 * p.sy * ky * gs;  // JPA's quad: 25 x its size, times the global scale
            n.rot = p.rot + p.rot_speed * (t - 1);
            memcpy(n.prm, d->prm, sizeof n.prm);
            if (d->keys > 0) {  // the colour table over the particle's frames
                const int nk = (int)d->keys;
                int a = 0;
                while (a + 1 < nk && d->key[a + 1][0] <= age) a++;
                const int b = std::min(a + 1, nk - 1);
                const float span = d->key[b][0] - d->key[a][0], f = span > 0 ? std::clamp((age - d->key[a][0]) / span, 0.f, 1.f) : 0;
                for (int c = 0; c < 4; c++) n.prm[c] = d->key[a][1 + c] + (d->key[b][1 + c] - d->key[a][1 + c]) * f;
            }
            if (d->alpha_anm) n.prm[3] *= curve(life_t, d->a_in_t, d->a_in_v, d->a_out_t, d->a_out_v, d->a_base);
            now.push_back(n);
        }
        if (d->shape == kStripe || d->shape == kStripeCross) {
            // a ribbon through the emitter's particles in the order they were made, across the way the camera sees it
            for (size_t k = 0; k + 1 < now.size(); k++) {
                if (order[i + k]->emitter != order[i + k + 1]->emitter) continue;
                float side[2][2][3];
                for (int s = 0; s < 2; s++) {
                    const Now& a = now[k + s];
                    const Now& b = now[std::min(k + s + 1, now.size() - 1)];
                    const Now& z = now[k + s > 0 ? k + s - 1 : 0];
                    const float dd[3] = {b.p[0] - z.p[0], b.p[1] - z.p[1], b.p[2] - z.p[2]};
                    const float v[3] = {eye[0] - a.p[0], eye[1] - a.p[1], eye[2] - a.p[2]};
                    float x[3] = {dd[1] * v[2] - dd[2] * v[1], dd[2] * v[0] - dd[0] * v[2], dd[0] * v[1] - dd[1] * v[0]};
                    const float l = std::sqrt(x[0] * x[0] + x[1] * x[1] + x[2] * x[2]);
                    for (int c = 0; c < 3; c++) x[c] = l > 1e-4f ? x[c] / l * a.hx : 0, side[s][0][c] = a.p[c] - x[c], side[s][1][c] = a.p[c] + x[c];
                }
                const float u0 = (float)k / std::max<size_t>(1, now.size() - 1), u1 = (float)(k + 1) / std::max<size_t>(1, now.size() - 1);
                put(side[0][0], u0, 0, *d, now[k].prm), put(side[0][1], u0, 1, *d, now[k].prm), put(side[1][0], u1, 0, *d, now[k + 1].prm);
                put(side[1][0], u1, 0, *d, now[k + 1].prm), put(side[0][1], u0, 1, *d, now[k].prm), put(side[1][1], u1, 1, *d, now[k + 1].prm);
            }
        } else {
            for (const Now& n : now) {
                // the quad's axes: facing the camera (billboard, point), or along the particle's velocity (direction)
                const float v[3] = {eye[0] - n.p[0], eye[1] - n.p[1], eye[2] - n.p[2]};
                float fwd[3] = {v[0], v[1], v[2]};
                const float fl = std::sqrt(fwd[0] * fwd[0] + fwd[1] * fwd[1] + fwd[2] * fwd[2]) + 1e-6f;
                for (float& c : fwd) c /= fl;
                float ax[3], ay[3];
                if (d->shape == kDirection || d->shape == kDirCross || d->shape == kDirBB) {
                    float dl = std::sqrt(n.dir[0] * n.dir[0] + n.dir[1] * n.dir[1] + n.dir[2] * n.dir[2]);
                    const float up[3] = {0, 1, 0};
                    for (int c = 0; c < 3; c++) ay[c] = dl > 1e-4f ? n.dir[c] / dl : up[c];
                    ax[0] = ay[1] * fwd[2] - ay[2] * fwd[1], ax[1] = ay[2] * fwd[0] - ay[0] * fwd[2], ax[2] = ay[0] * fwd[1] - ay[1] * fwd[0];
                    dl = std::sqrt(ax[0] * ax[0] + ax[1] * ax[1] + ax[2] * ax[2]) + 1e-6f;
                    for (float& c : ax) c /= dl;
                } else if (d->shape == kRotation || d->shape == kRotCross) {  // flat on its plane (the ground)
                    ax[0] = 1, ax[1] = 0, ax[2] = 0, ay[0] = 0, ay[1] = 0, ay[2] = 1;
                } else {
                    const float up[3] = {0, 1, 0};
                    ax[0] = up[1] * fwd[2] - up[2] * fwd[1], ax[1] = up[2] * fwd[0] - up[0] * fwd[2], ax[2] = up[0] * fwd[1] - up[1] * fwd[0];
                    float l = std::sqrt(ax[0] * ax[0] + ax[1] * ax[1] + ax[2] * ax[2]) + 1e-6f;
                    for (float& c : ax) c /= l;
                    ay[0] = fwd[1] * ax[2] - fwd[2] * ax[1], ay[1] = fwd[2] * ax[0] - fwd[0] * ax[2], ay[2] = fwd[0] * ax[1] - fwd[1] * ax[0];
                }
                const float cs = std::cos(n.rot), sn = std::sin(n.rot);
                float rx[3], ry[3];
                for (int c = 0; c < 3; c++) rx[c] = (ax[c] * cs + ay[c] * sn) * n.hx, ry[c] = (ay[c] * cs - ax[c] * sn) * n.hy;
                float q[4][3];
                for (int c = 0; c < 3; c++)
                    q[0][c] = n.p[c] - rx[c] - ry[c], q[1][c] = n.p[c] + rx[c] - ry[c], q[2][c] = n.p[c] - rx[c] + ry[c], q[3][c] = n.p[c] + rx[c] + ry[c];
                put(q[0], 0, 1, *d, n.prm), put(q[1], 1, 1, *d, n.prm), put(q[2], 0, 0, *d, n.prm);
                put(q[2], 0, 0, *d, n.prm), put(q[1], 1, 1, *d, n.prm), put(q[3], 1, 0, *d, n.prm);
            }
        }
        const uint32_t count = (uint32_t)(verts.size() / kVertexFloats) - first;
        if (count) groups.insert(groups.end(), {(uint32_t)d->texture, (uint32_t)d->blend, first, count});
        i = j;
    }
}

}  // namespace mods::tpfx
