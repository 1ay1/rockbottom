// collectors/net.cpp — /proc/net/dev, byte counters → per-second rates.

#include "../../sampler.hpp"
#include "procfs.hpp"

#include <algorithm>
#include <array>
#include <fstream>
#include <sstream>
#include <string>

namespace rockbottom {

using namespace procfs;

void Sampler::sample_net(std::vector<NetIface>& nets, double dt) {
    std::ifstream nd("/proc/net/dev");
    std::string line;
    std::getline(nd, line); std::getline(nd, line);  // two header lines
    while (std::getline(nd, line)) {
        auto colon = line.find(':');
        if (colon == std::string::npos) continue;
        std::string name = trim(line.substr(0, colon));
        std::istringstream ss(line.substr(colon + 1));
        std::array<std::uint64_t, 16> f{};
        int n = 0;
        while (n < 16 && (ss >> f[static_cast<std::size_t>(n)])) ++n;
        std::uint64_t rx = f[0], tx = f[8];

        NetIface& iface = net_hist_[name];
        iface.name = name;
        iface.up = (rx || tx || name == "lo");
        auto [prx, ptx] = prev_net_.count(name) ? prev_net_[name] : std::pair{rx, tx};
        Bytes drx{rx > prx ? rx - prx : 0}, dtx{tx > ptx ? tx - ptx : 0};
        iface.rx = first_ ? ByteRate{0} : rate(drx, dt);
        iface.tx = first_ ? ByteRate{0} : rate(dtx, dt);
        iface.rx_total = Bytes{rx};
        iface.tx_total = Bytes{tx};

        push_hist2(iface.rx_history, iface.tx_history, iface.hist_len,
                   static_cast<float>(iface.rx.per_sec),
                   static_cast<float>(iface.tx.per_sec));

        prev_net_[name] = {rx, tx};
    }

    // Surface only interfaces that have ever moved bytes, busiest first.
    // Loopback earns a row only while it's actually carrying traffic — a
    // dead-silent lo is noise, not signal.
    for (auto& [k, v] : net_hist_) {
        if (!v.rx_total.value && !v.tx_total.value) continue;
        if (k == "lo" && v.rx.per_sec + v.tx.per_sec < 1.0) continue;
        nets.push_back(v);
    }
    std::sort(nets.begin(), nets.end(), [](const NetIface& a, const NetIface& b) {
        const double ra = a.rx.per_sec + a.tx.per_sec, rb = b.rx.per_sec + b.tx.per_sec;
        if (ra != rb) return ra > rb;
        return a.name < b.name;   // stable order when rates tie
    });
}

}  // namespace rockbottom
