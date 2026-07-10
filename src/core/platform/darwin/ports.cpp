// platform/darwin/ports.cpp — libproc fd walk → bound TCP/UDP ports per pid.
//
// The macOS analogue of the /proc/net + /proc/<pid>/fd inode join, but simpler:
// proc_pidfdinfo(PROC_PIDFDSOCKETINFO) hands the socket state directly, so
// there's no inode table to build. For each process we list its file
// descriptors, keep the socket ones, and record the local port of TCP
// listeners and every bound UDP socket — the same rule the Linux backend uses.
// Visibility is identical too: without root you see your own processes' sockets.

#include "../../sampler.hpp"
#include "mach_util.hpp"

#include <algorithm>
#include <arpa/inet.h>
#include <cstdint>
#include <vector>

#include <libproc.h>
#include <sys/proc_info.h>
#include <netinet/in.h>
#include <netinet/tcp_fsm.h>

namespace rockbottom {

void Sampler::sample_ports() {
    pid_ports_.clear();

    int cap = ::proc_listpids(PROC_ALL_PIDS, 0, nullptr, 0);
    if (cap <= 0) return;
    std::vector<pid_t> pids(static_cast<std::size_t>(cap) / sizeof(pid_t) + 16);
    int got = ::proc_listpids(PROC_ALL_PIDS, 0, pids.data(),
                              static_cast<int>(pids.size() * sizeof(pid_t)));
    if (got <= 0) return;
    int npids = got / static_cast<int>(sizeof(pid_t));

    std::vector<char> fdbuf;
    for (int i = 0; i < npids; ++i) {
        int pid = pids[static_cast<std::size_t>(i)];
        if (pid <= 0) continue;

        int bytes = ::proc_pidinfo(pid, PROC_PIDLISTFDS, 0, nullptr, 0);
        if (bytes <= 0) continue;                       // not ours / no fds
        fdbuf.resize(static_cast<std::size_t>(bytes));
        bytes = ::proc_pidinfo(pid, PROC_PIDLISTFDS, 0, fdbuf.data(), bytes);
        if (bytes <= 0) continue;
        int nfd = bytes / static_cast<int>(sizeof(proc_fdinfo));
        auto* fds = reinterpret_cast<proc_fdinfo*>(fdbuf.data());

        std::vector<std::uint16_t> ports;
        for (int f = 0; f < nfd; ++f) {
            if (fds[f].proc_fdtype != PROX_FDTYPE_SOCKET) continue;
            socket_fdinfo si{};
            if (::proc_pidfdinfo(pid, fds[f].proc_fd, PROC_PIDFDSOCKETINFO,
                                 &si, sizeof si) < static_cast<int>(sizeof si))
                continue;
            const auto& in = si.psi.soi_proto.pri_in;
            std::uint16_t port = ntohs(static_cast<std::uint16_t>(in.insi_lport));
            if (!port) continue;
            if (si.psi.soi_kind == SOCKINFO_TCP) {
                // TCP: only listeners (matches the Linux LISTEN-only rule).
                if (si.psi.soi_proto.pri_tcp.tcpsi_state != TCPS_LISTEN) continue;
            } else if (si.psi.soi_kind != SOCKINFO_IN) {
                continue;   // keep bound UDP (SOCKINFO_IN); drop the rest
            }
            ports.push_back(port);
        }
        if (!ports.empty()) {
            std::sort(ports.begin(), ports.end());
            ports.erase(std::unique(ports.begin(), ports.end()), ports.end());
            pid_ports_[pid] = std::move(ports);
        }
    }
}

}  // namespace rockbottom
