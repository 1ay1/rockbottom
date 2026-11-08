// collectors/wireless.cpp — WiFi + cellular status via Termux:API.
//
// Desktop Linux surfaces link state through /sys/class/net + the net collector
// already; there's no portable "signal strength" there, so on non-Termux
// systems this collector is a no-op and Wireless stays empty. On Android the
// Termux:API helpers report what the platform exposes:
//
//   • termux-wifi-connectioninfo — ssid, rssi, link_speed_mbps, frequency_mhz, ip
//   • termux-telephony-deviceinfo — network_operator_name, data_network_type,
//                                    data_state
//
// Some Termux builds (notably the Play-Store variant) don't implement the wifi
// / telephony helpers; termux::run() returns "" for a missing helper and each
// slice is simply left blank. Both helpers spawn a process, so the sampler
// throttles this collector to a slow wall-clock cadence.

#include "../../sampler.hpp"
#include "termux.hpp"
#include "nl80211.hpp"

#include <cstdlib>
#include <string>

namespace rockbottom {

// Plain Linux: the kernel's wireless table, one line per associated interface
// ("wlp5s0: 0000   64.  -46.  -256 ..."): link quality, then signal in dBm.
// Link speed and band come from iw/nl80211, which we don't speak; the panes
// show what's known. Takes the strongest interface.
void Sampler::sample_wireless_proc(Wireless& w) {
    const std::string t = procfs::slurp("/proc/net/wireless");
    std::size_t at = 0;
    for (int skip = 0; skip < 2 && at != std::string::npos; ++skip)
        if ((at = t.find('\n', at)) != std::string::npos) ++at;
    while (at != std::string::npos && at < t.size()) {
        const std::size_t nl = t.find('\n', at);
        const std::string line = t.substr(at, nl == std::string::npos ? std::string::npos : nl - at);
        at = nl == std::string::npos ? nl : nl + 1;
        const auto colon = line.find(':');
        if (colon == std::string::npos) continue;
        int status = 0;
        double quality = 0, level = 0;
        if (std::sscanf(line.c_str() + colon + 1, "%x %lf %lf", &status, &quality, &level) != 3)
            continue;
        const int dbm = static_cast<int>(level);
        if (dbm >= 0 || dbm < -120) continue;          // not associated
        if (w.wifi_present && dbm <= w.wifi_rssi) continue;
        w.wifi_present = true;
        w.wifi_rssi = dbm;
        w.iface = sys::sanitize_display(procfs::trim(line.substr(0, colon)));
    }
    // name, band and speed: nl80211, as `iw dev <if> link` reads them
    if (!w.iface.empty()) {
        nl80211::Link l;
        if (nl80211::link(w.iface, l)) {
            if (!l.ssid.empty()) w.ssid = sys::sanitize_display(l.ssid);
            w.wifi_freq = l.freq_mhz;
            w.link_mbps = std::max(l.tx_mbps, l.rx_mbps);
            if (l.signal_dbm) w.wifi_rssi = l.signal_dbm;
        }
    }
}

void Sampler::sample_wireless(Wireless& w) {
    if (!termux::available()) {
        sample_wireless_proc(w);
        return;
    }

    // ── WiFi ──
    if (std::string j = termux::run("termux-wifi-connectioninfo"); !j.empty()) {
        std::string ssid = termux::json_value(j, "ssid");
        // A disconnected radio reports ssid "<unknown ssid>" or empty; treat
        // those as "no wifi" so the pane doesn't show a junk row.
        const bool connected = !ssid.empty() && ssid.front() != '<' &&
                               ssid != "null";
        if (connected) {
            w.wifi_present = true;
            // SSID is broadcast by the access point — externally controlled and
            // free-form; strip terminal-control bytes before it's drawn.
            w.ssid      = sys::sanitize_display(ssid);
            w.wifi_rssi = static_cast<int>(termux::json_number(j, "rssi"));
            w.link_mbps = static_cast<int>(termux::json_number(j, "link_speed_mbps"));
            if (w.link_mbps == 0)  // key name varies across API versions
                w.link_mbps = static_cast<int>(termux::json_number(j, "link_speed"));
            w.wifi_freq = static_cast<int>(termux::json_number(j, "frequency_mhz"));
            if (w.wifi_freq == 0)
                w.wifi_freq = static_cast<int>(termux::json_number(j, "frequency"));
            w.ip = sys::sanitize_display(termux::json_value(j, "ip"));
        }
    }

    // ── Cellular ──
    if (std::string j = termux::run("termux-telephony-deviceinfo"); !j.empty()) {
        std::string op = termux::json_value(j, "network_operator_name");
        std::string type = termux::json_value(j, "data_network_type");
        std::string state = termux::json_value(j, "data_state");
        if (!op.empty() || !type.empty()) {
            w.cell_present   = true;
            // Operator name comes from the carrier network — untrusted.
            w.operator_name  = sys::sanitize_display(op);
            w.net_type       = sys::sanitize_display(type);
            w.data_state     = state;
        }
    }
}

}  // namespace rockbottom
