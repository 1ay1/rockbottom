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
#include <cstdio>
#include <string>
#include <vector>

#include <libproc.h>
#include <sys/proc_info.h>
#include <netinet/in.h>
#include <netinet/tcp_fsm.h>

namespace rockbottom {

namespace {

// TCP state number → the familiar name (matches ss / netstat).
const char* tcp_state_name(int s) {
    switch (s) {
        case TCPS_CLOSED:       return "CLOSED";
        case TCPS_LISTEN:       return "LISTEN";
        case TCPS_SYN_SENT:     return "SYN_SENT";
        case TCPS_SYN_RECEIVED: return "SYN_RECV";
        case TCPS_ESTABLISHED:  return "ESTABLISHED";
        case TCPS_CLOSE_WAIT:   return "CLOSE_WAIT";
        case TCPS_FIN_WAIT_1:   return "FIN_WAIT1";
        case TCPS_CLOSING:      return "CLOSING";
        case TCPS_LAST_ACK:     return "LAST_ACK";
        case TCPS_FIN_WAIT_2:   return "FIN_WAIT2";
        case TCPS_TIME_WAIT:    return "TIME_WAIT";
        default:                return "?";
    }
}

// Render an in_sockinfo endpoint (v4 or v6) as "ip:port". A zero address reads
// as "*" (any), the netstat convention for listeners / unbound sockets.
std::string endpoint(const in_sockinfo& in, bool v6, bool local) {
    std::uint16_t port = ntohs(static_cast<std::uint16_t>(local ? in.insi_lport : in.insi_fport));
    char ip[INET6_ADDRSTRLEN] = {0};
    bool any = true;
    if (v6) {
        const auto& a = local ? in.insi_laddr.ina_6 : in.insi_faddr.ina_6;
        for (int i = 0; i < 16; ++i) if (a.s6_addr[i]) { any = false; break; }
        if (!any) ::inet_ntop(AF_INET6, &a, ip, sizeof ip);
    } else {
        std::uint32_t a = local ? in.insi_laddr.ina_46.i46a_addr4.s_addr
                                : in.insi_faddr.ina_46.i46a_addr4.s_addr;
        any = (a == 0);
        if (!any) ::inet_ntop(AF_INET, &a, ip, sizeof ip);
    }
    std::string host = any ? "*" : ip;
    return host + ":" + std::to_string(port);
}

}  // namespace

void Sampler::sample_ports() {
    pid_ports_.clear();
    connections_.clear();

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
            const int kind = si.psi.soi_kind;
            if (kind != SOCKINFO_TCP && kind != SOCKINFO_IN) continue;  // tcp/udp only
            const auto& in = si.psi.soi_proto.pri_in;
            const bool v6 = si.psi.soi_family == AF_INET6;
            std::uint16_t lport = ntohs(static_cast<std::uint16_t>(in.insi_lport));
            if (!lport) continue;

            const bool is_tcp = kind == SOCKINFO_TCP;
            int tstate = is_tcp ? si.psi.soi_proto.pri_tcp.tcpsi_state : -1;

            // ports column: TCP listeners + bound UDP (unchanged rule).
            if ((is_tcp && tstate == TCPS_LISTEN) || !is_tcp)
                ports.push_back(lport);

            // Connection table: every TCP socket + bound UDP, with endpoints.
            Connection c;
            c.proto = v6 ? (is_tcp ? "tcp6" : "udp6") : (is_tcp ? "tcp" : "udp");
            c.laddr = endpoint(in, v6, /*local=*/true);
            c.raddr = is_tcp ? endpoint(in, v6, /*local=*/false) : "*";
            c.state = is_tcp ? tcp_state_name(tstate) : "";
            c.pid = pid;
            connections_.push_back(std::move(c));
        }
        if (!ports.empty()) {
            std::sort(ports.begin(), ports.end());
            ports.erase(std::unique(ports.begin(), ports.end()), ports.end());
            pid_ports_[pid] = std::move(ports);
        }
    }

    // Established connections first (the interesting ones), then listeners, then
    // the rest; within a bucket keep a stable order.
    std::stable_sort(connections_.begin(), connections_.end(),
                     [](const Connection& a, const Connection& b) {
                         auto rank = [](const std::string& s) {
                             return s == "ESTABLISHED" ? 0 : s == "LISTEN" ? 1 : 2;
                         };
                         return rank(a.state) < rank(b.state);
                     });
}

}  // namespace rockbottom
