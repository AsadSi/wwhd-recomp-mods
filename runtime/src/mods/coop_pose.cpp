// Online co-op: Link's pose and the puppets (coop_pose.h).
//
// Link's models: daPy_lk_c::createHeap (023D5548) makes them all with mDoExt_J3DModel__create (025E38E0); right after
// it, on the same heap, each is made once more for every puppet (a model that does not fit is left out). Link's draw
// (023D9820) enters them through entryDLSetLight (023D8DF0) / updateDLSetLight (023D8454): there, once a logic step,
// each model's world joint matrices (model +0x2C -> block +0x10, count block +0x2C) are taken relative to Link's place
// and heading. Right after each of his models is entered, each puppet's copy of it gets its matrices from the puppet's
// pose at its own place and is entered the same way (Link as the light's owner), the ones he did not draw after his
// draw: his sword and shield, and no calc (Link's joint callbacks take any model they run on for his). Known: the
// copies draw in one blue (docs/multiplayer.md, Status).
#include "coop_pose.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "guest_addr.h"
#include "extras.h"
#include "mods.h"
#include "runtime.h"

namespace interp {
bool mtx_to_quat(const float* m, float* s, float* q);
}

namespace mods::coop {
namespace {

constexpr uint32_t kPos = 0x314, kRoom = 0x326, kAngleY = 0x32A, kCurProc = 0x65F0, kEquip = 0x69B0;
constexpr uint32_t kMdlJoints = 0x2C, kJntMtx = 0x10, kJntNum = 0x2C, kMdlBase = 0xC8;
constexpr int kMaxModels = 64, kMaxJoints = 128;
const uint32_t kPlay = GD(0x1046F0B0);
const uint32_t kStage = kPlay + 0x5134;

struct LinkModel { uint32_t model, data, flags, diff; uint32_t twin[kPuppets]; };
std::vector<LinkModel> g_models;  // Link's, in the order createHeap made them
bool g_in_heap = false;

bool g_want = false;  // take a pose at Link's next draw
bool g_in_draw = false;
bool g_have = false;
Pose g_pose, g_taking;

struct Puppet {
    bool shown = false;
    Pose pose;
    float pos[3] = {};
    int16_t yaw = 0;
} g_puppet[kPuppets];

float f32(uint32_t a) { return (float)ldf32(a); }
uint32_t bits(float f) { uint32_t u; memcpy(&u, &f, 4); return u; }

void rot_y(int16_t yaw, float& s, float& c) {
    const float a = yaw * (float)M_PI / 32768;
    s = std::sin(a), c = std::cos(a);
}

// a model's joints, relative to Link's place and heading (inverse of T(pos) * Ry(yaw), then the world matrix)
void take(int k, uint32_t model, uint32_t param, uint32_t link) {
    const uint32_t jnt = ld32(model + kMdlJoints), mtx = jnt ? ld32(jnt + kJntMtx) : 0, n = jnt ? ld16(jnt + kJntNum) : 0;
    if (!mtx || !n || n > kMaxJoints) return;
    for (const Model& m : g_taking.models) if (m.k == k) return;  // (entered twice in one draw)
    float s, c;
    rot_y((int16_t)ld16(link + kAngleY), s, c);
    const float p[3] = {f32(link + kPos), f32(link + kPos + 4), f32(link + kPos + 8)};
    Model out;
    out.k = (uint8_t)k, out.param = param ? 1 : 0;
    out.j.resize(n);
    for (uint32_t i = 0; i < n; i++) {
        float w[12], r[12];
        for (int e = 0; e < 12; e++) w[e] = f32(mtx + 48 * i + 4 * e);
        // R^-1 = [Ry^T | -Ry^T p]; Ry = [[c,0,s],[0,1,0],[-s,0,c]]
        for (int col = 0; col < 4; col++) {
            const float x = w[col] - (col == 3 ? p[0] : 0), y = w[4 + col] - (col == 3 ? p[1] : 0), z = w[8 + col] - (col == 3 ? p[2] : 0);
            r[col] = c * x - s * z, r[4 + col] = y, r[8 + col] = s * x + c * z;
        }
        float sc[3], q[4] = {1, 0, 0, 0};
        Joint& j = out.j[i];
        const bool ok = interp::mtx_to_quat(r, sc, q);
        const float us = ok ? (sc[0] + sc[1] + sc[2]) / 3 : 0;
        for (int e = 0; e < 4; e++) j.q[e] = (int16_t)std::lround(std::clamp(q[e], -1.f, 1.f) * 32767);
        for (int e = 0; e < 3; e++) j.t[e] = (int16_t)std::clamp<long>(std::lround(r[4 * e + 3] * 8), -32768, 32767);
        j.s = (uint8_t)std::clamp<long>(std::lround(us * 64), 0, 255);
    }
    g_taking.models.push_back(std::move(out));
}

// a joint back to a world matrix at a place and heading
void put(const Joint& j, const float p[3], float s, float c, uint32_t at) {
    float w = j.q[0] / 32767.f, x = j.q[1] / 32767.f, y = j.q[2] / 32767.f, z = j.q[3] / 32767.f;
    const float n = 1 / std::sqrt(std::max(1e-9f, w * w + x * x + y * y + z * z));
    w *= n, x *= n, y *= n, z *= n;
    const float sc = j.s / 64.f;
    const float r[3][3] = {{1 - 2 * (y * y + z * z), 2 * (x * y - w * z), 2 * (x * z + w * y)},
                           {2 * (x * y + w * z), 1 - 2 * (x * x + z * z), 2 * (y * z - w * x)},
                           {2 * (x * z - w * y), 2 * (y * z + w * x), 1 - 2 * (x * x + y * y)}};
    float l[12];
    for (int i = 0; i < 3; i++) {
        for (int k = 0; k < 3; k++) l[4 * i + k] = r[i][k] * sc;
        l[4 * i + 3] = j.t[i] / 8.f;
    }
    // world = T(p) * Ry * local
    for (int col = 0; col < 4; col++) {
        const float x0 = l[col], y0 = l[4 + col], z0 = l[8 + col];
        st32(at + 4 * col, bits(c * x0 + s * z0 + (col == 3 ? p[0] : 0)));
        st32(at + 16 + 4 * col, bits(y0 + (col == 3 ? p[1] : 0)));
        st32(at + 32 + 4 * col, bits(-s * x0 + c * z0 + (col == 3 ? p[2] : 0)));
    }
}

int index_of(uint32_t model) {
    for (size_t k = 0; k < g_models.size(); k++)
        if (g_models[k].model == model) return (int)k;
    return -1;
}

// the test aids
FILE* g_rec = nullptr;
std::vector<Pose> g_ghost;
size_t g_ghost_at = 0;
float g_ghost_off[3] = {150, 0, 0};

void load_ghost(const char* spec) {
    std::string path = spec;
    if (const size_t at = path.find('@'); at != std::string::npos) {
        sscanf(path.c_str() + at + 1, "%f,%f", &g_ghost_off[0], &g_ghost_off[2]);
        path.resize(at);
    }
    FILE* f = fopen(path.c_str(), "rb");
    if (!f) { trace("coop: ghost %s not found", path.c_str()); return; }
    std::vector<uint8_t> d;
    uint8_t buf[65536];
    for (size_t n; (n = fread(buf, 1, sizeof buf, f)) > 0;) d.insert(d.end(), buf, buf + n);
    fclose(f);
    for (size_t o = 0; o < d.size();) {
        Pose p;
        size_t used = 0;
        if (!unpack(d.data() + o, d.size() - o, p, &used)) break;
        g_ghost.push_back(std::move(p)), o += used;
    }
    trace("coop: ghost %s, %zu steps, offset %.0f %.0f", path.c_str(), g_ghost.size(), g_ghost_off[0], g_ghost_off[2]);
}

}  // namespace

void pack(const Pose& p, std::string& out) {
    auto put_bytes = [&](const void* v, size_t n) { out.append((const char*)v, n); };
    const size_t start = out.size();
    uint32_t size = 0;
    put_bytes(&size, 4);  // filled in below
    put_bytes(p.stage, 8), put_bytes(&p.room, 1), put_bytes(&p.flags, 1), put_bytes(p.pos, 12), put_bytes(&p.yaw, 2);
    put_bytes(&p.proc, 2), put_bytes(&p.equip, 2);
    const uint8_t nm = (uint8_t)p.models.size();
    put_bytes(&nm, 1);
    for (const Model& m : p.models) {
        const uint8_t nj = (uint8_t)m.j.size();
        put_bytes(&m.k, 1), put_bytes(&m.param, 1), put_bytes(&nj, 1);
        for (const Joint& j : m.j) put_bytes(j.q, 8), put_bytes(j.t, 6), put_bytes(&j.s, 1);
    }
    size = (uint32_t)(out.size() - start);
    memcpy(&out[start], &size, 4);
}

bool unpack(const uint8_t* d, size_t n, Pose& p, size_t* used) {
    size_t o = 0;
    auto get = [&](void* v, size_t k) {
        if (o + k > n) return false;
        memcpy(v, d + o, k), o += k;
        return true;
    };
    uint32_t size = 0;
    if (!get(&size, 4) || size > n || size < 35) return false;
    n = size;
    uint8_t nm = 0;
    if (!get(p.stage, 8) || !get(&p.room, 1) || !get(&p.flags, 1) || !get(p.pos, 12) || !get(&p.yaw, 2) ||
        !get(&p.proc, 2) || !get(&p.equip, 2) || !get(&nm, 1))
        return false;
    p.models.resize(nm);
    for (Model& m : p.models) {
        uint8_t nj = 0;
        if (!get(&m.k, 1) || !get(&m.param, 1) || !get(&nj, 1)) return false;
        m.j.resize(nj);
        for (Joint& j : m.j)
            if (!get(j.q, 8) || !get(j.t, 6) || !get(&j.s, 1)) return false;
    }
    if (used) *used = size;
    return o == size;
}

void blend(const Pose& a, const Pose& b, float t, Pose& out) {
    out = b;
    for (int i = 0; i < 3; i++) out.pos[i] = a.pos[i] + (b.pos[i] - a.pos[i]) * t;
    out.yaw = (int16_t)(a.yaw + (int16_t)(b.yaw - a.yaw) * t);
    for (Model& m : out.models)
        for (const Model& n : a.models) {
            if (n.k != m.k || n.j.size() != m.j.size()) continue;
            for (size_t i = 0; i < m.j.size(); i++) {
                const Joint &x = n.j[i], &y = m.j[i];
                const int dot = x.q[0] * y.q[0] + x.q[1] * y.q[1] + x.q[2] * y.q[2] + x.q[3] * y.q[3], sign = dot < 0 ? -1 : 1;
                Joint& o = m.j[i];
                for (int e = 0; e < 4; e++) o.q[e] = (int16_t)std::lround(sign * x.q[e] + (y.q[e] - sign * x.q[e]) * t);  // (put() normalises)
                for (int e = 0; e < 3; e++) o.t[e] = (int16_t)std::lround(x.t[e] + (y.t[e] - x.t[e]) * t);
                o.s = (uint8_t)std::lround(x.s + (y.s - x.s) * t);
            }
            break;
        }
}

bool own_pose(Pose& out) {
    if (g_have) out = g_pose;
    return g_have;
}

void show(int i, const Pose* p, const float pos[3], int16_t yaw) {
    Puppet& u = g_puppet[i];
    u.shown = p != nullptr;
    if (!p) return;
    u.pose = *p;
    memcpy(u.pos, pos, sizeof u.pos), u.yaw = yaw;
}

void step(Cpu*, uint32_t link, bool on) {
    static bool init = false;
    if (!init) {
        init = true;
        if (const char* r = getenv("WWHD_COOP_RECORD")) g_rec = fopen(r, "wb"), trace("coop: recording Link to %s", r);
        if (const char* g = getenv("WWHD_COOP_GHOST")) load_ghost(g);
    }
    g_want = on && link;
    if (!on) {
        for (Puppet& u : g_puppet) u.shown = false;
        return;
    }
    if (g_rec && g_have) {
        std::string b;
        pack(g_pose, b);
        fwrite(b.data(), 1, b.size(), g_rec), fflush(g_rec);
    }
    if (!g_ghost.empty()) {  // the recording, step by step beside where it was made, then round again
        const Pose& p = g_ghost[g_ghost_at++ % g_ghost.size()];
        const float at[3] = {p.pos[0] + g_ghost_off[0], p.pos[1] + g_ghost_off[1], p.pos[2] + g_ghost_off[2]};
        const bool here = strncmp(p.stage, (const char*)mem::ptr(kStage), 8) == 0;
        show(0, here ? &p : nullptr, at, p.yaw);
        static int n = 0;
        if (++n % 150 == 0)
            trace("coop: ghost step %zu at %.0f %.0f %.0f, %zu models, %s", g_ghost_at, at[0], at[1], at[2], p.models.size(),
                  here ? "shown" : "another stage");
    }
}

}  // namespace mods::coop

