// collectors/accounts.cpp — user ACCOUNTS: identity, disk usage, live sessions.
//
// The USERS pane started as a rollup of running processes. That answers "who is
// burning CPU right now" but not the other half of user management: who EXISTS
// on this box, who is actually logged in, and — the question every admin
// actually gets paged about — who is filling the disk.
//
// ── why disk usage is the hard one ───────────────────────────────────────────
//
// There is no cheap kernel counter for "bytes owned by uid N". The honest
// options, in the order we try them:
//
//   1. QUOTAS (quotactl). Instant, exact, kernel-maintained — the filesystem
//      already tracks per-uid bytes when usrquota is enabled. This is the
//      right answer and costs microseconds. It's just rarely turned on.
//   2. A DIRECTORY SCAN of each home. Truthful but expensive: measured on the
//      dev box, a warm-cache `du -s /home/ayush` over 242 GB takes 12.3
//      SECONDS. Doing that synchronously would freeze the UI for twelve
//      seconds; doing it every tick would make rockbottom the thing eating
//      the machine. So it runs on a background thread, under a strict wall
//      -clock budget and a file-count cap, never blocks a frame, and reports
//      itself as partial while it's still walking.
//
// What we refuse to do is guess. An account with no quota and no completed
// scan reports disk_known=false, and the pane prints "—" rather than a
// confident 0 that an admin might act on.

#include "../../sampler.hpp"
#include "../common/home_scan.hpp"
#include "procfs.hpp"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include <dirent.h>
#include <fcntl.h>
#include <pwd.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#if defined(__linux__)
#include <sys/quota.h>
// <sys/quota.h> gives us quotactl() and struct dqblk on both glibc and musl,
// but the three CONSTANTS below live in <linux/quota.h> — a KERNEL header,
// present on glibc distros and absent on Alpine (and any musl sysroot without
// linux-headers). Including it unconditionally broke the static musl build,
// which is exactly how the release binaries are produced, so CI went red at
// tag time.
//
// These values are a stable part of the quotactl(2) ABI — they encode the
// syscall's wire format, so the kernel cannot change them without breaking
// every binary ever built. Defining them when the header is missing is what
// quota-tools itself does, and the #ifndef guards defer to the real header
// wherever it exists.
#ifndef USRQUOTA
#define USRQUOTA 0
#endif
#ifndef SUBCMDSHIFT
#define SUBCMDSHIFT 8
#endif
#ifndef SUBCMDMASK
#define SUBCMDMASK 0x00ff
#endif
#ifndef QCMD
#define QCMD(cmd, type) (((cmd) << SUBCMDSHIFT) | ((type) & SUBCMDMASK))
#endif
#ifndef Q_GETQUOTA
#define Q_GETQUOTA 0x800007
#endif
#endif

