// Online co-op: Link's pose and the puppets (coop_pose.h).
//
// Link's models: daPy_lk_c::createHeap (023D5548) makes them all with mDoExt_J3DModel__create (025E38E0); right after
// it, on the same heap, each is made once more for every puppet (a model that does not fit is left out). Link's draw
// (023D9820) enters them through entryDLSetLight (023D8DF0) / updateDLSetLight (023D8454): there, once a logic step,
// each model's world joint matrices (model +0x2C -> block +0x10, count block +0x2C) are taken relative to Link's place
// and heading. Right after each of his models is entered, each puppet's copy of it gets its matrices from the puppet's
// pose at its own place and is entered the same way (Link as the light's owner), the ones he did not draw after his
// draw: his sword and shield, and no calc (Link's joint callbacks take any model they run on for his). What he holds
// (his drawn sword, an item) is made as he takes it out (daPy_lk_c::initModel, 023D4BB8, a file of Link's archive): a
// pose names its file, and each puppet gets its own copy of that file's model the first time it holds it, on a heap
// of its own, given back as Link is made again.
#include "coop_pose.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>

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
struct Made { uint16_t file; uint32_t diff; };
std::map<uint32_t, Made> g_link_files;  // Link's models by initModel: their files
struct Held { uint16_t file; uint32_t model, heap; };
std::vector<Held> g_held[kPuppets];  // each puppet's copies of what its player held
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

// a material's shown flag: the model data's (shared by Link's model and its copies), Link's draw sets it as he is
uint32_t shown_flag(uint32_t model, uint32_t i) { return ld32(ld32(ld32(model + 0xAC) + 0x10) + i * 0x39C + 8) + 4; }
uint32_t materials(uint32_t model) { return std::min<uint32_t>(ld32(ld32(model + 0xAC) + 0xC), 32); }

void rot_y(int16_t yaw, float& s, float& c) {
    const float a = yaw * (float)M_PI / 32768;
    s = std::sin(a), c = std::cos(a);
}

