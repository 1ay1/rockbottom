// platform/common/sys_util.hpp — OS-agnostic helpers shared by every backend.
//
// Nothing here touches a platform-specific API: string trimming, a ring-buffer
// push, uid→name resolution (POSIX), and a generic file slurp usable on any
// OS that exposes text files. Backend-specific readers (Linux /proc parsing,
// macOS sysctl/mach) live in their own platform directories and may build on
// top of these.

#pragma once

#include <array>
#include <cstddef>
#include <fstream>
#include <sstream>
#include <string>
#include <pwd.h>
#include <unistd.h>

namespace rockbottom::sys {

// Read an entire (small, virtual) file into a string. Empty on failure.
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

}  // namespace rockbottom::sys
