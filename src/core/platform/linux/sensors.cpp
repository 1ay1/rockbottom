// platform/linux/sensors.cpp — hardware temperatures from /sys/class/hwmon.
//
// The canonical Linux sensor source (what `sensors` / lm-sensors reads). Each
// hwmon device exposes a "name" plus tempN_input (milli-°C), optional
// tempN_label / tempN_max / tempN_crit. We surface every readable temp, tagged
// with a coarse zone (cpu/nvme/drive/acpi/…) so the UI can group them. Idle
// cost is a handful of small sysfs reads; nothing spawns.

#include "../../sampler.hpp"
#include "procfs.hpp"

#include <algorithm>
#include <cstdlib>
#include <dirent.h>
#include <fstream>
#include <string>

namespace rockbottom {

namespace {

using procfs::slurp;
using procfs::trim;

float read_milli_c(const std::string& path) {
    std::string v = trim(slurp(path));
    if (v.empty()) return 0;
    return static_cast<float>(std::strtod(v.c_str(), nullptr) / 1000.0);
}

// Coarse grouping from the hwmon device "name" so the UI can bucket sensors.
std::string zone_of(const std::string& name) {
    std::string n = name;
    for (char& c : n) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    if (n.find("coretemp") != std::string::npos || n.find("k10temp") != std::string::npos ||
        n.find("zenpower") != std::string::npos || n.find("cpu") != std::string::npos)
        return "cpu";
    if (n.find("nvme") != std::string::npos)      return "nvme";
    if (n.find("drivetemp") != std::string::npos) return "drive";
    if (n.find("acpitz") != std::string::npos)    return "acpi";
    if (n.find("bat") != std::string::npos)       return "battery";
    if (n.find("pch") != std::string::npos || n.find("chipset") != std::string::npos)
        return "chipset";
    if (n.find("wifi") != std::string::npos || n.find("iwlwifi") != std::string::npos)
        return "wifi";
    return n.empty() ? "other" : n;
}

}  // namespace

void Sampler::sample_sensors(std::vector<Sensor>& out) {
    out.clear();
    DIR* d = ::opendir("/sys/class/hwmon");
    if (!d) return;

    dirent* e;
    while ((e = ::readdir(d)) != nullptr) {
        if (e->d_name[0] == '.') continue;
        std::string base = std::string("/sys/class/hwmon/") + e->d_name;
        std::string dev_name = trim(slurp(base + "/name"));
        std::string zone = zone_of(dev_name);

        // tempN_input, N = 1.. — but STOP at the first run of misses rather
        // than probing a fixed 1..32. hwmon numbers its inputs contiguously
        // from 1, so the old loop spent 32 open() calls per device (most on
        // files that do not exist) every pass. A short miss tolerance keeps us
        // safe against the rare driver that leaves a gap.
        int misses = 0;
        for (int i = 1; i <= 32 && misses < 4; ++i) {
            std::string stem = base + "/temp" + std::to_string(i);
            const float t = read_milli_c(stem + "_input");
            if (t <= 0) { ++misses; continue; }   // absent OR unreadable
            misses = 0;
            if (t > 200) continue;                 // implausible → skip

            std::string label = trim(slurp(stem + "_label"));
            if (label.empty())
                label = dev_name.empty() ? ("temp" + std::to_string(i)) : dev_name;

            Sensor s;
            // hwmon labels and device names are driver-supplied sysfs strings
            // that reach the sensors pane; sanitize at the data boundary like
            // every other displayed string.
            s.label  = procfs::sanitize_display(label);
            s.zone   = zone;
            s.temp_c = t;
            s.high_c = read_milli_c(stem + "_max");
            s.crit_c = read_milli_c(stem + "_crit");
            out.push_back(std::move(s));
        }
    }
    ::closedir(d);

    // Group by zone, hottest first within a zone, so the UI reads top-down.
    std::stable_sort(out.begin(), out.end(), [](const Sensor& a, const Sensor& b) {
        if (a.zone != b.zone) return a.zone < b.zone;
        return a.temp_c > b.temp_c;
    });
}

}  // namespace rockbottom
