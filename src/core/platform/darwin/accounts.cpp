// collectors/accounts.cpp (darwin) — user accounts + login sessions.
//
// macOS keeps user identity in OpenDirectory, not a flat passwd file, but
// getpwent() still walks it through the NSS-equivalent, so the identity half
// works unchanged. The two pieces that do NOT port are deliberately left
// empty rather than faked:
//
//   * QUOTAS. macOS has quotactl(2), but the modern default (APFS) does not
//     implement per-uid quotas at all, so the call would fail on every real
//     Mac. Reporting "no quota data" is the truth.
//   * LOGIND. There is no systemd; sessions come from utmpx here.
//
// The background home scanner is platform-independent (it's just readdir +
// lstat), so disk-per-user still works on macOS via that path.

#include "../../sampler.hpp"

#include <algorithm>
#include <string>
#include <string_view>
#include <vector>

#include <pwd.h>
#include <utmpx.h>

namespace rockbottom {

namespace {

bool is_nologin(std::string_view sh) {
    if (sh.empty()) return true;
    auto ends = [&](std::string_view suf) {
        return sh.size() >= suf.size() &&
               sh.compare(sh.size() - suf.size(), suf.size(), suf) == 0;
    };
    return ends("/nologin") || ends("/false") || ends("/uucico");
}

}  // namespace

void Sampler::sample_accounts(std::vector<UserAccount>& accounts,
                              std::vector<LoginSession>& sessions) {
    accounts.clear();
    sessions.clear();

    ::setpwent();
    while (struct passwd* pw = ::getpwent()) {
        UserAccount a;
        a.name  = pw->pw_name ? pw->pw_name : "";
        a.uid   = pw->pw_uid;
        a.gid   = pw->pw_gid;
        a.home  = pw->pw_dir ? pw->pw_dir : "";
        a.shell = pw->pw_shell ? pw->pw_shell : "";
        a.gecos = pw->pw_gecos ? pw->pw_gecos : "";
        if (const std::size_t c = a.gecos.find(','); c != std::string::npos)
            a.gecos.resize(c);
        // macOS convention: real users start at 501; _-prefixed names are the
        // service accounts (_spotlight, _windowserver, …).
        a.system = a.uid < 500 || (!a.name.empty() && a.name[0] == '_');
        a.can_login = !is_nologin(a.shell);
        if (a.name.empty()) continue;
        accounts.push_back(std::move(a));
    }
    ::endpwent();

    // Sessions from utmpx (USER_PROCESS entries are live logins).
    ::setutxent();
    while (struct utmpx* u = ::getutxent()) {
        if (u->ut_type != USER_PROCESS) continue;
        LoginSession s;
        s.user   = u->ut_user;
        s.tty    = u->ut_line;
        s.remote = u->ut_host;
        s.id     = s.tty;
        s.active = true;
        s.leader = static_cast<int>(u->ut_pid);
        s.login_at = static_cast<std::uint64_t>(u->ut_tv.tv_sec);
        if (!s.user.empty()) sessions.push_back(std::move(s));
    }
    ::endutxent();

    std::sort(sessions.begin(), sessions.end(),
              [](const LoginSession& a, const LoginSession& b) {
                  if (a.user != b.user) return a.user < b.user;
                  return a.id < b.id;
              });

    // Disk: quotas aren't available on APFS, so only the background scanner
    // can answer. Publish whatever it has finished.
    {
        std::lock_guard<std::mutex> lk(home_scan_mu_);
        for (UserAccount& a : accounts) {
            auto it = home_scan_.find(a.name);
            if (it == home_scan_.end()) continue;
            a.disk_bytes   = it->second.bytes;
            a.disk_files   = it->second.files;
            a.disk_known   = true;
            a.disk_partial = !it->second.complete;
            a.disk_source  = "scan";
        }
    }

    std::sort(accounts.begin(), accounts.end(),
              [](const UserAccount& a, const UserAccount& b) {
                  if (a.system != b.system) return !a.system;
                  return a.name < b.name;
              });
}

}  // namespace rockbottom
