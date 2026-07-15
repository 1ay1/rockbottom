// collectors/battery.cpp — /sys/class/power_supply/BAT*, with a Termux-API
// fallback for Android/Termux where the sysfs battery nodes are absent or
// SELinux-blocked (untrusted_app cannot read /sys/class/power_supply/battery).

#include "../../sampler.hpp"
#include "procfs.hpp"

#include <array>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <unistd.h>

namespace rockbottom {

using namespace procfs;

namespace {

// Run a command and slurp its stdout. Empty string on any failure (missing
// binary, non-zero exit, etc.). Used for the termux-* CLI helpers.
std::string run_capture(const char* cmd) {
    std::string out;
    FILE* p = ::popen(cmd, "r");
    if (!p) return out;
    std::array<char, 512> buf{};
    size_t n;
    while ((n = std::fread(buf.data(), 1, buf.size(), p)) > 0)
        out.append(buf.data(), n);
    ::pclose(p);
    return out;
}

// Extract the value that follows "key" in a flat JSON object. Returns the raw
// token (number, or unquoted contents of a string) or "" if not found. Good
// enough for termux-battery-status's flat, well-formed output — no nested
// objects to worry about.
std::string json_value(const std::string& j, const char* key) {
    std::string needle = std::string("\"") + key + "\"";
    auto k = j.find(needle);
    if (k == std::string::npos) return {};
    auto colon = j.find(':', k + needle.size());
    if (colon == std::string::npos) return {};
    size_t i = colon + 1;
    while (i < j.size() && (j[i] == ' ' || j[i] == '\t')) ++i;
    if (i >= j.size()) return {};
    if (j[i] == '"') {  // string value
        auto end = j.find('"', ++i);
        if (end == std::string::npos) return {};
        return j.substr(i, end - i);
    }
    // bare number / literal — read up to comma, brace, or newline
    size_t end = i;
    while (end < j.size() && j[end] != ',' && j[end] != '}' && j[end] != '\n')
        ++end;
    return trim(j.substr(i, end - i));
}

// Termux-API path: `termux-battery-status` emits JSON with percentage,
// status, temperature. Returns true if it produced a usable reading.
bool sample_battery_termux(Battery& b) {
    // popen() itself forks a shell, so first confirm the helper exists at all
    // (a cheap access() vs a fork+exec of /bin/sh that would fail anyway).
    // Cached across calls: the CLI doesn't appear/vanish at runtime.
    static const int have_cli = [] {
        const char* p = "/data/data/com.termux/files/usr/bin/termux-battery-status";
        return ::access(p, X_OK) == 0 ? 1 : 0;
    }();
    if (!have_cli) return false;

    std::string j = run_capture("termux-battery-status 2>/dev/null");
    if (j.empty()) return false;

    std::string pct = json_value(j, "percentage");
    if (pct.empty()) pct = json_value(j, "level");
    if (pct.empty()) return false;

    b.present = true;
    b.percent = std::atoi(pct.c_str());

    std::string status = json_value(j, "status");  // CHARGING/DISCHARGING/FULL/...
    b.charging = (status == "CHARGING" || status == "FULL");

    std::string temp = json_value(j, "temperature");  // e.g. "27.8"
    if (!temp.empty()) b.temp_c = static_cast<float>(std::atof(temp.c_str()));

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
        return;
    }

    // Fallback: Termux on Android — no readable sysfs battery, ask the
    // Termux:API app via the termux-battery-status CLI.
    sample_battery_termux(b);
}

}  // namespace rockbottom
