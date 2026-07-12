// platform/darwin/net.cpp — getifaddrs(AF_LINK) byte counters → rates.
//
// The macOS analogue of /proc/net/dev. Each AF_LINK entry carries an
// `if_data` block whose ifi_ibytes / ifi_obytes are cumulative byte counters
// for that interface — we delta them to per-second rates and shape the list
// (busiest first, keep lo only while it's active, top 4) identically to the
// Linux backend so the UI is byte-for-byte the same.

#include "../../sampler.hpp"
#include "mach_util.hpp"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <string>

#include <arpa/inet.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <net/if_dl.h>
#include <netinet/in.h>
#include <sys/socket.h>

namespace rockbottom {

void Sampler::sample_net(std::vector<NetIface>& nets, double dt) {
    ifaddrs* ifap = nullptr;
    if (::getifaddrs(&ifap) != 0 || !ifap) return;

    // First pass: link-layer entries carry the counters + MAC + MTU.
    for (ifaddrs* p = ifap; p; p = p->ifa_next) {
        if (!p->ifa_addr || p->ifa_addr->sa_family != AF_LINK || !p->ifa_data) continue;
        auto* d = reinterpret_cast<if_data*>(p->ifa_data);
        std::string name = p->ifa_name ? p->ifa_name : "";
        if (name.empty()) continue;

        std::uint64_t rx = d->ifi_ibytes, tx = d->ifi_obytes;

        NetIface& iface = net_hist_[name];
        iface.name = name;
        iface.up = ((p->ifa_flags & IFF_UP) && (p->ifa_flags & IFF_RUNNING)) ||
                   name == "lo0";
        iface.mtu = static_cast<int>(d->ifi_mtu);
        iface.rx_packets = d->ifi_ipackets;
        iface.tx_packets = d->ifi_opackets;
        iface.rx_errs = d->ifi_ierrors;
        iface.tx_errs = d->ifi_oerrors;
        iface.drops   = d->ifi_iqdrops;

        // Link-layer (MAC) address from the sockaddr_dl.
        auto* sdl = reinterpret_cast<sockaddr_dl*>(p->ifa_addr);
        if (sdl->sdl_alen == 6) {
            const auto* a = reinterpret_cast<const unsigned char*>(LLADDR(sdl));
            char macbuf[18];
            std::snprintf(macbuf, sizeof macbuf, "%02x:%02x:%02x:%02x:%02x:%02x",
                          a[0], a[1], a[2], a[3], a[4], a[5]);
            iface.mac = macbuf;
        }

        auto [prx, ptx] = prev_net_.count(name) ? prev_net_[name] : std::pair{rx, tx};
        Bytes drx{rx > prx ? rx - prx : 0}, dtx{tx > ptx ? tx - ptx : 0};
        iface.rx = first_ ? ByteRate{0} : rate(drx, dt);
        iface.tx = first_ ? ByteRate{0} : rate(dtx, dt);
        iface.rx_total = Bytes{rx};
        iface.tx_total = Bytes{tx};

        // Packet rates from the same delta discipline as bytes.
        auto [ppr, ppt] = prev_net_pkts_.count(name)
                              ? prev_net_pkts_[name]
                              : std::pair{iface.rx_packets, iface.tx_packets};
        iface.rx_pps = first_ || dt <= 0 ? 0
            : static_cast<double>(iface.rx_packets > ppr ? iface.rx_packets - ppr : 0) / dt;
        iface.tx_pps = first_ || dt <= 0 ? 0
            : static_cast<double>(iface.tx_packets > ppt ? iface.tx_packets - ppt : 0) / dt;
        prev_net_pkts_[name] = {iface.rx_packets, iface.tx_packets};

        sys::push_hist2(iface.rx_history, iface.tx_history, iface.hist_len,
                        static_cast<float>(iface.rx.per_sec),
                        static_cast<float>(iface.tx.per_sec));

        prev_net_[name] = {rx, tx};
    }

    // Second pass: first IPv4 address per interface.
    for (ifaddrs* p = ifap; p; p = p->ifa_next) {
        if (!p->ifa_addr || p->ifa_addr->sa_family != AF_INET) continue;
        std::string name = p->ifa_name ? p->ifa_name : "";
        auto it = net_hist_.find(name);
        if (it == net_hist_.end() || !it->second.ip4.empty()) continue;
        char buf[INET_ADDRSTRLEN] = {};
        auto* sin = reinterpret_cast<sockaddr_in*>(p->ifa_addr);
        if (::inet_ntop(AF_INET, &sin->sin_addr, buf, sizeof buf)) it->second.ip4 = buf;
    }
    ::freeifaddrs(ifap);

    for (auto& [k, v] : net_hist_) {
        if (!v.rx_total.value && !v.tx_total.value) continue;
        if (k == "lo0" && v.rx.per_sec + v.tx.per_sec < 1.0) continue;
        nets.push_back(v);
    }
    std::sort(nets.begin(), nets.end(), [](const NetIface& a, const NetIface& b) {
        const double ra = a.rx.per_sec + a.tx.per_sec, rb = b.rx.per_sec + b.tx.per_sec;
        if (ra != rb) return ra > rb;
        return a.name < b.name;   // stable, physical (en*) before utun*
    });
}

}  // namespace rockbottom
