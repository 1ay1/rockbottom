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
#include <cstdio>
#include <cstdlib>
#include <cstring>
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

// Push a sample onto TWO parallel rings that share one length counter (e.g.
// rx+tx, read+write). Advancing them together keeps the two series index-
// aligned; doing it as two separate hand-shifts injects a stale zero into the
// second ring during the growth phase (the shift reads an index the first
// push already vacated). One counter, one shift, both newest at len-1.
template <std::size_t N>
inline void push_hist2(std::array<float, N>& a, std::array<float, N>& b,
                       int& len, float va, float vb) {
    if (len < static_cast<int>(N)) {
        a[static_cast<std::size_t>(len)] = va;
        b[static_cast<std::size_t>(len)] = vb;
        ++len;
        return;
    }
    for (std::size_t i = 1; i < N; ++i) { a[i - 1] = a[i]; b[i - 1] = b[i]; }
    a[N - 1] = va;
    b[N - 1] = vb;
}

// uid → login name.
//
// STATIC-LINKING HAZARD, and the reason this is not a one-line getpwuid call.
// `rb` ships as a fully static binary so it runs on any Linux regardless of
// the host's glibc version. But glibc resolves users through NSS, which
// dlopen()s libnss_* at runtime — and a static binary has no dynamic loader,
// so getpwuid() can only ever see /etc/passwd. On an LDAP/AD/SSSD host (where
// every interactive account is a directory account and /etc/passwd holds only
// system users) that means every process would display a bare numeric uid.
// "1000" instead of "ayush" is a legibility regression, and a silent one.
//
// So: try getpwuid first — correct and complete on a normally-linked build,
// and on a static build it still resolves every local/system account. Only if
// that misses do we ask the SYSTEM's own dynamically-linked `getent`, which
// loads the NSS stack we cannot, and therefore does see directory accounts.
// The uid→name result is cached by the caller (Sampler::uid_cache_), so this
// costs at most one fork per distinct uid per run, not one per sample tick.
//
// Returns the uid as a decimal string if even getent can't name it — an
// unresolvable uid is a real state (deleted account, unmapped container id),
// not an error worth surfacing.
inline std::string user_of(uid_t uid) {
    if (passwd* pw = ::getpwuid(uid)) return pw->pw_name;

#ifndef _WIN32
    // getent is in the base install of every distro we target (glibc's own
    // package on Debian/Ubuntu/RHEL/SUSE, busybox on Alpine). Absent it, we
    // fall through to the numeric form rather than failing.
    char cmd[64];
    std::snprintf(cmd, sizeof cmd, "getent passwd %lu 2>/dev/null",
                  static_cast<unsigned long>(uid));
    if (FILE* p = ::popen(cmd, "r")) {
        char line[512];
        std::string name;
        if (std::fgets(line, sizeof line, p)) {
            // passwd format: name:passwd:uid:gid:gecos:dir:shell — take field 1.
            if (const char* colon = std::strchr(line, ':'))
                name.assign(line, static_cast<std::size_t>(colon - line));
        }
        ::pclose(p);
        if (!name.empty()) return name;
    }
#endif

    return std::to_string(uid);
}

}  // namespace rockbottom::sys
