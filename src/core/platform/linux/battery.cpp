// collectors/battery.cpp — /sys/class/power_supply/BAT*, with a Termux-API
// fallback for Android/Termux where the sysfs battery nodes are absent or
// SELinux-blocked (untrusted_app cannot read /sys/class/power_supply/battery).

#include "../../sampler.hpp"
#include "procfs.hpp"
#include "termux.hpp"

#include <cstdlib>
#include <string>

namespace rockbottom {

using namespace procfs;

namespace {

// Termux-API path: `termux-battery-status` emits a flat JSON object with
// percentage, status, health, plugged, technology, temperature, current, and
// cycle. Returns true if it produced a usable reading. See termux.hpp for the
// shared availability check + JSON scraper (never forks when the CLI is gone).
bool sample_battery_termux(Battery& b) {
    std::string j = termux::run("termux-battery-status");
    if (j.empty()) return false;

    std::string pct = termux::json_value(j, "percentage");
    if (pct.empty()) pct = termux::json_value(j, "level");
    if (pct.empty()) return false;

    b.present = true;
    b.percent = std::atoi(pct.c_str());

    std::string status = termux::json_value(j, "status");  // CHARGING/DISCHARGING/FULL/…
    b.charging = (status == "CHARGING" || status == "FULL");

    std::string temp = termux::json_value(j, "temperature");
    if (!temp.empty()) b.temp_c = static_cast<float>(std::atof(temp.c_str()));

    // Rich extras (all optional — absent keys just stay at their sentinel).
    b.health = termux::json_value(j, "health");
    b.tech   = termux::json_value(j, "technology");
    std::string plugged = termux::json_value(j, "plugged");  // "PLUGGED_AC" / "UNPLUGGED" / …
    if (plugged.rfind("PLUGGED_", 0) == 0) b.plug = plugged.substr(8);
    else                                    b.plug.clear();
    // Termux reports current in microamps; normalise to milliamps.
    std::string cur = termux::json_value(j, "current");
    if (!cur.empty()) b.current_ma = std::strtod(cur.c_str(), nullptr) / 1000.0;
    std::string cyc = termux::json_value(j, "cycle");
    if (!cyc.empty()) b.cycles = std::atoi(cyc.c_str());

    return true;
}

}  // namespace

void Sampler::sample_battery(Battery& b) {
    // Preferred: standard Linux sysfs power-supply nodes.
    for (int i = 0; i < 4; ++i) {
        std::string base = "/sys/class/power_supply/BAT" + std::to_string(i);
        std::string cap = first_line(slurp(base + "/capacity"));
        if (cap.empty()) continue;
        b.present = true;
        b.percent = std::atoi(cap.c_str());
        std::string status = trim(first_line(slurp(base + "/status")));
        b.charging = (status == "Charging" || status == "Full");
        // Battery temp is exposed in deci-Celsius on many Linux platforms.
        std::string t = first_line(slurp(base + "/temp"));
        if (!t.empty()) b.temp_c = std::atoi(t.c_str()) / 10.0f;
        // Rich sysfs extras where present.
        std::string cyc = first_line(slurp(base + "/cycle_count"));
        if (!cyc.empty()) b.cycles = std::atoi(cyc.c_str());
        b.health = trim(first_line(slurp(base + "/health")));
        b.tech   = trim(first_line(slurp(base + "/technology")));
        // current_now is microamps (sign varies by platform); to milliamps.
        std::string cur = first_line(slurp(base + "/current_now"));
        if (!cur.empty()) b.current_ma = std::atof(cur.c_str()) / 1000.0;
        return;
    }

    // Fallback: Termux on Android — no readable sysfs battery, ask the
    // Termux:API app via the termux-battery-status CLI.
    sample_battery_termux(b);
}

}  // namespace rockbottom