// a model's joints, relative to Link's place and heading (inverse of T(pos) * Ry(yaw), then the world matrix)
void take(int k, const Made* held, uint32_t model, uint32_t link) {
    const uint32_t jnt = ld32(model + kMdlJoints), mtx = jnt ? ld32(jnt + kJntMtx) : 0, n = jnt ? ld16(jnt + kJntNum) : 0;
    if (!mtx || !n || n > kMaxJoints) return;
    for (const Model& m : g_taking.models) if (held ? m.k == kHeld && m.file == held->file : m.k == k) return;  // (entered twice in one draw)
    float s, c;
    rot_y((int16_t)ld16(link + kAngleY), s, c);
    const float p[3] = {f32(link + kPos), f32(link + kPos + 4), f32(link + kPos + 8)};
    Model out;
    out.k = held ? kHeld : (uint8_t)k;
    if (held) out.file = held->file, out.diff = held->diff;
    for (uint32_t i = 0; i < materials(model); i++) if (!ld8(shown_flag(model, i))) out.shown &= ~(1u << i);
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

// puppet i's copy of a held model: made from its file of Link's archive (dComIfG_getObjectRes, 026066C4, as initModel
// asks) the first time, on a solid heap of its own from the game heap (a file that makes none is not tried again)
void make_held(Cpu* c, int i, const Model& m) {
    for (const Held& h : g_held[i]) if (h.file == m.file) return;
    static const uint32_t arc = [] {  // "Link", as initModel passes it (the text, then the string class's vtable)
        const uint32_t b = mem::host_alloc(8);
        st32(b, GD(0x101CEB48)), st32(b + 4, GD(0x10034B24));
        return b;
    }();
    const uint32_t data = guest_call(c, GC(0x026066C4), {ld32(GD(0x101F4F28)), arc, m.file});
    uint32_t heap = data ? guest_call(c, GC(0x025E3528), {0, 0}) : 0, model = 0;  // mDoExt_createSolidHeapFromGame
    if (heap) {
        const uint32_t was = guest_call(c, GC(0x027EC220), {heap});  // JKRHeap::becomeCurrentHeap
        model = guest_call(c, GC(0x025E38E0), {data, 0x80000, m.diff});
        guest_call(c, GC(0x027EC220), {was});
        guest_call(c, GC(0x025E3678), {heap});  // mDoExt_adjustSolidHeap
        if (!model) guest_call(c, GC(0x025E3868), {heap}), heap = 0;
    }
    g_held[i].push_back({m.file, model, heap});
    trace("coop: puppet %d holds file %u: %s", i, m.file, model ? "its copy made" : "no copy");
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
        put_bytes(&m.k, 1), put_bytes(&m.shown, 4), put_bytes(&nj, 1);
        if (m.k == kHeld) put_bytes(&m.file, 2), put_bytes(&m.diff, 4);
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
        if (!get(&m.k, 1) || !get(&m.shown, 4) || !get(&nj, 1)) return false;
        if (m.k == kHeld && (!get(&m.file, 2) || !get(&m.diff, 4))) return false;
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
            if (n.k != m.k || n.file != m.file || n.j.size() != m.j.size()) continue;
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

void step(Cpu* c, uint32_t link, bool on) {
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
    for (int i = 0; i < kPuppets; i++)
        if (g_puppet[i].shown)
            for (const Model& m : g_puppet[i].pose.models) if (m.k == kHeld) make_held(c, i, m);
}

}  // namespace mods::coop

using namespace mods::coop;

// daPy_lk_c::createHeap: Link's models, then a copy of each for every puppet on his heap
extern "C" void f_023D5548_orig(Cpu* c);
extern "C" void hook_023D5548(Cpu* c) {
    for (auto& held : g_held) {
        for (const Held& h : held) if (h.heap) guest_call(c, GC(0x025E3868), {h.heap});  // mDoExt_destroySolidHeap
        held.clear();
    }
    g_link_files.clear();
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

// daPy_lk_c::initModel(this, J3DModel**, file, differed-list flags): which file each of Link's models is
extern "C" void f_023D4BB8_orig(Cpu* c);
extern "C" void hook_023D4BB8(Cpu* c) {
    const uint32_t out = c->r[4], file = c->r[5], diff = c->r[6];
    f_023D4BB8_orig(c);
    if (const uint32_t model = ld32(out)) g_link_files[model] = {(uint16_t)file, diff};
}

extern "C" void f_023D8DF0_orig(Cpu* c);
extern "C" void f_025E2E5C_orig(Cpu* c);

// puppet i's copy of Link's model k, posed and entered as Link's own, each once a draw
static uint32_t g_link_drawing = 0;
static uint64_t g_entered[kPuppets];  // models entered this draw, by bit k
static void enter(Cpu* c, uint32_t link, int i, const Model& m) {
    const Puppet& u = g_puppet[i];
    uint32_t twin = 0;
    if (m.k == kHeld) {
        for (const Held& h : g_held[i]) if (h.file == m.file) twin = h.model;
    } else if (m.k < g_models.size() && m.k < 64 && !(g_entered[i] >> m.k & 1)) {
        g_entered[i] |= 1ull << m.k, twin = g_models[m.k].twin[i];
    }
    const uint32_t jnt = twin ? ld32(twin + kMdlJoints) : 0, mtx = jnt ? ld32(jnt + kJntMtx) : 0;
    if (!mtx || ld16(jnt + kJntNum) != m.j.size()) return;
    float s, co;
    rot_y(u.yaw, s, co);
    for (size_t j = 0; j < m.j.size(); j++) put(m.j[j], u.pos, s, co, mtx + 48 * (uint32_t)j);
    // its base transform: T(pos) * Ry
    const float base[12] = {co, 0, s, u.pos[0], 0, 1, 0, u.pos[1], -s, 0, co, u.pos[2]};
    for (int e = 0; e < 12; e++) st32(twin + kMdlBase + 4 * e, bits(base[e]));
    // its packets' draw flags as Link's model has them (027F596C: model +0x74 into each shape packet)
    if (m.k != kHeld) {
        const uint32_t fl = ld32(g_models[m.k].model + 0x74);
        st32(twin + 0x74, fl), guest_call(c, GC(0x027F596C), {twin, fl});
    }
    // its materials shown as that player's were (his sword drawn: the blade), Link's put back after
    const uint32_t n = materials(twin);
    uint8_t was[32];
    for (uint32_t k = 0; k < n; k++) was[k] = ld8(shown_flag(twin, k)), st8(shown_flag(twin, k), m.shown >> k & 1);
    // entryDLSetLight(Link, twin, 0): lit as Link, entered into the draw buffers current now, Link's. (Its other way,
    // 0259130C, enters into the actors' buffers, drawn after the sea and in the sea's state: the copies drew in one blue.)
    const uint32_t r3 = c->r[3], r4 = c->r[4], r5 = c->r[5];
    c->r[3] = link, c->r[4] = twin, c->r[5] = 0;
    f_023D8DF0_orig(c);
    c->r[3] = r3, c->r[4] = r4, c->r[5] = r5;
    for (uint32_t k = 0; k < n; k++) st8(shown_flag(twin, k), was[k]);
}
// Link's model just entered in his draw: taken into the pose, and each puppet's copy of it entered right after it
static void entered(Cpu* c, uint32_t link, uint32_t model) {
    const int k = index_of(model);
    if (k < 0) {
        if (const auto it = g_link_files.find(model); g_want && it != g_link_files.end()) take(-1, &it->second, model, link);
        return;
    }
    if (g_want) take(k, nullptr, model, link);
    for (int i = 0; i < kPuppets; i++)
        if (g_puppet[i].shown)
            for (const Model& m : g_puppet[i].pose.models) if (m.k == k) enter(c, link, i, m);
}
extern "C" void hook_023D8DF0(Cpu* c) {
    const uint32_t link = c->r[3], model = c->r[4];
    f_023D8DF0_orig(c);
    if (g_in_draw) entered(c, link, model);
}
extern "C" void f_023D8454_orig(Cpu* c);
extern "C" void hook_023D8454(Cpu* c) {
    const uint32_t link = c->r[3], model = c->r[4];
    f_023D8454_orig(c);
    if (g_in_draw) entered(c, link, model);
}

// mDoExt_modelEntryDL (025E2E5C) / modelUpdateDL (025E2DE0): Link's draw enters his body (+0x448) and a few others
// straight through these, without entryDLSetLight
extern "C" void hook_025E2E5C(Cpu* c) {
    const uint32_t model = c->r[3];
    f_025E2E5C_orig(c);
    if (g_in_draw) entered(c, g_link_drawing, model);
}
extern "C" void f_025E2DE0_orig(Cpu* c);
extern "C" void hook_025E2DE0(Cpu* c) {
    const uint32_t model = c->r[3];
    f_025E2DE0_orig(c);
    if (g_in_draw) entered(c, g_link_drawing, model);
}

// J3DModel's entry of one material's shapes (027F583C): Link's draw enters his face (eyes, brows, mouth over his hair)
// this way (before his body), into its own buffers; each puppet's copy of that model the same
extern "C" void f_027F583C_orig(Cpu* c);
extern "C" void hook_027F583C(Cpu* c) {
    const uint32_t model = c->r[3], mat = c->r[4];
    f_027F583C_orig(c);
    const int k = g_in_draw ? index_of(model) : -1;
    if (k < 0 || k >= 64) return;
    const uint32_t r3 = c->r[3], r4 = c->r[4];
    for (int i = 0; i < kPuppets; i++)
        if (g_puppet[i].shown && g_models[k].twin[i])
            for (const Model& m : g_puppet[i].pose.models)
                if (m.k == k) c->r[3] = g_models[k].twin[i], c->r[4] = mat, f_027F583C_orig(c);
    c->r[3] = r3, c->r[4] = r4;
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
