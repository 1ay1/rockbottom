// procfs.hpp — Shared, side-effect-light helpers for reading /proc and /sys.
// Every collector includes this; nothing here holds state.

#pragma once

#include <array>
#include <cstdint>
#include <fstream>
#include <sstream>
#include <string>
#include <pwd.h>
#include <unistd.h>

namespace rockbottom::procfs {

// Read an entire file into a string. Empty string on failure (procfs files are
// small and virtual, so a full slurp is cheap and simplest).
inline std::string slurp(const char* path) {
    std::ifstream f(path);
    if (!f) return {};
    std::stringstream ss;
    ss << f.rdbuf();
    return ss.str();
}
inline std::string slurp(const std::string& path) { return slurp(path.c_str()); }

inline std::string first_line(const std::string& s) {
    auto nl = s.find('\n');
    return nl == std::string::npos ? s : s.substr(0, nl);
}

inline std::string trim(std::string s) {
    auto a = s.find_first_not_of(" \t\r\n");
    auto b = s.find_last_not_of(" \t\r\n");
    return a == std::string::npos ? std::string{} : s.substr(a, b - a + 1);
}

// Push a sample onto a fixed ring buffer, keeping the newest values
// left-packed (index 0 = oldest kept). `len` grows until it saturates at N.
template <std::size_t N>
inline void push_hist(std::array<float, N>& ring, int& len, float v) {
    if (len < static_cast<int>(N)) { ring[static_cast<std::size_t>(len++)] = v; return; }
    for (std::size_t i = 1; i < N; ++i) ring[i - 1] = ring[i];
    ring[N - 1] = v;
}

inline std::string user_of(uid_t uid) {
    if (passwd* pw = ::getpwuid(uid)) return pw->pw_name;
    return std::to_string(uid);
}

}  // namespace rockbottom::procfs
