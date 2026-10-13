// Online co-op, the session (coop_sync.h).
//
// Each other player's poses are kept by the time they were taken on their game (its clock), and shown 100 ms behind
// the newest: between the two around that time, carried on along the last move for at most 250 ms when the next is
// late, then held. That player's clock against ours is the smallest gap seen, creeping up a little with each pose
// (so a slower link is followed, a faster one at once). The host's ship and monsters are shown the same way
// (host_state). A guest takes the host's world at its join: the host's portable state (its save data, stage and
// place; savestate.cpp), sent in pieces and loaded into the Quest Log the guest plays; the guest's saves are blocked at
// the file system from then (hle/fs.cpp) and it goes back to the title screen when the host is gone (or it leaves).
#include "coop_sync.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <memory>
#include <mutex>

#include "coop_pose.h"
#include "extras.h"
#include "guest_addr.h"
#include "mods.h"
#include "runtime.h"
#include "../overlay/hostui.h"
#include "../portable_state.h"
#include "../savestate.h"
#include "../screenshot.h"

namespace hlefs { extern std::atomic<bool> g_block_saves; }

namespace mods::coop::sync {
namespace {

const uint32_t kPlay = GD(0x1046F0B0), kStage = kPlay + 0x5134;
constexpr uint32_t kPos = 0x314, kRoom = 0x326, kAngleY = 0x32A;
constexpr int32_t kBehind = 100, kReach = 250, kStale = 500;  // a player with no pose for kStale is loading or away
constexpr size_t kChunk = 1000;

std::mutex g_mu;  // the settings (the menu writes them on the main thread)
Settings g_set;
bool g_set_read = false;
uint32_t g_set_gen = 1;
std::string g_status = "Off";

struct Clock {  // another game's clock: ours less theirs
    int32_t offset = 0;
    bool known = false;
    void seen(uint32_t theirs, uint32_t ours) {
        const int32_t gap = (int32_t)(ours - theirs);
        offset = !known || gap < offset ? gap : offset + 1, known = true;
    }
    uint32_t shown(uint32_t ours) const { return ours - offset - kBehind; }  // their time we show now
};
struct Remote {
    Clock clock;
    uint32_t heard = 0, gaps = 0, gap_sum = 0, gap_max = 0;  // poses' arrivals (the trace)
    size_t size = 0;
    std::deque<std::pair<uint32_t, Pose>> poses;  // by their time
    bool shown = false;
    float at[3] = {};
    int16_t yaw = 0;
};
Remote g_remote[net::kMaxPlayers];
std::deque<std::pair<uint32_t, std::string>> g_host[4];  // the host's states by kind, by its time
std::vector<WorldMsg> g_world;
struct Effects { int from; uint32_t at; std::string body; };
std::vector<Effects> g_effects_in;  // the others' particles, started at their puppets as those are shown (kBehind)

std::vector<int> g_send_join;    // host: guests the world is still to go to
std::string g_join;              // guest: the host's portable state as it comes
uint32_t g_join_total = 0;
uint32_t g_join_link = 0;        // guest: Link as the load was asked for (a new one: arrived)
bool g_deck_drop = false;
float g_deck[3];
bool g_was_guest = false;
uint32_t g_gen = 0, g_failed = 0;  // the settings the session runs with; ones it gave up on
std::string g_back;              // guest: why it goes back to the title (set once the host is gone)

float f32(uint32_t a) { return (float)ldf32(a); }
uint32_t now_ms_since(uint32_t t) { return net::now_ms() - t; }
std::string stage_now() { return std::string((const char*)mem::ptr(kStage), strnlen((const char*)mem::ptr(kStage), 8)); }

// a ship's frame (extras.cpp's to_local / to_world: local +x is (cos a, -sin a), +z (sin a, cos a))
void to_world(uint32_t hull, const float l[3], float w[3]) {
    const double t = (int16_t)ld16(hull + kAngleY) * M_PI / 32768;
    w[0] = (float)(f32(hull + kPos) + l[0] * cos(t) + l[2] * sin(t));
    w[1] = f32(hull + kPos + 4) + l[1];
    w[2] = (float)(f32(hull + kPos + 8) - l[0] * sin(t) + l[2] * cos(t));
}
void to_local(uint32_t hull, const float w[3], float l[3]) {
    const double t = (int16_t)ld16(hull + kAngleY) * M_PI / 32768;
    const double dx = w[0] - f32(hull + kPos), dz = w[2] - f32(hull + kPos + 8);
    l[0] = (float)(dx * cos(t) - dz * sin(t)), l[1] = w[1] - f32(hull + kPos + 4), l[2] = (float)(dx * sin(t) + dz * cos(t));
}

bool same_place(const Pose& a, const Pose& b) {
    const float d = std::hypot(a.pos[0] - b.pos[0], a.pos[2] - b.pos[2]);
    return !memcmp(a.stage, b.stage, 8) && a.flags == b.flags && d < 500;
}

// the pose shown at their time `rt`
bool pose_at(const Remote& r, uint32_t rt, Pose& out) {
    const auto& q = r.poses;
    if (q.empty()) return false;
    if ((int32_t)(rt - q.front().first) <= 0) return out = q.front().second, true;
    for (size_t i = 1; i < q.size(); i++)
        if ((int32_t)(rt - q[i].first) <= 0) {
            const auto &a = q[i - 1], &b = q[i];
            if (same_place(a.second, b.second)) blend(a.second, b.second, (float)(rt - a.first) / (float)(b.first - a.first), out);
            else out = b.second;  // a stage change or a jump: there at once
            return true;
        }
    out = q.back().second;  // late: on along its last move for a while, then held
    if (q.size() >= 2 && same_place(q[q.size() - 2].second, q.back().second)) {
        const auto &a = q[q.size() - 2], &b = q.back();
        const float k = (float)std::min<int32_t>((int32_t)(rt - b.first), kReach) / (float)std::max<int32_t>(1, (int32_t)(b.first - a.first));
        for (int i = 0; i < 3; i++) out.pos[i] += (b.second.pos[i] - a.second.pos[i]) * k;
    }
    return true;
}

std::atomic<bool> g_hud_hidden{false};
uint32_t g_hud_link = 0;  // Link when it was hidden: shown again once there is another (the guest plays again)
bool g_to_title = false;  // asked for at the play scene's next execute (below)
void back_to_title(const std::string& why) {
    ss::notice(why);
    mods::trace("coop: back to the title: %s", why.c_str());
    g_to_title = true, g_hud_hidden = true, g_hud_link = ld32(kPlay + 0x5B34);
}

struct Handler : net::Handler {
    void message(int from, net::Msg type, const uint8_t* d, size_t n) override {
        if (type == net::kEffect) return g_effects_in.push_back({from, net::now_ms(), std::string((const char*)d, n)});
        if (type != net::kJoinState) {
            g_world.push_back({from, type, std::string((const char*)d, n)});
            return;
        }
        uint32_t total, at;
        if (n < 8) return;
        memcpy(&total, d, 4), memcpy(&at, d + 4, 4);
        if (at == 0) g_join.clear(), g_join_total = total;
        if (at != g_join.size() || total != g_join_total || total > pstate::kMaxFileSize) return;
        g_join.append((const char*)d + 8, n - 8);
        if (g_join.size() < g_join_total) return;
        auto s = std::make_shared<pstate::State>();
        std::string why;
        if (!pstate::read(g_join, *s, why)) {
            mods::trace("coop: the host's world does not load: %s", why.c_str());
            ss::notice("Can't load the host's world (" + why + ")");
            return;
        }
        // set down beside the host; on Tetra's ship's deck, there once this game has made her (take_deck_drop)
        Pose hp;
        const Remote& h = g_remote[0];
        g_deck_drop = pose_at(h, h.clock.shown(net::now_ms()), hp) && (hp.flags & 1);
        if (g_deck_drop) g_deck[0] = hp.pos[0] + 120, g_deck[1] = hp.pos[1], g_deck[2] = hp.pos[2];
        s->pos[0] += 120;
        g_join_link = ld32(GD(0x1046F0B0) + 0x5B34);
        hlefs::g_block_saves = true;
        ss::request_load_portable(s);
        mods::trace("coop: the host's world (%zu bytes): %s room %d at %.0f %.0f %.0f%s", g_join.size(), s->stage.c_str(), s->room,
                    s->pos[0], s->pos[1], s->pos[2], g_deck_drop ? ", the host on the ship's deck" : "");
    }
    void state(int from, uint8_t kind, uint32_t ms, const uint8_t* d, size_t n) override {
        Remote& r = g_remote[from];
        r.clock.seen(ms, net::now_ms());
        if (kind == kPose) {
            Pose p;
            if (!unpack(d, n, p)) return;
            if (const uint32_t now = net::now_ms(), gap = now - r.heard; r.heard)
                r.gaps++, r.gap_sum += gap, r.gap_max = std::max(r.gap_max, gap), r.heard = now;
            else r.heard = now;
            r.size = n;
            if (!r.poses.empty() && (int32_t)(ms - r.poses.back().first) <= 0) return;  // (out of order: dropped)
            // that player came into this game's stage or left it: told by name
            const std::string mine = stage_now(), now_at(p.stage, strnlen(p.stage, 8));
            if (const std::string was = r.poses.empty() ? now_at : std::string(r.poses.back().second.stage, strnlen(r.poses.back().second.stage, 8));
                was != now_at && !mine.empty()) {
                if (now_at == mine) ss::notice(net::name(from) + " is back");
                else if (was == mine) ss::notice(net::name(from) + (now_at == "sea" ? " went out to sea" : " went inside"));
            }
            r.poses.emplace_back(ms, std::move(p));
            while (r.poses.size() > 2 && (int32_t)(ms - r.poses.front().first) > 1000) r.poses.pop_front();
        } else if (from == 0 && kind < 4) {
            auto& q = g_host[kind];
            if (!q.empty() && (int32_t)(ms - q.back().first) <= 0) return;
            q.emplace_back(ms, std::string((const char*)d, n));
            while (q.size() > 2 && (int32_t)(ms - q.front().first) > 1000) q.pop_front();
        }
    }
    void joined(int id) override {
        ss::notice(net::name(id) + " joined (" + kTunics[net::tunic(id)] + ")");
        if (net::role() == net::Role::kHost) g_send_join.push_back(id);
    }
    void left(int id, const char* why) override {
        g_remote[id] = Remote{};
        show(id < net::my_id() ? id : std::max(0, id - 1), nullptr, nullptr, 0);
        if (id != 0 || net::role() != net::Role::kOff) return ss::notice(net::name(id) + " left");
        g_failed = g_gen;  // (not joined again until the toggle or the settings change)
        if (g_was_guest) g_back = why;
        else ss::notice(std::string("Online co-op: ") + why);
    }
} g_handler;

}  // namespace

Settings settings() {
    std::lock_guard<std::mutex> lk(g_mu);
    if (!g_set_read) {
        g_set_read = true;
        std::string v;
        if (hostui::get("mod.coop.choice", v)) g_set.role = atoi(v.c_str()) == 1;
        if (hostui::get("mod.coop.addr", v) && !v.empty()) g_set.addr = v;
        if (hostui::get("mod.coop.port", v) && atoi(v.c_str()) > 0) g_set.port = atoi(v.c_str());
        if (hostui::get("mod.coop.name", v) && !v.empty()) g_set.name = v;
        if (hostui::get("mod.coop.tunic", v)) g_set.tunic = atoi(v.c_str()) & 3;
        // (tests: WWHD_COOP=host[:port] or join:<address>[:port], WWHD_COOP_NAME, WWHD_COOP_TUNIC 0-3)
        if (const char* e = getenv("WWHD_COOP")) {
            std::string s = e;
            g_set.role = s.rfind("join:", 0) == 0;
            if (g_set.role) s = s.substr(5), g_set.addr = s.substr(0, s.find(':'));
            else s = s.substr(std::min(s.size(), (size_t)4));
            if (const size_t colon = s.find(':'); colon != std::string::npos) g_set.port = atoi(s.c_str() + colon + 1);
        }
        if (const char* e = getenv("WWHD_COOP_NAME")) g_set.name = e;
        if (const char* e = getenv("WWHD_COOP_TUNIC")) g_set.tunic = atoi(e) & 3;
        if (!getenv("WWHD_COOP_TUNIC") && !hostui::get("mod.coop.tunic", v)) g_set.tunic = g_set.role;  // host green, a guest red
    }
    return g_set;
}

void set_settings(const Settings& s) {
    settings();
    std::lock_guard<std::mutex> lk(g_mu);
    g_set = s, g_set_gen++;
}

const char* status() { return g_status.c_str(); }
bool host() { return net::role() == net::Role::kHost; }
bool guest() { return net::role() == net::Role::kGuest && net::my_id() != 0; }

bool place(int id, float out[3]) {
    if (id < 0 || id >= net::kMaxPlayers || !g_remote[id].shown) return false;
    memcpy(out, g_remote[id].at, 12);
    return true;
}

void nearest(const float at[3], uint32_t link, int& target, float out[3]) {
    const int me = net::role() == net::Role::kOff ? 0 : net::my_id();
    auto where = [&](int id, float p[3]) {
        if (id == me) return p[0] = f32(link + kPos), p[1] = f32(link + kPos + 4), p[2] = f32(link + kPos + 8), true;
        return id >= 0 && id < net::kMaxPlayers && g_remote[id].shown && (memcpy(p, g_remote[id].at, 12), true);
    };
    auto dist = [&](const float p[3]) { return std::hypot(p[0] - at[0], p[2] - at[2]); };
    float p[3], best[3];
    int pick = -1;
    for (int id = 0; id < net::kMaxPlayers; id++)
        if (where(id, p) && (pick < 0 || dist(p) < dist(best))) pick = id, memcpy(best, p, 12);
    if (pick < 0) pick = me, where(me, best);
    if (float cur[3]; target != pick && where(target, cur) && dist(best) > 0.7f * dist(cur)) pick = target, memcpy(best, cur, 12);
    target = pick;
    memcpy(out, best, 12);
}

bool arrived() { return host() || (guest() && g_join_link && ld32(kPlay + 0x5B34) != g_join_link); }

bool take_deck_drop(float local[3]) {
    if (!g_deck_drop || ld32(kPlay + 0x5B34) == g_join_link) return false;
    g_deck_drop = false;
    memcpy(local, g_deck, 12);
    return true;
}

std::vector<WorldMsg> take_world() { return std::move(g_world); }

bool host_state(Kind k, std::string& a, std::string& b, float& t) {
    const auto& q = g_host[k];
    if (q.empty()) return false;
    const uint32_t rt = g_remote[0].clock.shown(net::now_ms());
    a = b = q.back().second, t = 0;
    for (size_t i = 1; i < q.size(); i++)
        if ((int32_t)(rt - q[i].first) <= 0) {
            a = q[i - 1].second, b = q[i].second;
            t = std::clamp((float)(int32_t)(rt - q[i - 1].first) / (float)(q[i].first - q[i - 1].first), 0.f, 1.f);
            return true;
        }
    if (q.size() >= 2) {  // late: on past the newest for a while (t over 1)
        const auto &p = q[q.size() - 2], &n = q.back();
        a = p.second, t = 1 + (float)std::min<int32_t>((int32_t)(rt - n.first), kReach) / (float)std::max<int32_t>(1, (int32_t)(n.first - p.first));
    }
    return true;
}

void poll(Cpu* c, bool on) {
    uint32_t &gen = g_gen, &failed = g_failed;
    Settings s = settings();
    if (!getenv("WWHD_COOP")) s.role = mods::extras::choice("coop") == 1;  // Host / Join in the menu
    uint32_t want_gen;
    {
        std::lock_guard<std::mutex> lk(g_mu);
        static int role = -1;
        if (role != s.role) role = s.role, g_set_gen++;
        want_gen = g_set_gen;
    }
    // test aid: WWHD_COOP_SHOT=<file>: a screenshot when that file appears (a test driver touches it), then it is removed
    if (static const char* shot = getenv("WWHD_COOP_SHOT"); shot && remove(shot) == 0) screenshot::request(), mods::trace("coop: shot");
    const std::string stage = stage_now();
    const bool title = stage.empty() || stage == "sea_T" || stage == "Name";
    if (static bool was = true; title != was && (was = title)) mods::trace("coop: at the title");
    if (title && net::role() == net::Role::kOff) hlefs::g_block_saves = false;  // the guest's own Quest Logs again
    if (net::role() != net::Role::kOff && (!on || gen != want_gen)) {  // switched off, or other settings
        const bool was = guest();
        net::stop(nullptr);
        for (Remote& r : g_remote) r = Remote{};
        for (int i = 0; i < kPuppets; i++) show(i, nullptr, nullptr, 0);
        if (was) g_back = "You left the host's game";
    }
    if (!g_back.empty()) {
        back_to_title(g_back);
        g_back.clear(), g_was_guest = false, g_deck_drop = false;
    }
    if (on && net::role() == net::Role::kOff && failed != want_gen) {
        gen = want_gen;
        std::string why;
        if (!net::start(s.role ? net::Role::kGuest : net::Role::kHost, s.addr, s.port, s.name, s.tunic, why)) {
            failed = want_gen;
            g_status = "Not started: " + why;
            ss::notice("Online co-op: " + why);
        }
    }
    if (!on && net::role() == net::Role::kOff && g_status.rfind("Not started", 0) != 0) g_status = "Off";
    net::poll(g_handler);
    if (static bool told = false; !arrived()) told = false;
    else if (guest() && !told) told = true, net::send(0, net::kArrived, std::string());
    own_tunic(host() ? net::tunic(0) : guest() ? net::tunic(net::my_id()) : -1);
    if (guest() && !g_was_guest && net::tunic(net::my_id()) != s.tunic)
        ss::notice(std::string(kTunics[s.tunic]) + " was taken, you're " + kTunics[net::tunic(net::my_id())]);
    if (guest()) g_was_guest = true;
    if (net::role() != net::Role::kOff)
        g_status = std::string(net::status()) + (guest() && g_join_total && g_join.size() < g_join_total ? ", loading the host's world" : "");
    else if (!g_back.empty() || failed != want_gen) g_status = net::status();
}

bool hud_hidden() { return g_hud_hidden.load(std::memory_order_relaxed); }

void step(Cpu* c, uint32_t link, uint32_t hull) {
    if (link != g_hud_link) g_hud_hidden = false;  // (playing again)
    if (net::role() == net::Role::kOff) return;
    // this game's pose out (on Tetra's ship's deck: in her frame)
    Pose me;
    const float at[3] = {f32(link + kPos), f32(link + kPos + 4), f32(link + kPos + 8)};
    float l[3] = {};
    if (hull) to_local(hull, at, l);
    const bool on_deck = hull && std::fabs(l[0]) < 720 && l[2] > -1800 && l[2] < 2250 && l[1] > 370 && l[1] < 2500;  // (extras' on_deck)
    if (own_pose(me)) {
        if (on_deck) {
            to_local(hull, me.pos, me.pos);
            me.yaw = (int16_t)(me.yaw - (int16_t)ld16(hull + kAngleY)), me.flags |= 1;
        }
        std::string b;
        pack(me, b);
        net::put_state(kPose, b);
    }
    if (const std::vector<Effect> fx = take_effects(); !fx.empty())
        net::send(-1, net::kEffect, std::string((const char*)fx.data(), fx.size() * sizeof(Effect)));
    // the host: its world to each guest that has just joined (once Link is free to have it taken)
    for (size_t i = 0; i < g_send_join.size();) {
        std::string text, why;
        if (!ss::capture_portable_text(c, text, why)) {
            if (static uint32_t told = 0; told != g_send_join[i]) told = g_send_join[i], mods::trace("coop: the world waits to be sent: %s", why.c_str());
            break;
        }
        const uint32_t total = (uint32_t)text.size();
        for (uint32_t at = 0; at < total; at += kChunk) {
            std::string b(8, '\0');
            memcpy(&b[0], &total, 4), memcpy(&b[4], &at, 4);
            b += text.substr(at, kChunk);
            net::send(g_send_join[i], net::kJoinState, b);
        }
        mods::trace("coop: the world sent to player %d (%u bytes)", g_send_join[i], total);
        g_send_join.erase(g_send_join.begin() + i);
    }
    if (static uint32_t traced = 0; now_ms_since(traced) >= 5000) {  // how the link goes (the trace)
        traced = net::now_ms();
        for (int id = 0; id < net::kMaxPlayers; id++)
            if (Remote& r = g_remote[id]; id != net::my_id() && r.gaps) {
                mods::trace("coop: player %d: ping %d ms, a pose every %u ms (longest gap %u), %zu bytes, shown %d ms behind", id,
                            net::ping_ms(id), r.gap_sum / r.gaps, r.gap_max, r.size, kBehind);
                r.gaps = r.gap_sum = r.gap_max = 0;
            }
    }
    // the others, each where its pose was taken (in the same stage and room; on the ship's deck, wherever she is here)
    const std::string stage = stage_now();
    const int8_t room = (int8_t)ld8(link + kRoom);
    const uint32_t now = net::now_ms();
    for (int id = 0; id < net::kMaxPlayers; id++) {
        if (id == net::my_id()) continue;
        const int slot = id < net::my_id() ? id : id - 1;
        Remote& r = g_remote[id];
        Pose p;
        const bool was = r.shown;
        r.shown = net::connected(id) && now - r.heard < (uint32_t)kStale && pose_at(r, r.clock.shown(now), p) && !strncmp(p.stage, stage.c_str(), 8) &&
                  ((p.flags & 1) ? hull != 0 : p.room == room);
        if (!r.shown) {
            if (was) show(slot, nullptr, nullptr, 0);  // (only the ones it showed: the ghost test aid has slot 0 to itself)
            continue;
        }
        int16_t yaw = p.yaw;
        if (p.flags & 1) to_world(hull, p.pos, r.at), yaw = (int16_t)(yaw + (int16_t)ld16(hull + kAngleY));
        else memcpy(r.at, p.pos, 12);
        r.yaw = yaw;
        show(slot, &p, r.at, yaw, net::tunic(id));
    }
    // their particles, as their puppets are where they were made (dropped for a player not shown here)
    for (size_t i = 0; i < g_effects_in.size();) {
        const Effects& e = g_effects_in[i];
        if (now - e.at < (uint32_t)kBehind) { i++; continue; }
        if (e.from >= 0 && e.from < net::kMaxPlayers && g_remote[e.from].shown)
            for (size_t o = 0; o + sizeof(Effect) <= e.body.size(); o += sizeof(Effect)) {
                Effect fx;
                memcpy(&fx, e.body.data() + o, sizeof fx);
                effect_at(c, fx, g_remote[e.from].at, g_remote[e.from].yaw);
            }
        g_effects_in.erase(g_effects_in.begin() + i);
    }
}

}  // namespace mods::coop::sync

// dComIfG_resetToOpening (025202F8, this scene): every scene's execute asks it each step whether the reset button
// sends the game back to the opening; a guest's way back to the title is taken here, as that reset is:
// changeOpeningScene(this scene, the opening), the music faded and the sound stopped (asked for from the mods' step,
// in the middle of the actors' executes, the scene change request crashed)
extern "C" void f_025202F8_orig(Cpu* c);
extern "C" void hook_025202F8(Cpu* c) {
    if (!mods::coop::sync::g_to_title) return f_025202F8_orig(c);
    mods::coop::sync::g_to_title = false;
    const uint32_t scene = c->r[3];
    guest_call(c, GC(0x02520230), {scene, 8});
    guest_call(c, GC(0x025E1904), {0x1E});
    guest_call(c, GC(0x025E1D40));
    mods::trace("coop: the opening asked for (scene proc name %d)", (int16_t)ld16(scene + 8));
    c->r[3] = 1;
}