namespace rockbottom {

using namespace procfs;

namespace {

// A shell that means "this account cannot log in". Checked by suffix so
// /usr/sbin/nologin, /sbin/nologin and /bin/false all resolve the same.
bool is_nologin(std::string_view sh) {
    if (sh.empty()) return true;
    auto ends = [&](std::string_view suf) {
        return sh.size() >= suf.size() &&
               sh.compare(sh.size() - suf.size(), suf.size(), suf) == 0;
    };
    return ends("/nologin") || ends("/false") || ends("/sync") || sh == "nologin";
}

// ── quotas ──────────────────────────────────────────────────────────────────
// Ask the kernel for a uid's usage on a filesystem. Returns false when quotas
// aren't enabled there (the overwhelmingly common case), which is not an error
// — it just means we fall through to the scanner.
#if defined(__linux__)
bool quota_usage(const std::string& dev, unsigned uid,
                 std::uint64_t& bytes, std::uint64_t& limit, std::uint64_t& files) {
    // struct dqblk, not if_dqblk: `dqblk` is declared by <sys/quota.h> on BOTH
    // glibc and musl with the identical field layout (it IS the quotactl wire
    // struct), whereas `if_dqblk` is only reachable via the kernel headers.
    // Using it means this file needs no kernel headers at all.
    struct dqblk d;
    std::memset(&d, 0, sizeof d);
    // char*, not caddr_t: glibc's prototype takes __caddr_t (a typedef for
    // char*) while musl's takes char* directly and never defines caddr_t at
    // all. char* converts implicitly to both, so this compiles on either
    // libc — and the release binaries are built against musl.
    if (::quotactl(QCMD(Q_GETQUOTA, USRQUOTA), dev.c_str(),
                   static_cast<int>(uid), reinterpret_cast<char*>(&d)) != 0)
        return false;
    // dqb_curspace is bytes; the limits are in 1 KiB blocks (quota(3) ABI).
    bytes = d.dqb_curspace;
    limit = static_cast<std::uint64_t>(d.dqb_bhardlimit) * 1024ull;
    files = d.dqb_curinodes;
    return true;
}

// Every mounted filesystem that has user quotas turned on, as device paths.
std::vector<std::string> quota_devices() {
    std::vector<std::string> out;
    std::string mounts = sys::slurp("/proc/mounts");
    std::size_t pos = 0;
    while (pos < mounts.size()) {
        std::size_t nl = mounts.find('\n', pos);
        if (nl == std::string::npos) nl = mounts.size();
        std::string_view line(mounts.data() + pos, nl - pos);
        pos = nl + 1;
        // dev mountpoint fstype opts …
        std::string dev, mnt, fstype, opts;
        {
            std::size_t f = 0;
            auto field = [&]() -> std::string {
                while (f < line.size() && line[f] == ' ') ++f;
                std::size_t s = f;
                while (f < line.size() && line[f] != ' ') ++f;
                return std::string(line.substr(s, f - s));
            };
            dev = field(); mnt = field(); fstype = field(); opts = field();
        }
        if (dev.rfind("/dev/", 0) != 0) continue;
        if (opts.find("usrquota") == std::string::npos &&
            opts.find("usrjquota") == std::string::npos) continue;
        out.push_back(dev);
    }
    return out;
}
#endif  // __linux__

// Parse /etc/passwd directly into accounts.
//
// CRITICAL: this must NOT use getpwent()/getpwuid(). `rb` ships as a fully
// static binary (see sys_util.hpp user_of() for the long version), and glibc
// resolves users through NSS, which dlopen()s libnss_* at runtime. A static
// binary has no dynamic loader, so those calls don't merely return nothing —
// they SEGFAULT. The first cut of this collector used getpwent() and crashed
// every entry point (`--selfcheck`, `--bench`, `--doctor`) with SIGSEGV.
//
// Line format: name:passwd:uid:gid:gecos:dir:shell
void parse_passwd(std::vector<UserAccount>& out) {
    std::FILE* pf = std::fopen("/etc/passwd", "r");
    if (!pf) return;
    char line[2048];
    while (std::fgets(line, sizeof line, pf)) {
        // Split on ':' into at most 7 fields, in place.
        std::string_view sv(line);
        while (!sv.empty() && (sv.back() == '\n' || sv.back() == '\r')) sv.remove_suffix(1);
        std::string_view f[7];
        int n = 0;
        std::size_t pos = 0;
        while (n < 7) {
            const std::size_t c = sv.find(':', pos);
            if (c == std::string_view::npos) { f[n++] = sv.substr(pos); break; }
            f[n++] = sv.substr(pos, c - pos);
            pos = c + 1;
        }
        if (n < 7 || f[0].empty()) continue;

        UserAccount a;
        a.name  = std::string(f[0]);
        a.uid   = static_cast<unsigned>(std::strtoul(std::string(f[2]).c_str(), nullptr, 10));
        a.gid   = static_cast<unsigned>(std::strtoul(std::string(f[3]).c_str(), nullptr, 10));
        a.gecos = std::string(f[4]);
        a.home  = std::string(f[5]);
        a.shell = std::string(f[6]);
        // gecos is comma-separated (full name, room, phones); only the first
        // part is the human's name.
        if (const std::size_t c = a.gecos.find(','); c != std::string::npos)
            a.gecos.resize(c);
        a.system = a.uid < 1000 || a.uid == 65534;   // 65534 = nobody
        a.can_login = !is_nologin(a.shell);
        out.push_back(std::move(a));
    }
    std::fclose(pf);
}

// ── login sessions ──────────────────────────────────────────────────────────
// systemd-logind keeps one file per session under /run/systemd/sessions. The
// files say "do not parse" because the format is private — but the alternative
// is linking libsystemd (a hard dependency that would break the static build
// this project guarantees) or shelling out to loginctl once a second. We read
// them defensively: every field is optional, anything missing is left at its
// default, and a format change degrades to fewer columns rather than a crash.
void read_logind_sessions(std::vector<LoginSession>& out) {
    DIR* d = ::opendir("/run/systemd/sessions");
    if (!d) return;
    while (dirent* e = ::readdir(d)) {
        if (e->d_name[0] == '.') continue;
        // Skip the "N.ref" lock files logind also keeps in here.
        const std::string name = e->d_name;
        if (name.find('.') != std::string::npos) continue;

        const std::string body = sys::slurp("/run/systemd/sessions/" + name);
        if (body.empty()) continue;

        LoginSession s;
        s.id = name;
        std::size_t pos = 0;
        while (pos < body.size()) {
            std::size_t nl = body.find('\n', pos);
            if (nl == std::string::npos) nl = body.size();
            std::string_view line(body.data() + pos, nl - pos);
            pos = nl + 1;
            const std::size_t eq = line.find('=');
            if (eq == std::string_view::npos) continue;
            const std::string_view k = line.substr(0, eq);
            const std::string v(line.substr(eq + 1));
            if      (k == "USER")    s.user = v;
            else if (k == "TTY")     s.tty = v;
            else if (k == "REMOTE_HOST") s.remote = v;
            else if (k == "TYPE")    s.type = v;
            else if (k == "ACTIVE")  s.active = (v == "1");
            else if (k == "LEADER")  s.leader = std::atoi(v.c_str());
            else if (k == "REALTIME") {
                // microseconds since the epoch
                s.login_at = std::strtoull(v.c_str(), nullptr, 10) / 1000000ull;
            }
        }
        if (!s.user.empty()) out.push_back(std::move(s));
    }
    ::closedir(d);
}

}  // namespace

// ── the collector ───────────────────────────────────────────────────────────
void Sampler::sample_accounts(std::vector<UserAccount>& accounts,
                              std::vector<LoginSession>& sessions) {
    accounts.clear();
    sessions.clear();

    // 1. Identity. Parsed from /etc/passwd directly — NOT getpwent(), which
    //    segfaults in a static binary (see parse_passwd above).
    parse_passwd(accounts);

    // 2. Live sessions.
    read_logind_sessions(sessions);
    std::sort(sessions.begin(), sessions.end(),
              [](const LoginSession& a, const LoginSession& b) {
                  if (a.user != b.user) return a.user < b.user;
                  return a.id < b.id;
              });

#if defined(__linux__)
    // 3. Disk via quotas — instant when the filesystem has them enabled.
    for (const std::string& dev : quota_devices()) {
        for (UserAccount& a : accounts) {
            std::uint64_t bytes = 0, limit = 0, files = 0;
            if (!quota_usage(dev, a.uid, bytes, limit, files)) continue;
            // Quotas are per-filesystem; a user with a home on one and scratch
            // on another legitimately sums across both.
            a.disk_bytes += bytes;
            a.disk_files += files;
            a.disk_quota = std::max(a.disk_quota, limit);
            a.disk_known = true;
            a.disk_source = "quota";
        }
    }
#endif

    // 4. Disk via the background scanner, for accounts quotas didn't cover.
    //    Results are published by the scan thread into home_scan_; here we
    //    just read whatever has completed so far.
    {
        std::lock_guard<std::mutex> lk(home_scan_mu_);
        for (UserAccount& a : accounts) {
            if (a.disk_known && a.disk_source == std::string("quota")) continue;
            auto it = home_scan_.find(a.name);
            if (it == home_scan_.end()) continue;
            a.disk_bytes   = it->second.bytes;
            a.disk_files   = it->second.files;
            a.disk_known   = true;
            a.disk_partial = !it->second.complete;
            a.disk_source  = "scan";
        }
    }

    // Deterministic order: real people first (they're who you're looking for),
    // then system accounts, each alphabetically.
    std::sort(accounts.begin(), accounts.end(),
              [](const UserAccount& a, const UserAccount& b) {
                  if (a.system != b.system) return !a.system;
                  return a.name < b.name;
              });
}

}  // namespace rockbottom
