// core/config.hpp — CLI flags + persisted user preferences.
//
// One small struct carries the knobs rockbottom remembers between runs (sort
// column + direction, tree vs flat, refresh cadence, an optional startup
// filter). Precedence: built-in defaults < config file < CLI flags. On a clean
// exit the *current* view state is written back, so the tool reopens the way
// you left it — the thing every long-lived monitor should do and few do.
//
// The config file is a trivial key=value text file at
//   $XDG_CONFIG_HOME/rockbottom/config   (falls back to ~/.config/rockbottom/config)
// so it's inspectable and editable by hand.

#pragma once

#include "sampler.hpp"   // SortKey

#include <cstdlib>
#include <fstream>
#include <optional>
#include <sstream>
#include <string>
#include <sys/stat.h>

namespace rockbottom {

struct Config {
    SortKey     sort = SortKey::Cpu;
    bool        sort_desc = true;
    bool        tree = false;          // flat is the default view
    int         refresh_ms = 1000;
    std::string filter;                // startup filter query ("" = none)
    std::string theme = "native";      // active palette name (see ui/theme.hpp)
    bool        show_help_on_exit = false;   // never persisted; reserved

    // ── SortKey <-> name ──
    static const char* sort_name(SortKey k) {
        switch (k) {
            case SortKey::Cpu:  return "cpu";
            case SortKey::Mem:  return "mem";
            case SortKey::Io:   return "io";
            case SortKey::Pid:  return "pid";
            case SortKey::Name: return "name";
            case SortKey::Port: return "port";
        }
        return "cpu";
    }
    static std::optional<SortKey> parse_sort(const std::string& s) {
        if (s == "cpu")  return SortKey::Cpu;
        if (s == "mem" || s == "memory") return SortKey::Mem;
        if (s == "io" || s == "i/o" || s == "disk") return SortKey::Io;
        if (s == "pid")  return SortKey::Pid;
        if (s == "name") return SortKey::Name;
        if (s == "port") return SortKey::Port;
        return std::nullopt;
    }

    // ── file path ──
    static std::string dir_path() {
        const char* xdg = std::getenv("XDG_CONFIG_HOME");
        std::string base;
        if (xdg && *xdg) base = xdg;
        else {
            const char* home = std::getenv("HOME");
            base = std::string(home ? home : ".") + "/.config";
        }
        return base + "/rockbottom";
    }
    static std::string file_path() { return dir_path() + "/config"; }

    // ── load: defaults, then overlay the file if present ──
    static Config load() {
        Config c;
        std::ifstream f(file_path());
        if (!f) return c;
        std::string line;
        while (std::getline(f, line)) {
            auto hash = line.find('#');
            if (hash != std::string::npos) line.resize(hash);
            auto eq = line.find('=');
            if (eq == std::string::npos) continue;
            std::string key = trim(line.substr(0, eq));
            std::string val = trim(line.substr(eq + 1));
            if (key.empty()) continue;
            if (key == "sort") { if (auto s = parse_sort(val)) c.sort = *s; }
            else if (key == "sort_desc") c.sort_desc = (val == "1" || val == "true");
            else if (key == "tree")      c.tree = (val == "1" || val == "true");
            else if (key == "refresh_ms") { int v = std::atoi(val.c_str()); if (v >= 250 && v <= 5000) c.refresh_ms = v; }
            else if (key == "filter")    c.filter = val;
            else if (key == "theme")     c.theme = val;
        }
        return c;
    }

    // ── save: best-effort, never throws; creates the dir ──
    void save() const {
        ::mkdir(dir_path().c_str(), 0755);   // ok if it already exists
        std::ofstream f(file_path(), std::ios::trunc);
        if (!f) return;
        f << "# rockbottom preferences — edit freely; overwritten on clean exit\n";
        f << "sort=" << sort_name(sort) << "\n";
        f << "sort_desc=" << (sort_desc ? 1 : 0) << "\n";
        f << "tree=" << (tree ? 1 : 0) << "\n";
        f << "refresh_ms=" << refresh_ms << "\n";
        f << "filter=" << filter << "\n";
        f << "theme=" << theme << "\n";
    }

    // ── CLI parsing: returns false + fills `exit_msg` for --help/--version or
    //    a bad flag, so main() can print and exit. Flags override the file. ──
    static bool parse_args(int argc, char** argv, Config& c, std::string& exit_msg) {
        auto usage = [] {
            return std::string(
                "rockbottom — a calmer system monitor\n\n"
                "usage: rb [options]\n\n"
                "  --sort=KEY        cpu|mem|io|pid|name|port   (default: cpu)\n"
                "  --tree            open in the flow-tree view\n"
                "  --flat            open in the flat list view (default)\n"
                "  --refresh=MS      sample cadence 250..5000ms  (default: 1000)\n"
                "  --filter=QUERY    startup filter, e.g. --filter='cpu:>5'\n"
                "  --theme=NAME      color theme — 35 total (native, mocha, tokyo,\n"
                "                    dracula, gruvbox, nord, synthwave, matrix, …)\n"
                "  --no-config       ignore ~/.config/rockbottom/config this run\n"
                "  -h, --help        show this help and exit\n"
                "  -v, --version     show version and exit\n\n"
                "in-app: ? for the full key reference. state persists between runs.\n");
        };
        for (int i = 1; i < argc; ++i) {
            std::string a = argv[i];
            auto val = [&](const std::string& pfx) -> std::optional<std::string> {
                if (a.rfind(pfx, 0) == 0) return a.substr(pfx.size());
                return std::nullopt;
            };
            if (a == "-h" || a == "--help")    { exit_msg = usage(); return false; }
            if (a == "-v" || a == "--version") { exit_msg = "rockbottom 1.0\n"; return false; }
            if (a == "--tree") { c.tree = true; continue; }
            if (a == "--flat") { c.tree = false; continue; }
            if (a == "--no-config") { continue; }   // handled before load in main
            if (auto s = val("--sort=")) {
                if (auto k = parse_sort(*s)) c.sort = *k;
                else { exit_msg = "unknown sort key: " + *s + "\n" + usage(); return false; }
                continue;
            }
            if (auto r = val("--refresh=")) {
                int v = std::atoi(r->c_str());
                if (v < 250 || v > 5000) { exit_msg = "refresh must be 250..5000ms\n"; return false; }
                c.refresh_ms = v; continue;
            }
            if (auto q = val("--filter=")) { c.filter = *q; continue; }
            if (auto th = val("--theme=")) { c.theme = *th; continue; }
            exit_msg = "unknown option: " + a + "\n" + usage();
            return false;
        }
        return true;
    }

private:
    static std::string trim(std::string s) {
        auto a = s.find_first_not_of(" \t\r\n");
        if (a == std::string::npos) return "";
        auto b = s.find_last_not_of(" \t\r\n");
        return s.substr(a, b - a + 1);
    }
};

}  // namespace rockbottom
