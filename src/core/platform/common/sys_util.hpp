// platform/common/sys_util.hpp — OS-agnostic helpers shared by every backend.
//
// Nothing here touches a platform-specific API: string trimming, a ring-buffer
// push, uid→name resolution (POSIX), and a generic file slurp usable on any
// OS that exposes text files. Backend-specific readers (Linux /proc parsing,
// macOS sysctl/mach) live in their own platform directories and may build on
// top of these.

#pragma once

#include <array>
#include <cerrno>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <fcntl.h>
#include <unistd.h>
#ifndef _WIN32
#include <pwd.h>      // getpwuid_r / struct passwd (macOS resolver)
#endif

namespace rockbottom::sys {

// Read an entire (small, virtual) file into `out`, REUSING its capacity. This
// is the hot path: sample_procs() reads /proc/<pid>/{stat,statm,io,cmdline}
// for hundreds of pids every tick, and the old std::ifstream + std::stringstream
// slurp allocated a stream buffer + a result string PER read (~2000 heap
// allocations/tick at 400 pids) and dragged in locale/sentry machinery for
// what is a raw byte copy. Here: one open()/read() loop straight into the
// caller's buffer, which after the first pid is already big enough for every
// /proc node, so steady-state allocations are ZERO. Returns false + clears
// `out` on failure (file gone / permission). procfs files report size 0 via
// stat, so we can't pre-size from st_size — read in chunks until EOF.
//
// Robustness: every /proc node we read is a few KB, but a caller could aim
// this at a regular file (or a pathological/growing sysfs node). Cap the read
// at kMaxSlurp so a bad path can never balloon the sampler's memory or spin
// forever — we return what we got (still `true`; the parsers all tolerate a
// truncated tail) rather than allocating without bound. EINTR is retried;
// EAGAIN too, in case a caller ever hands us a non-blocking fd.
inline bool slurp_into(const char* path, std::string& out) {
    constexpr std::size_t kMaxSlurp = 8u << 20;   // 8 MiB hard ceiling
    out.clear();
    int fd = ::open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) return false;
    char buf[4096];
    bool ok = true;
    for (;;) {
        ssize_t n = ::read(fd, buf, sizeof buf);
        if (n > 0) {
            out.append(buf, static_cast<std::size_t>(n));
            if (out.size() >= kMaxSlurp) break;   // refuse to grow without bound
            continue;
        }
        if (n == 0) break;                        // EOF
        if (errno == EINTR || errno == EAGAIN) continue;
        ok = false;
        break;
    }
    // Retry close across EINTR; on most platforms the fd is already gone after
    // EINTR, but the retry is harmless and correct where it isn't.
    while (::close(fd) != 0 && errno == EINTR) {}
    if (!ok) { out.clear(); return false; }
    return true;
}

// Read an entire (small, virtual) file into a fresh string. Empty on failure.
// Convenience wrapper over slurp_into for cold callers that don't keep a
// reusable buffer; the hot per-process loop should call slurp_into directly.
inline std::string slurp(const char* path) {
    std::string s;
    slurp_into(path, s);
    return s;
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
// The right resolver depends on how `rb` is linked, which depends on the OS.
//
// macOS: getpwuid_r is the ONLY correct answer. The login user (uid 501+) is
// a Directory Services account that never appears in /etc/passwd — and there
// is no `getent`. rb is not statically linked here, so getpwuid_r safely goes
// through Open Directory and resolves every real account. Parsing /etc/passwd
// would miss the interactive user entirely and then fork a doomed shell per
// uid; at bench cadence (dozens of uids per tick) that fork storm wedges the
// process. So on Apple we call getpwuid_r and stop.
//
// Linux STATIC-LINKING HAZARD, and the reason this is not a one-line getpwuid
// call there. `rb` ships as a fully static binary so it runs on any Linux
// regardless of the host's glibc version. But glibc resolves users through
// NSS, which dlopen()s libnss_* at runtime — and a static binary has no
// dynamic loader, so getpwuid() can only ever see /etc/passwd, and on some
// glibc builds even *calling* it from a static binary segfaults. On an
// LDAP/AD/SSSD host (where every interactive account is a directory account
// and /etc/passwd holds only system users) a bare numeric uid is a silent
// legibility regression. So on Linux we parse /etc/passwd directly (no NSS,
// no dlopen, cannot segfault) and fall back to the system's own dynamically-
// linked `getent`, which loads the NSS stack we cannot and sees directory
// accounts. The uid→name result is cached by the caller (Sampler::uid_cache_),
// so getent costs at most one fork per distinct uid per run.
//
// Returns the uid as a decimal string if nothing can name it — an unresolvable
// uid is a real state (deleted account, unmapped container id), not an error.
inline std::string user_of(uid_t uid) {
#if defined(__APPLE__)
    // Dynamically linked, Directory-Services-backed: getpwuid_r is safe and
    // complete. _r variant so this stays reentrant across sampler threads.
    struct passwd pw{};
    struct passwd* result = nullptr;
    char buf[1024];
    if (::getpwuid_r(uid, &pw, buf, sizeof buf, &result) == 0 && result &&
        pw.pw_name && pw.pw_name[0]) {
        return std::string(pw.pw_name);
    }
#elif !defined(_WIN32)
    // 1) Parse /etc/passwd directly. No NSS, no dlopen, cannot segfault in a
    //    static binary. Line format: name:passwd:uid:gid:gecos:dir:shell
    if (std::FILE* pf = std::fopen("/etc/passwd", "r")) {
        char line[1024];
        std::string name;
        while (std::fgets(line, sizeof line, pf)) {
            const char* c1 = std::strchr(line, ':');
            if (!c1) continue;
            const char* c2 = std::strchr(c1 + 1, ':');   // after passwd field
            if (!c2) continue;
            unsigned long row = std::strtoul(c2 + 1, nullptr, 10);
            if (row == static_cast<unsigned long>(uid)) {
                name.assign(line, static_cast<std::size_t>(c1 - line));
                break;
            }
        }
        std::fclose(pf);
        if (!name.empty()) return name;
    }

    // 2) Directory accounts (LDAP/AD/SSSD) are not in /etc/passwd. Ask the
    //    system's own dynamically-linked getent, which loads the NSS stack
    //    safely in a normal process. Base install on every distro we target
    //    (glibc on Debian/Ubuntu/RHEL/SUSE, busybox on Alpine).
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
