// collectors/battery.cpp — /sys/class/power_supply/BAT*.

#include "../../sampler.hpp"
#include "procfs.hpp"

#include <cstdlib>
#include <string>

namespace rockbottom {

using namespace procfs;

void Sampler::sample_battery(Battery& b) {
    for (int i = 0; i < 4; ++i) {
        std::string base = "/sys/class/power_supply/BAT" + std::to_string(i);
        std::string cap = first_line(slurp(base + "/capacity"));
        if (cap.empty()) continue;
        b.present = true;
        b.percent = std::atoi(cap.c_str());
        std::string status = trim(first_line(slurp(base + "/status")));
        b.charging = (status == "Charging" || status == "Full");
        return;
    }
}

}  // namespace rockbottom
