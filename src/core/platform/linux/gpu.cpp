// collectors/gpu.cpp — GPU telemetry. Replaces nvtop.
//
// Three back-ends, tried in order and merged into one GpuInfo list:
//   • NVIDIA — `nvidia-smi` CSV queries (the only reliable source of SM %, temp,
//     power, clocks, fan, encoder/decoder on the proprietary driver) plus a
//     compute-apps query for per-process VRAM.
//   • AMD    — /sys/class/drm/card*/device: gpu_busy_percent, mem_info_vram_*,
//     hwmon for temp / power / fan, pp_dpm_sclk/mclk for clocks.
//   • Intel  — /sys/class/drm/card*/gt_act_freq_mhz + engine busy where exposed.
//
// Anything a vendor can't report stays at its sentinel and the pane omits it.
// nvidia-smi is only spawned if the binary exists AND an NVIDIA DRM card is
// present, so machines without one pay nothing.

#include "../../sampler.hpp"
#include "procfs.hpp"

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

namespace rockbottom {

using namespace procfs;
namespace fs = std::filesystem;

namespace {

// Run a command, capture stdout. Empty string on any failure.
std::string run(const char* cmd) {
    std::string out;
    FILE* p = ::popen(cmd, "r");
    if (!p) return out;
    char buf[4096];
    while (std::fgets(buf, sizeof buf, p)) out += buf;
    ::pclose(p);
    return out;
}

// Split a CSV line into trimmed fields.
std::vector<std::string> csv(const std::string& line) {
    std::vector<std::string> f;
    std::size_t i = 0;
    while (i <= line.size()) {
        std::size_t c = line.find(',', i);
        if (c == std::string::npos) c = line.size();
        f.push_back(trim(line.substr(i, c - i)));
        i = c + 1;
    }
    return f;
}

double num(const std::string& s) {
    if (s.empty() || s == "[N/A]" || s == "N/A") return 0;
    return std::strtod(s.c_str(), nullptr);
}

bool has_nvidia_card() {
    for (int i = 0; i < 8; ++i) {
        std::string v = trim(first_line(slurp(
            "/sys/class/drm/card" + std::to_string(i) + "/device/vendor")));
        if (v == "0x10de") return true;
    }
    return false;
}

bool nvidia_smi_exists() {
    return fs::exists("/usr/bin/nvidia-smi") || fs::exists("/bin/nvidia-smi") ||
           fs::exists("/usr/local/bin/nvidia-smi");
}

// ── NVIDIA ────────────────────────────────────────────────────────────────
void collect_nvidia(std::vector<GpuInfo>& out) {
    const char* q =
        "nvidia-smi --query-gpu="
        "name,utilization.gpu,memory.used,memory.total,temperature.gpu,"
        "power.draw,power.limit,clocks.sm,clocks.mem,fan.speed,"
        "utilization.encoder,utilization.decoder,pstate,driver_version "
        "--format=csv,noheader,nounits 2>/dev/null";
    std::string res = run(q);
    if (res.empty()) return;

    std::size_t line_start = 0;
    while (line_start < res.size()) {
        std::size_t nl = res.find('\n', line_start);
        if (nl == std::string::npos) nl = res.size();
        std::string line = res.substr(line_start, nl - line_start);
        line_start = nl + 1;
        if (trim(line).empty()) continue;
        auto f = csv(line);
        if (f.size() < 14) continue;

        GpuInfo g;
        g.vendor = "NVIDIA";
        g.name = f[0];
        g.usage = Ratio{num(f[1]) / 100.0};
        g.mem_used = Bytes{static_cast<std::uint64_t>(num(f[2])) * 1024 * 1024};
        g.mem_total = Bytes{static_cast<std::uint64_t>(num(f[3])) * 1024 * 1024};
        g.mem_usage = Ratio::of(g.mem_used, g.mem_total);
        g.temp_c = static_cast<float>(num(f[4]));
        g.power_w = num(f[5]);
        g.power_limit_w = num(f[6]);
        g.core_clock = Hertz{static_cast<std::uint64_t>(num(f[7])) * 1000000ull};
        g.mem_clock = Hertz{static_cast<std::uint64_t>(num(f[8])) * 1000000ull};
        g.fan_pct = f[9].empty() || f[9] == "[N/A]" ? -1 : static_cast<int>(num(f[9]));
        g.enc_usage = Ratio{num(f[10]) / 100.0};
        g.dec_usage = Ratio{num(f[11]) / 100.0};
        g.pstate = f[12];
        g.driver = f[13];
        out.push_back(std::move(g));
    }
    if (out.empty()) return;

    // Per-process VRAM (compute + graphics apps). Attribute to the first GPU
    // since the free query doesn't tell us which card without more work.
    std::string apps = run(
        "nvidia-smi --query-compute-apps=pid,used_memory,name "
        "--format=csv,noheader,nounits 2>/dev/null");
    std::size_t ls = 0;
    while (ls < apps.size()) {
        std::size_t nl = apps.find('\n', ls);
        if (nl == std::string::npos) nl = apps.size();
        std::string line = apps.substr(ls, nl - ls);
        ls = nl + 1;
        if (trim(line).empty()) continue;
        auto f = csv(line);
        if (f.size() < 3) continue;
        GpuProc gp;
        gp.pid = static_cast<int>(num(f[0]));
        gp.mem = Bytes{static_cast<std::uint64_t>(num(f[1])) * 1024 * 1024};
        // The name field is a full path/cmdline — keep the basename word.
        std::string n = f[2];
        auto slash = n.find_last_of('/');
        if (slash != std::string::npos) n = n.substr(slash + 1);
        auto sp = n.find(' ');
        if (sp != std::string::npos) n = n.substr(0, sp);
        gp.name = n;
        out.front().procs.push_back(std::move(gp));
    }
}

// ── AMD (amdgpu sysfs) ──────────────────────────────────────────────────────
std::uint64_t read_u64(const std::string& path) {
    std::string s = trim(first_line(slurp(path)));
    return s.empty() ? 0 : std::strtoull(s.c_str(), nullptr, 10);
}

// Parse the active clock ("N: 1234Mhz *") from pp_dpm_sclk/mclk.
std::uint64_t active_clock_mhz(const std::string& path) {
    std::string s = slurp(path);
    std::size_t star = s.find('*');
    if (star == std::string::npos) return 0;
    std::size_t nl = s.rfind('\n', star);
    std::size_t begin = nl == std::string::npos ? 0 : nl + 1;
    std::string line = s.substr(begin, star - begin);
    // e.g. "1: 1234Mhz"
    std::size_t colon = line.find(':');
    if (colon != std::string::npos) line = line.substr(colon + 1);
    return static_cast<std::uint64_t>(std::strtod(trim(line).c_str(), nullptr));
}

void collect_amd(std::vector<GpuInfo>& out) {
    for (int i = 0; i < 8; ++i) {
        std::string dev = "/sys/class/drm/card" + std::to_string(i) + "/device";
        std::string vendor = trim(first_line(slurp(dev + "/vendor")));
        if (vendor != "0x1002") continue;                 // AMD PCI vendor
        if (!fs::exists(dev + "/gpu_busy_percent")) continue;

        GpuInfo g;
        g.vendor = "AMD";
        g.name = "AMD GPU";   // amdgpu doesn't expose a marketing name in sysfs
        std::string busy = trim(first_line(slurp(dev + "/gpu_busy_percent")));
        g.usage = Ratio{num(busy) / 100.0};
        g.mem_used = Bytes{read_u64(dev + "/mem_info_vram_used")};
        g.mem_total = Bytes{read_u64(dev + "/mem_info_vram_total")};
        g.mem_usage = Ratio::of(g.mem_used, g.mem_total);
        g.core_clock = Hertz{active_clock_mhz(dev + "/pp_dpm_sclk") * 1000000ull};
        g.mem_clock = Hertz{active_clock_mhz(dev + "/pp_dpm_mclk") * 1000000ull};

        // hwmon: temp1_input (millideg), power1_average (microwatt), fan.
        for (const auto& e : fs::directory_iterator(dev + "/hwmon",
                                                    fs::directory_options::skip_permission_denied)) {
            std::string h = e.path().string();
            std::uint64_t t = read_u64(h + "/temp1_input");
            if (t) g.temp_c = static_cast<float>(t) / 1000.0f;
            std::uint64_t p = read_u64(h + "/power1_average");
            if (p) g.power_w = static_cast<double>(p) / 1e6;
            std::uint64_t cap = read_u64(h + "/power1_cap");
            if (cap) g.power_limit_w = static_cast<double>(cap) / 1e6;
            std::uint64_t f1 = read_u64(h + "/fan1_input");
            std::uint64_t fmax = read_u64(h + "/fan1_max");
            if (f1 && fmax) g.fan_pct = static_cast<int>(f1 * 100 / fmax);
            break;
        }
        out.push_back(std::move(g));
    }
}

// ── Intel (i915) ────────────────────────────────────────────────────────────
void collect_intel(std::vector<GpuInfo>& out) {
    for (int i = 0; i < 8; ++i) {
        std::string card = "/sys/class/drm/card" + std::to_string(i);
        std::string dev = card + "/device";
        std::string vendor = trim(first_line(slurp(dev + "/vendor")));
        if (vendor != "0x8086") continue;                 // Intel PCI vendor
        if (!fs::exists(card + "/gt_act_freq_mhz")) continue;

        GpuInfo g;
        g.vendor = "Intel";
        g.name = "Intel GPU";
        g.core_clock = Hertz{read_u64(card + "/gt_act_freq_mhz") * 1000000ull};
        // i915 doesn't export a simple busy% in sysfs; leave usage at 0 so the
        // pane shows clocks + whatever hwmon gives us rather than a fake load.
        for (const auto& e : fs::directory_iterator(dev + "/hwmon",
                                                    fs::directory_options::skip_permission_denied)) {
            std::string h = e.path().string();
            std::uint64_t t = read_u64(h + "/temp1_input");
            if (t) g.temp_c = static_cast<float>(t) / 1000.0f;
            std::uint64_t p = read_u64(h + "/energy1_input");   // some report energy only
            (void)p;
            break;
        }
        out.push_back(std::move(g));
    }
}

}  // namespace

void Sampler::sample_gpu(std::vector<GpuInfo>& gpus) {
    gpus.clear();

    if (nvidia_smi_exists() && has_nvidia_card())
        collect_nvidia(gpus);
    collect_amd(gpus);
    collect_intel(gpus);

    // Attach rolling history per GPU index (survives across ticks).
    for (std::size_t i = 0; i < gpus.size(); ++i) {
        int idx = static_cast<int>(i);
        auto& rings = gpu_hist_[idx];
        int& len = gpu_hist_len_[idx];
        push_hist(rings.first, len, static_cast<float>(gpus[i].usage.v));
        int mlen = len;   // mem ring advances in lockstep
        push_hist(rings.second, mlen, static_cast<float>(gpus[i].mem_usage.v));
        gpus[i].util_history = rings.first;
        gpus[i].mem_history = rings.second;
        gpus[i].hist_len = len;
        gpus[i].mem_hist_len = len;

        // Keep only the top VRAM consumers, biggest first.
        auto& ps = gpus[i].procs;
        std::sort(ps.begin(), ps.end(),
                  [](const GpuProc& a, const GpuProc& b) { return a.mem.value > b.mem.value; });
        if (ps.size() > 16) ps.resize(16);
    }
}

}  // namespace rockbottom