using namespace mods::coop;

// daPy_lk_c::createHeap: Link's models, then a copy of each for every puppet on his heap
extern "C" void f_023D5548_orig(Cpu* c);
extern "C" void hook_023D5548(Cpu* c) {
    g_models.clear();
    g_have = false;
    g_in_heap = true;
    f_023D5548_orig(c);
    g_in_heap = false;
    const uint32_t ok = c->r[3];
    int made = 0, missed = 0;
    static bool (*const on)() = mods::extras::enabled_fn("coop");
    if (ok && on && on())
        for (LinkModel& m : g_models)
            for (uint32_t& t : m.twin) (t = guest_call(c, GC(0x025E38E0), {m.data, m.flags, m.diff})) ? made++ : missed++;
    c->r[3] = ok;
    mods::trace("coop: Link made %zu models; %d puppet copies made, %d did not fit", g_models.size(), made, missed);
}

// mDoExt_J3DModel__create(data, flags, diff flags): the ones Link's createHeap makes
extern "C" void f_025E38E0_orig(Cpu* c);
extern "C" void hook_025E38E0(Cpu* c) {
    const uint32_t data = c->r[3], flags = c->r[4], diff = c->r[5];
    f_025E38E0_orig(c);
    if (g_in_heap && c->r[3] && g_models.size() < kMaxModels) g_models.push_back({c->r[3], data, flags, diff, {}});
}

