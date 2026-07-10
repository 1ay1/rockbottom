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

#include "../sampler.hpp"
#include "../procfs.hpp"

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <dirent.h>
#include <fstream>
#include <sstream>
#include <string>
#include <unistd.h>
#include <unordered_map>
#include <unordered_set>

namespace bottom {

namespace {

// Parse one /proc/net/{tcp,udp}[6] table: inode → local port.
// `listeners_only` keeps TCP LISTEN (st == 0A); UDP passes everything bound.
void parse_net_table(const char* path, bool listeners_only,
                     std::unordered_map<std::uint64_t, std::uint16_t>& out) {
    std::ifstream f(path);
    if (!f) return;
    std::string line;
    std::getline(f, line);   // header
    while (std::getline(f, line)) {
        //  sl  local_address rem_address   st ... inode
        std::istringstream ss(line);
        std::string sl, local, rem, st;
        ss >> sl >> local >> rem >> st;
        std::string skip;
        // tx_queue:rx_queue tr:tm->when retrnsmt uid timeout
        for (int i = 0; i < 5; ++i) ss >> skip;
        std::uint64_t inode = 0;
        ss >> inode;
        if (!inode) continue;
        if (listeners_only && st != "0A") continue;

        auto colon = local.rfind(':');
        if (colon == std::string::npos) continue;
        auto port = static_cast<std::uint16_t>(
            std::strtoul(local.substr(colon + 1).c_str(), nullptr, 16));
        if (port) out.emplace(inode, port);
    }
}

}  // namespace

void Sampler::sample_ports() {
    pid_ports_.clear();

    // Phase 1: socket inode → port.
    std::unordered_map<std::uint64_t, std::uint16_t> inode_port;
    parse_net_table("/proc/net/tcp",  true,  inode_port);
    parse_net_table("/proc/net/tcp6", true,  inode_port);
    parse_net_table("/proc/net/udp",  false, inode_port);
    parse_net_table("/proc/net/udp6", false, inode_port);
    if (inode_port.empty()) return;

    // Phase 2: walk fd tables, join on inode.
    DIR* proc = ::opendir("/proc");
    if (!proc) return;
    dirent* e;
    char buf[64];
    while ((e = ::readdir(proc)) != nullptr) {
        if (e->d_name[0] < '0' || e->d_name[0] > '9') continue;
        int pid = std::atoi(e->d_name);
        std::string fd_dir = "/proc/" + std::string(e->d_name) + "/fd";
        DIR* fds = ::opendir(fd_dir.c_str());
        if (!fds) continue;   // not ours — same visibility rule as ss/lsof

        std::unordered_set<std::uint16_t> ports;
        dirent* fe;
        while ((fe = ::readdir(fds)) != nullptr) {
            if (fe->d_name[0] == '.') continue;
            std::string link = fd_dir + "/" + fe->d_name;
            ssize_t n = ::readlink(link.c_str(), buf, sizeof buf - 1);
            if (n <= 10 || std::string_view(buf, 8) != "socket:[") continue;
            buf[n] = 0;
            std::uint64_t inode = std::strtoull(buf + 8, nullptr, 10);
            auto it = inode_port.find(inode);
            if (it != inode_port.end()) ports.insert(it->second);
        }
        ::closedir(fds);

        if (!ports.empty()) {
            auto& v = pid_ports_[pid];
            v.assign(ports.begin(), ports.end());
            std::sort(v.begin(), v.end());
        }
    }
    ::closedir(proc);
}

}  // namespace bottom
