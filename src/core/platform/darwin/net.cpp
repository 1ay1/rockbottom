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
#include <string>

#include <ifaddrs.h>
#include <net/if.h>
#include <net/if_dl.h>
#include <sys/socket.h>

namespace rockbottom {

void Sampler::sample_net(std::vector<NetIface>& nets, double dt) {
    ifaddrs* ifap = nullptr;
    if (::getifaddrs(&ifap) != 0 || !ifap) return;

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
        auto [prx, ptx] = prev_net_.count(name) ? prev_net_[name] : std::pair{rx, tx};
        Bytes drx{rx > prx ? rx - prx : 0}, dtx{tx > ptx ? tx - ptx : 0};
        iface.rx = first_ ? ByteRate{0} : rate(drx, dt);
        iface.tx = first_ ? ByteRate{0} : rate(dtx, dt);
        iface.rx_total = Bytes{rx};
        iface.tx_total = Bytes{tx};

        sys::push_hist(iface.rx_history, iface.hist_len, static_cast<float>(iface.rx.per_sec));
        for (int i = 1; i < iface.hist_len; ++i)
            iface.tx_history[static_cast<std::size_t>(i - 1)] =
                iface.tx_history[static_cast<std::size_t>(i)];
        iface.tx_history[static_cast<std::size_t>(std::min(iface.hist_len - 1, 47))] =
            static_cast<float>(iface.tx.per_sec);

        prev_net_[name] = {rx, tx};
    }
    ::freeifaddrs(ifap);

    for (auto& [k, v] : net_hist_) {
        if (!v.rx_total.value && !v.tx_total.value) continue;
        if (k == "lo0" && v.rx.per_sec + v.tx.per_sec < 1.0) continue;
        nets.push_back(v);
    }
    std::sort(nets.begin(), nets.end(), [](const NetIface& a, const NetIface& b) {
        return (a.rx.per_sec + a.tx.per_sec) > (b.rx.per_sec + b.tx.per_sec);
    });
    if (nets.size() > 4) nets.resize(4);
}

}  // namespace rockbottom
