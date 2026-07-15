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

#include <cstdlib>
#include <string>

namespace rockbottom {

void Sampler::sample_wireless(Wireless& w) {
    if (!termux::available()) return;   // desktop Linux: nothing to do

    // ── WiFi ──
    if (std::string j = termux::run("termux-wifi-connectioninfo"); !j.empty()) {
        std::string ssid = termux::json_value(j, "ssid");
        // A disconnected radio reports ssid "<unknown ssid>" or empty; treat
        // those as "no wifi" so the pane doesn't show a junk row.
        const bool connected = !ssid.empty() && ssid.front() != '<' &&
                               ssid != "null";
        if (connected) {
            w.wifi_present = true;
            w.ssid      = ssid;
            w.wifi_rssi = static_cast<int>(termux::json_number(j, "rssi"));
            w.link_mbps = static_cast<int>(termux::json_number(j, "link_speed_mbps"));
            if (w.link_mbps == 0)  // key name varies across API versions
                w.link_mbps = static_cast<int>(termux::json_number(j, "link_speed"));
            w.wifi_freq = static_cast<int>(termux::json_number(j, "frequency_mhz"));
            if (w.wifi_freq == 0)
                w.wifi_freq = static_cast<int>(termux::json_number(j, "frequency"));
            w.ip = termux::json_value(j, "ip");
        }
    }

    // ── Cellular ──
    if (std::string j = termux::run("termux-telephony-deviceinfo"); !j.empty()) {
        std::string op = termux::json_value(j, "network_operator_name");
        std::string type = termux::json_value(j, "data_network_type");
        std::string state = termux::json_value(j, "data_state");
        if (!op.empty() || !type.empty()) {
            w.cell_present   = true;
            w.operator_name  = op;
            w.net_type       = type;
            w.data_state     = state;
        }
    }
}

}  // namespace rockbottom
