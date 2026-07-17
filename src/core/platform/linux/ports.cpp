// collectors/ports.cpp — which process is bound to which port.
//
// Two-phase join, the same trick `ss -p` / `lsof -i` use:
//   1. /proc/net/{tcp,tcp6,udp,udp6} lists sockets: local port (hex) +
//      the socket's INODE. We keep listeners (TCP state 0A = LISTEN) and
//      all bound UDP sockets.
//   2. /proc/<pid>/fd/* are symlinks; open sockets read "socket:[inode]".
//      Matching inodes joins port → pid.
//
// Phase 2 requires walking every process's fd table, which is only
// readable for your own processes without root — same limitation ss has.
// Cost is bounded: one readlink per fd, and we only resolve inodes that
// phase 1 actually collected.

#include "../../sampler.hpp"
#include "procfs.hpp"

#include <algorithm>
#include <climits>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <dirent.h>
#include <fstream>
#include <sstream>
#include <string>
#include <unistd.h>
#include <unordered_map>
#include <unordered_set>

namespace rockbottom {

namespace {

// TCP state hex code (/proc/net/tcp `st` column) → the familiar name.
const char* tcp_state_name(const std::string& st) {
    static const struct { const char* code; const char* name; } kMap[] = {
        {"01","ESTABLISHED"},{"02","SYN_SENT"},{"03","SYN_RECV"},
        {"04","FIN_WAIT1"},{"05","FIN_WAIT2"},{"06","TIME_WAIT"},
        {"07","CLOSE"},{"08","CLOSE_WAIT"},{"09","LAST_ACK"},
        {"0A","LISTEN"},{"0B","CLOSING"},
    };
    for (auto& m : kMap) if (st == m.code) return m.name;
    return "?";
}

// Decode a /proc/net hex endpoint "0100007F:1F90" (or a v6 32-hex-digit addr)
// into "ip:port". Little-endian byte order for the v4 address, as the kernel
// writes it. A zero address renders as "*".
std::string decode_endpoint(const std::string& hex, bool v6) {
    auto colon = hex.rfind(':');
    if (colon == std::string::npos) return "*";
    std::string a = hex.substr(0, colon);
    unsigned port = static_cast<unsigned>(std::strtoul(hex.substr(colon + 1).c_str(), nullptr, 16));
    std::string host;
    if (!v6 && a.size() == 8) {
        unsigned long v = std::strtoul(a.c_str(), nullptr, 16);
        unsigned b0 = v & 0xff, b1 = (v >> 8) & 0xff, b2 = (v >> 16) & 0xff, b3 = (v >> 24) & 0xff;
        if (v == 0) host = "*";
        else { char t[24]; std::snprintf(t, sizeof t, "%u.%u.%u.%u", b0, b1, b2, b3); host = t; }
    } else {
        bool allzero = a.find_first_not_of('0') == std::string::npos;
        host = allzero ? "*" : "[v6]";   // full v6 pretty-print is overkill for the table
    }
    return host + ":" + std::to_string(port);
}

// One socket row parsed from /proc/net/{tcp,udp}[6].
struct SockRow {
    std::uint16_t lport = 0;
    std::string   proto, laddr, raddr, state;
    bool          listener = false;
};

// Parse a /proc/net table into inode → SockRow. `is_tcp` selects state parsing
// (UDP has no meaningful state); `proto` labels the rows.
void parse_net_table(const char* path, bool is_tcp, bool v6, const char* proto,
                     std::unordered_map<std::uint64_t, SockRow>& out) {
    std::ifstream f(path);
    if (!f) return;
    std::string line;
    std::getline(f, line);   // header
    while (std::getline(f, line)) {
        std::istringstream ss(line);
        std::string sl, local, rem, st;
        ss >> sl >> local >> rem >> st;
        std::string skip;
        for (int i = 0; i < 5; ++i) ss >> skip;   // tx:rx tr tm retr uid timeout
        std::uint64_t inode = 0;
        ss >> inode;
        if (!inode) continue;

        auto colon = local.rfind(':');
        if (colon == std::string::npos) continue;
        auto port = static_cast<std::uint16_t>(
            std::strtoul(local.substr(colon + 1).c_str(), nullptr, 16));
        if (!port) continue;

        SockRow r;
        r.lport = port;
        r.proto = proto;
        r.laddr = decode_endpoint(local, v6);
        r.raddr = is_tcp ? decode_endpoint(rem, v6) : "*";
        r.state = is_tcp ? tcp_state_name(st) : "";
        r.listener = is_tcp ? (st == "0A") : true;
        out.emplace(inode, std::move(r));
    }
}

}  // namespace

void Sampler::sample_ports() {
    pid_ports_.clear();
    connections_.clear();

    // Phase 1: socket inode → full socket row (addrs + state).
    std::unordered_map<std::uint64_t, SockRow> inode_row;
    parse_net_table("/proc/net/tcp",  true,  false, "tcp",  inode_row);
    parse_net_table("/proc/net/tcp6", true,  true,  "tcp6", inode_row);
    parse_net_table("/proc/net/udp",  false, false, "udp",  inode_row);
    parse_net_table("/proc/net/udp6", false, true,  "udp6", inode_row);
    if (inode_row.empty()) return;

    // Phase 2: walk fd tables, join on inode.
    DIR* proc = ::opendir("/proc");
    if (!proc) return;
    dirent* e;
    char buf[64];
    while ((e = ::readdir(proc)) != nullptr) {
        if (e->d_name[0] < '0' || e->d_name[0] > '9') continue;
        // Validated pid parse (same rule as proc.cpp): reject trailing garbage
        // rather than letting atoi turn a malformed entry into a bogus pid.
        char* pend = nullptr;
        long pidl = std::strtol(e->d_name, &pend, 10);
        if (pend == e->d_name || *pend != '\0' || pidl <= 0 || pidl > INT_MAX) continue;
        int pid = static_cast<int>(pidl);
        std::string fd_dir = "/proc/" + std::string(e->d_name) + "/fd";
        DIR* fds = ::opendir(fd_dir.c_str());
        if (!fds) continue;   // not ours — same visibility rule as ss/lsof

        std::unordered_set<std::uint16_t> ports;
        // A process can hold the SAME socket through several fds (dup, fork
        // inheritance); emit one connection row per (pid, inode), not per fd.
        std::unordered_set<std::uint64_t> conn_seen;
        dirent* fe;
        while ((fe = ::readdir(fds)) != nullptr) {
            if (fe->d_name[0] == '.') continue;
            std::string link = fd_dir + "/" + fe->d_name;
            ssize_t n = ::readlink(link.c_str(), buf, sizeof buf - 1);
            if (n <= 10 || std::string_view(buf, 8) != "socket:[") continue;
            buf[n] = 0;
            std::uint64_t inode = std::strtoull(buf + 8, nullptr, 10);
            auto it = inode_row.find(inode);
            if (it == inode_row.end()) continue;
            const SockRow& r = it->second;
            if (conn_seen.insert(inode).second) {
                Connection c;
                c.proto = r.proto; c.laddr = r.laddr; c.raddr = r.raddr;
                c.state = r.state; c.pid = pid;
                connections_.push_back(std::move(c));
            }
            // ports column: listeners + bound UDP.
            if (r.listener) ports.insert(r.lport);
        }
        ::closedir(fds);

        if (!ports.empty()) {
            auto& v = pid_ports_[pid];
            v.assign(ports.begin(), ports.end());
            std::sort(v.begin(), v.end());
        }
    }
    ::closedir(proc);

    // Established first, then listeners, then the rest.
    std::stable_sort(connections_.begin(), connections_.end(),
                     [](const Connection& a, const Connection& b) {
                         auto rank = [](const std::string& s) {
                             return s == "ESTABLISHED" ? 0 : s == "LISTEN" ? 1 : 2;
                         };
                         return rank(a.state) < rank(b.state);
                     });
}

}  // namespace rockbottom
