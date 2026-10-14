// Online co-op, the shared progress (coop_progress.h).
//
// The fields are dSv_save_c's (zeldaret/tww include/d/d_save.h, CC0; the same offsets as savestate.cpp's). A stage's
// memory bits are kept twice: the stage being played has them live in dSv_info_c::mMemory (put back into its slot of
// the table as it is left), the others in the table, so a stage's bits are read and written where they are now. A
// change goes as {u16 offset, u8 size, the field's new value, and for bit fields the bits that changed}; bit fields go
// byte by byte, so two players' changes to different bits of one byte both hold.
#include "coop_progress.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <string>
#include <vector>

#include "coop_net.h"
#include "guest_addr.h"
#include "mods.h"
#include "runtime.h"

namespace mods::coop::progress {
namespace {

const uint32_t kSaveInfoPtr = GD(0x101F84DC);  // dComIfGs save area, dSv_info_c at +0x20 (savestate.cpp)
const uint32_t kNextStage = GD(0x1046F0B0) + 0x5140;  // dStage_nextStage_c, enabled at +12
constexpr uint32_t kInfo = 0x20, kLive = 0x778, kStageNo = 0x79C, kDataNum = 0x1290;  // dSv_info_c: mMemory, mDan.mStageNo, mDataNum
constexpr uint16_t kTable = 0x380, kMemSize = 0x24, kStages = 16, kSaveSize = 0x778;
constexpr int kReplaced = 40;  // more fields changed in one step than play does: the save data was replaced (a load)

enum Mode : uint8_t { kBits, kSet, kMax, kBottle };  // kBottle: a new bottle is the others' as an empty one
struct Field { uint16_t off, size; Mode mode; };

std::vector<Field> fields() {
    std::vector<Field> f = {
        {0x000, 2, kMax},      // dSv_player_status_a_c: max life (heart containers and pieces)
        {0x00E, 3, kSet},      //   sword, shield, bracelets (mSelectEquip)
        {0x012, 1, kMax},      //   wallet size
        {0x013, 1, kMax},      //   max magic
        {0x051, 21, kBits},    // dSv_player_get_item_c
        {0x06E, 1, kMax}, {0x06F, 1, kMax}, {0x070, 1, kMax},  // dSv_player_item_max_c: arrows, bombs
        {0x0B4, 13, kBits},    // dSv_player_collect_c: collected items, songs, Triforce, pearls
        {0x0C4, 0x84, kBits},  // dSv_player_map_c: charts, sea map
        {0x5C0, 0x64, kBits},  // dSv_ocean_c: salvage
        {0x624, 0x100, kBits}, // dSv_event_c: the story
    };
    for (uint16_t i = 0; i < 21; i++)  // dSv_player_item_c, dInvSlot_* (bottles 14-17)
        f.push_back({(uint16_t)(0x03C + i), 1, i >= 14 && i <= 17 ? kBottle : kSet});
    for (uint16_t s = 0; s < kStages; s++) {  // dSv_memBit_c of each stage
        const uint16_t m = kTable + s * kMemSize;
        f.push_back({m, 4, kBits});                 // chests
        f.push_back({(uint16_t)(m + 0x04), 16, kBits});  // switches 0-127
        f.push_back({(uint16_t)(m + 0x14), 4, kBits});   // pickups
        f.push_back({(uint16_t)(m + 0x18), 8, kBits});   // visited rooms
        f.push_back({(uint16_t)(m + 0x20), 1, kSet});    // small keys
        f.push_back({(uint16_t)(m + 0x21), 1, kBits});   // map, compass, big key, boss beaten, heart taken, boss met
    }
    return f;
}
const std::vector<Field> kFields = fields();

std::vector<uint8_t> g_was;           // the save data as of the last step (empty: none yet)
uint8_t g_data_num = 0xFF;
std::deque<std::string> g_in;         // the others' changes not put in yet

// a byte of dSv_save_c where it is now (a played stage's memory bits: live)
uint32_t at(uint32_t info, uint16_t off) {
    if (off >= kTable && off < kTable + kStages * kMemSize) {
        const int8_t cur = (int8_t)ld8(info + kStageNo);
        if (cur >= 0 && cur < kStages && (off - kTable) / kMemSize == cur) return info + kLive + (off - kTable) % kMemSize;
    }
    return info + off;
}
void read(uint32_t info, std::vector<uint8_t>& out) {
    out.resize(kSaveSize);
    for (uint16_t o = 0; o < kSaveSize; o++) out[o] = ld8(at(info, o));
}
uint32_t value(const uint8_t* p, int n) {
    uint32_t v = 0;
    for (int i = 0; i < n; i++) v = v << 8 | p[i];
    return v;
}

// the others' changes in; out: the fields as they are now ("04A=50 66B=03"), for the trace
std::string put_in(uint32_t info, const std::string& m) {
    const auto* d = (const uint8_t*)m.data();
    std::string done;
    for (size_t i = 0; i + 3 <= m.size();) {
        const uint16_t off = (uint16_t)(d[i] | d[i + 1] << 8);
        const uint8_t size = d[i + 2];
        const Field* f = nullptr;
        for (const Field& k : kFields) if (off >= k.off && off + size <= k.off + k.size) { f = &k; break; }
        const size_t len = 3 + size * (f && f->mode == kBits ? 2 : 1);
        if (!f || i + len > m.size()) break;  // (the same build on both: not expected)
        const uint8_t* v = d + i + 3;
        if (f->mode == kBits) {
            for (int b = 0; b < size; b++) {
                const uint32_t a = at(info, off + b);
                st8(a, (uint8_t)((ld8(a) & ~v[size + b]) | (v[b] & v[size + b])));
            }
        } else if (f->mode == kBottle) {
            if (ld8(at(info, off)) == 0xFF && v[0] != 0xFF) st8(at(info, off), 0x50);  // dItemNo_EMPTY_BOTTLE_e
        } else {
            uint8_t cur[4];
            for (int b = 0; b < size; b++) cur[b] = ld8(at(info, off + b));
            if (f->mode == kSet || value(v, size) > value(cur, size))
                for (int b = 0; b < size; b++) st8(at(info, off + b), v[b]);
        }
        char t[16];
        snprintf(t, sizeof t, " %03X=%02X", off + size - 1, ld8(at(info, off + size - 1)));  // (its last byte)
        done += t, i += len;
    }
    return done;
}

}  // namespace

void received(const uint8_t* d, size_t n) {
    if (g_in.size() < 512) g_in.emplace_back((const char*)d, n);
}

void stop() { g_in.clear(), g_was.clear(); }

void step(bool live) {
    const uint32_t sv = ld32(kSaveInfoPtr);
    if (!live || !sv || ld8(kNextStage + 12)) {  // (a stage change moves the played stage's bits: compared after it)
        if (!live) g_was.clear();
        return;
    }
    const uint32_t info = sv + kInfo;
    // test aid: WWHD_COOP_TEST_PROGRESS=<offset>:<value>:<mask> (hex, dSv_save_c): those bits set here 300 steps after
    // arriving
    static unsigned t_off = 0, t_val = 0, t_mask = 0, t_steps = 0;
    static const bool t_on = getenv("WWHD_COOP_TEST_PROGRESS") &&
                             sscanf(getenv("WWHD_COOP_TEST_PROGRESS"), "%x:%x:%x", &t_off, &t_val, &t_mask) == 3 && t_off < kSaveSize;
    if (t_on && ++t_steps == 300) {
        const uint8_t was = ld8(at(info, t_off));
        st8(at(info, t_off), (uint8_t)((was & ~t_mask) | (t_val & t_mask)));
        mods::trace("coop: progress test: byte %03X from %02X to %02X", t_off, was, ld8(at(info, t_off)));
    }
    std::vector<uint8_t> now;
    read(info, now);
    if (g_was.empty() || ld8(info + kDataNum) != g_data_num) {  // the first step in the host's world, or another Quest Log
        g_was = now, g_data_num = ld8(info + kDataNum);
    }
    // this game's changes out
    std::string out, said;
    int changed = 0;
    for (const Field& f : kFields) {
        if (!memcmp(&now[f.off], &g_was[f.off], f.size)) continue;
        changed++;
        if (char t[8]; said.size() < 64) snprintf(t, sizeof t, " %03X", f.off), said += t;
        if (f.mode != kBits) {
            out += std::string{(char)(f.off & 0xFF), (char)(f.off >> 8), (char)f.size};
            out.append((const char*)&now[f.off], f.size);
            continue;
        }
        for (uint16_t b = f.off; b < f.off + f.size; b++)
            if (const uint8_t diff = now[b] ^ g_was[b])
                out += std::string{(char)(b & 0xFF), (char)(b >> 8), (char)1, (char)now[b], (char)diff};
    }
    if (changed > kReplaced) {
        mods::trace("coop: progress: %d fields changed at once (the save data replaced): not sent", changed);
    } else if (!out.empty()) {
        net::send(-1, net::kProgress, out);
        mods::trace("coop: progress out:%s", said.c_str());
    }
    // the others' in
    for (const std::string& m : g_in) mods::trace("coop: progress in:%s", put_in(info, m).c_str());
    g_in.clear();
    read(info, g_was);
}

}  // namespace mods::coop::progress