extern "C" void f_023D8DF0_orig(Cpu* c);
extern "C" void f_025E2E5C_orig(Cpu* c);

// puppet i's copy of Link's model k, posed and entered as Link's own was (param 0/1: entryDLSetLight with Link as the
// light's owner; 2: straight through modelEntryDL), each once a draw
static uint32_t g_link_drawing = 0;
static uint64_t g_entered[kPuppets];  // models entered this draw, by bit k
static void enter(Cpu* c, uint32_t link, int i, const Model& m) {
    const Puppet& u = g_puppet[i];
    if (m.k >= g_models.size() || m.k >= 64 || (g_entered[i] >> m.k & 1)) return;
    g_entered[i] |= 1ull << m.k;
    const uint32_t twin = g_models[m.k].twin[i];
    const uint32_t jnt = twin ? ld32(twin + kMdlJoints) : 0, mtx = jnt ? ld32(jnt + kJntMtx) : 0;
    if (!mtx || ld16(jnt + kJntNum) != m.j.size()) return;
    float s, co;
    rot_y(u.yaw, s, co);
    for (size_t j = 0; j < m.j.size(); j++) put(m.j[j], u.pos, s, co, mtx + 48 * (uint32_t)j);
    // its base transform: T(pos) * Ry
    const float base[12] = {co, 0, s, u.pos[0], 0, 1, 0, u.pos[1], -s, 0, co, u.pos[2]};
    for (int e = 0; e < 12; e++) st32(twin + kMdlBase + 4 * e, bits(base[e]));
    // its packets' draw flags as Link's model has them (027F596C: model +0x74 into each shape packet)
    const uint32_t fl = ld32(g_models[m.k].model + 0x74);
    st32(twin + 0x74, fl), guest_call(c, GC(0x027F596C), {twin, fl});
    const uint32_t r3 = c->r[3], r4 = c->r[4], r5 = c->r[5];
    if (m.param == 2) {
        guest_call(c, GC(0x02562F5C), {guest_call(c, GC(0x02555D0C)), twin, link + 0x110});  // setLightTevColorType
        c->r[3] = twin;
        f_025E2E5C_orig(c);
    } else {
        c->r[3] = link, c->r[4] = twin, c->r[5] = m.param;
        f_023D8DF0_orig(c);
    }
    c->r[3] = r3, c->r[4] = r4, c->r[5] = r5;
}
// Link's model just entered in his draw: taken into the pose, and each puppet's copy of it entered right after it
static void entered(Cpu* c, uint32_t link, uint32_t model, uint32_t param) {
    const int k = index_of(model);
    if (k < 0) return;
    if (g_want) take(k, model, param, link);
    for (int i = 0; i < kPuppets; i++)
        if (g_puppet[i].shown)
            for (const Model& m : g_puppet[i].pose.models) if (m.k == k) enter(c, link, i, m);
}
extern "C" void hook_023D8DF0(Cpu* c) {
    const uint32_t link = c->r[3], model = c->r[4], param = c->r[5];
    f_023D8DF0_orig(c);
    if (g_in_draw) entered(c, link, model, param);
}
extern "C" void f_023D8454_orig(Cpu* c);
extern "C" void hook_023D8454(Cpu* c) {
    const uint32_t link = c->r[3], model = c->r[4], param = c->r[5];
    f_023D8454_orig(c);
    if (g_in_draw) entered(c, link, model, param);
}

