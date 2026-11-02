// The live probe (probe.h).
#include "probe.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <sstream>
#include <string>
#include <filesystem>
#include <system_error>

#include "runtime.h"

namespace mods::probe {
namespace {

std::map<std::string, uint32_t> g_names;

// guest memory the probe may touch (code, data, the heaps and the host's allocations below 0x70000000): anything
// else is refused, not a crash
bool ok(uint32_t a, uint32_t n = 4) { return a >= 0x02000000 && a < 0x70000000 && n <= 0x70000000 - a; }

// terms added with '+' / '-': hex, a name, [expr]
bool eval(const std::string& s, size_t& i, uint32_t& out) {
    out = 0;
    int sign = 1;
    while (i < s.size() && s[i] != ']') {
        uint32_t v = 0;
        if (s[i] == '[') {
            if (!eval(s, ++i, v) || i >= s.size()) return false;
            if (!ok(v)) return false;
            ++i, v = ld32(v);
        } else {
            size_t j = i;
            while (j < s.size() && (isalnum((unsigned char)s[j]) || s[j] == '_')) j++;
            const std::string t = s.substr(i, j - i);
            if (t.empty()) return false;
            if (const auto it = g_names.find(t); it != g_names.end()) v = it->second;
            else v = (uint32_t)strtoul(t.c_str(), nullptr, 16);
            i = j;
        }
        out += sign * v;
        if (i < s.size() && (s[i] == '+' || s[i] == '-')) sign = s[i++] == '-' ? -1 : 1;
    }
    return true;
}
bool eval(const std::string& s, uint32_t& out) {
    size_t i = 0;
    return eval(s, i, out);
}

std::string run(Cpu* c, const std::string& line) {
    std::istringstream in(line);
    std::string cmd, a;
    in >> cmd >> a;
    char b[160];
    std::string o;
    uint32_t at = 0;
    if (cmd == "names") {
        for (const auto& [n, v] : g_names) snprintf(b, sizeof b, "%s = %08X\n", n.c_str(), v), o += b;
        return o;
    }
    if (a.empty() || !eval(a, at)) return "? " + line + " (or an address outside guest memory)\n";
    if (cmd == "peek") {
        std::string n;
        in >> n;
        const uint32_t bytes = n.empty() ? 64 : (uint32_t)strtoul(n.c_str(), nullptr, 0);
        if (!ok(at, bytes)) return "? outside guest memory\n";
        for (uint32_t r = 0; r < bytes; r += 16) {
            snprintf(b, sizeof b, "%08X +%03X:", at + r, r), o += b;
            for (uint32_t w = 0; w < 16 && r + w < bytes; w += 4) snprintf(b, sizeof b, " %08X", ld32(at + r + w)), o += b;
            o += "  |";
            for (uint32_t w = 0; w < 16 && r + w < bytes; w += 4) snprintf(b, sizeof b, " %.4g", ldf32(at + r + w)), o += b;
            o += "\n";
        }
    } else if (cmd == "str") {
        if (!ok(at, 256)) return "? outside guest memory\n";
        for (uint32_t k = 0; k < 256 && ld8(at + k); k++) o += (char)ld8(at + k);
        o += "\n";
    } else if (cmd == "call") {
        uint32_t v[8] = {};
        for (int k = 0; k < 8 && in >> a; k++) if (!eval(a, v[k])) return "? " + a + "\n";
        const uint32_t r = guest_call(c, at, {v[0], v[1], v[2], v[3], v[4], v[5], v[6], v[7]});
        snprintf(b, sizeof b, "r3 = %08X\n", r), o = b;
    } else if (cmd == "poke") {
        uint32_t v = 0;
        if (!(in >> a) || !eval(a, v) || !ok(at)) return "? " + line + "\n";
        st32(at, v);
        snprintf(b, sizeof b, "%08X = %08X\n", at, v), o = b;
    } else {
        o = "? " + cmd + "\n";
    }
    return o;
}

}  // namespace

void name(const char* n, uint32_t value) { g_names[n] = value; }

uint32_t knob(const char* n) {
    const auto it = g_names.find(n);
    if (it != g_names.end()) return it->second;
    const uint32_t a = mem::host_alloc(4);
    st32(a, 0);
    return g_names[n] = a;
}

void step(Cpu* c, uint32_t link) {
    static const char* file = getenv("WWHD_PROBE");
    static long long seen = 0;
    if (!file) return;
    std::error_code failed;
    const long long t = std::filesystem::last_write_time(file, failed).time_since_epoch().count();
    if (failed) return;
    if (t == seen) return;
    seen = t;
    g_names["link"] = link;
    g_names["body"] = link ? ld32(link + 0x448) : 0;
    FILE* f = fopen(file, "r");
    if (!f) return;
    std::string o;
    char line[512];
    while (fgets(line, sizeof line, f)) {
        std::string l = line;
        while (!l.empty() && (l.back() == '\n' || l.back() == '\r')) l.pop_back();
        if (!l.empty()) o += "> " + l + "\n" + run(c, l);
    }
    fclose(f);
    const std::string out = std::string(file) + ".out";
    if (FILE* w = fopen((out + ".tmp").c_str(), "w")) {
        fputs(o.c_str(), w), fclose(w);
        rename((out + ".tmp").c_str(), out.c_str());
    }
}

}  // namespace mods::probe
