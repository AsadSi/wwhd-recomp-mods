// More built-in mods (extras.cpp): listed in the Mods tab with the others (manager.cpp), some with a choice.
#pragma once
#include <cstdint>
#include <string>
#include <vector>

struct Cpu;
namespace input { struct PadState; }

namespace mods::extras {
struct Mod {
    std::string id, name, category, description;
    std::vector<std::string> choices;  // empty: a plain switch; else the first is the default
    bool often = false;                // also a How often setting (kOften): a spooky event now and then, or always
    std::string env = "WWHD_MOD_" + [](std::string s) {
        for (auto& ch : s) ch = ch == '-' ? '_' : (char)toupper(ch);
        return s;
    }(id);  // WWHD_MOD_<ID>=1 starts with it on
};
const std::vector<Mod>& catalogue();
const Mod* find(const std::string& id);
bool (*enabled_fn(const std::string& id))();
void (*apply_fn(const std::string& id))(bool);
int choice(const std::string& id);
void set_choice(const std::string& id, int choice);
extern const std::vector<std::string> kOften;  // Rare, Now and then (the default), Often, Always
int often(const std::string& id);
void set_often(const std::string& id, int often);
void step(Cpu* c);  // game thread, once per full logic step (interp.cpp)
void filter_pad(input::PadState& s);
void camera_draw(uint32_t cam);  // camera_draw (interp.cpp), before it draws: the Big ship helm's far camera  // each GamePad read (camera.cpp's filter_pad): the deck cannon's controls
}  // namespace mods::extras