// mDoExt_modelEntryDL (025E2E5C) / modelUpdateDL (025E2DE0): Link's draw enters his body (+0x448) and a few others
// straight through these, without entryDLSetLight (param 2 in a pose: entered the same way)
extern "C" void hook_025E2E5C(Cpu* c) {
    const uint32_t model = c->r[3];
    f_025E2E5C_orig(c);
    if (g_in_draw) entered(c, g_link_drawing, model, 2);
}
extern "C" void f_025E2DE0_orig(Cpu* c);
extern "C" void hook_025E2DE0(Cpu* c) {
    const uint32_t model = c->r[3];
    f_025E2DE0_orig(c);
    if (g_in_draw) entered(c, g_link_drawing, model, 2);
}

// daPy_lk_c::draw: the pose taken; the puppets' models Link did not draw himself this time entered after him
extern "C" void f_023D9820_orig(Cpu* c);
extern "C" void hook_023D9820(Cpu* c) {
    const uint32_t link = c->r[3];
    if (g_want) g_taking = Pose{};
    for (uint64_t& e : g_entered) e = 0;
    g_in_draw = true, g_link_drawing = link;
    f_023D9820_orig(c);
    g_in_draw = false;
    const uint32_t ret = c->r[3];
    if (g_want && !g_taking.models.empty()) {
        memcpy(g_taking.stage, mem::ptr(kStage), 8);
        g_taking.room = (int8_t)ld8(link + kRoom);
        for (int i = 0; i < 3; i++) g_taking.pos[i] = f32(link + kPos + 4 * i);
        g_taking.yaw = (int16_t)ld16(link + kAngleY);
        g_taking.proc = (uint16_t)ld32(link + kCurProc), g_taking.equip = ld16(link + kEquip);
        g_pose = std::move(g_taking), g_have = true, g_want = false;
    }
    for (int i = 0; i < kPuppets; i++)
        if (g_puppet[i].shown)
            for (const Model& m : g_puppet[i].pose.models) enter(c, link, i, m);
    c->r[3] = ret;
}

