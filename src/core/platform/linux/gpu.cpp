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
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <map>
#include <string>
#include <string_view>
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

    // Per-process VRAM. Query BOTH compute apps (CUDA/OpenCL) and graphics apps
    // (games, browsers, compositors, video players) — the compute-apps query
    // alone misses every non-CUDA GPU user, which is most of them on a desktop.
    // Attribute to the first GPU since these free queries don't name the card.
    auto& procs = out.front().procs;
    auto ingest_apps = [&](const char* query, char type) {
        std::string apps = run(query);
        std::size_t ls = 0;
        while (ls < apps.size()) {
            std::size_t nl = apps.find('\n', ls);
            if (nl == std::string::npos) nl = apps.size();
            std::string line = apps.substr(ls, nl - ls);
            ls = nl + 1;
            if (trim(line).empty()) continue;
            auto f = csv(line);
            if (f.size() < 3) continue;
            const int pid = static_cast<int>(num(f[0]));
            if (pid <= 0) continue;
            const std::uint64_t mem =
                static_cast<std::uint64_t>(num(f[1])) * 1024 * 1024;
            // The name field is a full path/cmdline — keep the basename word.
            std::string n = f[2];
            auto slash = n.find_last_of('/');
            if (slash != std::string::npos) n = n.substr(slash + 1);
            auto sp = n.find(' ');
            if (sp != std::string::npos) n = n.substr(0, sp);
            // Merge: a pid can appear in BOTH lists (uses compute AND graphics)
            // — keep one row, sum VRAM, mark type 'B'.
            auto it = std::find_if(procs.begin(), procs.end(),
                                   [&](const GpuProc& g) { return g.pid == pid; });
            if (it != procs.end()) {
                it->mem = Bytes{it->mem.value + mem};
                if (it->type != type) it->type = 'B';
                if (it->name.empty() && !n.empty()) it->name = n;
            } else {
                GpuProc gp;
                gp.pid = pid;
                gp.mem = Bytes{mem};
                gp.name = n;
                gp.type = type;
                procs.push_back(std::move(gp));
            }
        }
    };
    ingest_apps("nvidia-smi --query-compute-apps=pid,used_memory,name "
                "--format=csv,noheader,nounits 2>/dev/null", 'C');
    ingest_apps("nvidia-smi --query-accounted-apps=pid,used_memory,name "
                "--format=csv,noheader,nounits 2>/dev/null", 'C');
    // Graphics apps aren't a --query-*; nvidia-smi lists them in the process
    // table. pmon (below) is the reliable per-process source and also carries
    // sm/enc/dec utilisation, so fold graphics + util in from a single pmon
    // sample.

    // ── nvidia-smi pmon: one sample, per-process sm%/mem/enc/dec ────────────
    // Columns: gpu pid type sm mem enc dec [jpg ofa] command. Older drivers
    // omit the trailing engines; parse defensively by header where possible.
    {
        std::string pm = run("nvidia-smi pmon -c 1 2>/dev/null");
        std::size_t ls = 0;
        while (ls < pm.size()) {
            std::size_t nl = pm.find('\n', ls);
            if (nl == std::string::npos) nl = pm.size();
            std::string line = pm.substr(ls, nl - ls);
            ls = nl + 1;
            std::string t = trim(line);
            if (t.empty() || t[0] == '#') continue;   // header/comment rows
            // Whitespace-split.
            std::vector<std::string> col;
            std::size_t i = 0;
            while (i < t.size()) {
                while (i < t.size() && std::isspace((unsigned char)t[i])) ++i;
                std::size_t j = i;
                while (j < t.size() && !std::isspace((unsigned char)t[j])) ++j;
                if (j > i) col.push_back(t.substr(i, j - i));
                i = j;
            }
            if (col.size() < 4) continue;
            const int pid = static_cast<int>(num(col[1]));
            if (pid <= 0) continue;   // '-' pids (idle) parse to 0
            const char ptype = col.size() > 2 && !col[2].empty() ? col[2][0] : '?';
            // sm mem enc dec are cols 3..6 on the common layout; '-' -> 0.
            auto pc = [&](std::size_t k) -> double {
                return k < col.size() && col[k] != "-" ? num(col[k]) / 100.0 : 0;
            };
            const double sm = pc(3), enc = pc(5), dec = pc(6);
            // pmon col 4 = fb (framebuffer) MB used by this pid — the only
            // VRAM source for graphics apps that never hit a --query list.
            const std::uint64_t fb_mb =
                col.size() > 4 && col[4] != "-"
                    ? static_cast<std::uint64_t>(num(col[4])) : 0;
            std::string cmd = col.empty() ? "" : col.back();
            auto it = std::find_if(procs.begin(), procs.end(),
                                   [&](const GpuProc& g) { return g.pid == pid; });
            if (it == procs.end()) {
                GpuProc gp;
                gp.pid = pid;
                gp.name = cmd;
                gp.type = ptype == 'C' || ptype == 'G' ? ptype : '?';
                procs.push_back(std::move(gp));
                it = std::prev(procs.end());
            }
            it->sm = Ratio{sm};
            it->enc = Ratio{enc};
            it->dec = Ratio{dec};
            it->has_util = true;
            if (it->mem.value == 0 && fb_mb)
                it->mem = Bytes{fb_mb * 1024 * 1024};
            if (it->name.empty()) it->name = cmd;
            if (it->type == '?' && (ptype == 'C' || ptype == 'G')) it->type = ptype;
        }
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
        // Newer amdgpu exposes the marketing name; fall back to the generic.
        g.name = trim(first_line(slurp(dev + "/product_name")));
        if (g.name.empty()) g.name = "AMD GPU";
        g.driver = trim(first_line(slurp("/sys/module/amdgpu/version")));
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
        g.driver = trim(first_line(slurp("/sys/module/i915/version")));
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

// ── DRM fdinfo: per-process VRAM + engine time (AMD, Intel, any modern DRM) ──
// The kernel exposes each process's GPU usage through /proc/<pid>/fdinfo/<fd>
// on any open DRM device. Keys of interest (present since ~5.14, universal on
// amdgpu/i915/xe):
//   drm-driver: amdgpu | i915 | ...
//   drm-memory-vram / drm-total-vram: bytes of VRAM this fd holds
//   drm-engine-<class>: nanoseconds of engine time (cumulative)
// One process opens the DRM node many times (many fds) — we take the MAX vram
// across its fds (they alias the same allocation), not the sum. This is how
// nvtop attributes AMD/Intel apps, and it needs no root.
struct FdinfoProc {
    std::uint64_t vram = 0;
    std::uint64_t gfx_ns = 0;   // summed engine-time across classes
    std::string   name;
};

void scan_drm_fdinfo(const std::string& driver_match,
                     std::map<int, FdinfoProc>& acc) {
    std::error_code ec;
    for (const auto& pe : fs::directory_iterator("/proc", ec)) {
        if (ec) break;
        const std::string base = pe.path().filename().string();
        if (base.empty() || !std::isdigit((unsigned char)base[0])) continue;
        const int pid = std::atoi(base.c_str());
        if (pid <= 0) continue;
        const std::string fddir = pe.path().string() + "/fdinfo";
        std::error_code ec2;
        auto dir = fs::directory_iterator(fddir, fs::directory_options::skip_permission_denied, ec2);
        if (ec2) continue;
        FdinfoProc found;
        bool matched = false;
        for (const auto& fe : dir) {
            std::string body = slurp(fe.path().string());
            if (body.find("drm-driver") == std::string::npos) continue;
            if (body.find(driver_match) == std::string::npos) continue;
            matched = true;
            // Parse the small key: value block line by line.
            std::uint64_t vram = 0, eng = 0;
            std::size_t p = 0;
            while (p < body.size()) {
                std::size_t nl = body.find('\n', p);
                if (nl == std::string::npos) nl = body.size();
                std::string_view ln(body.data() + p, nl - p);
                p = nl + 1;
                auto starts = [&](const char* k) {
                    return ln.size() > std::strlen(k) &&
                           ln.compare(0, std::strlen(k), k) == 0;
                };
                auto val_u64 = [&]() -> std::uint64_t {
                    std::size_t c = ln.find(':');
                    if (c == std::string::npos) return 0;
                    return std::strtoull(std::string(ln.substr(c + 1)).c_str(), nullptr, 10);
                };
                if (starts("drm-memory-vram") || starts("drm-total-vram"))
                    vram = std::max(vram, val_u64() * 1024);   // reported in KiB
                else if (starts("drm-engine-"))
                    eng += val_u64();
            }
            found.vram = std::max(found.vram, vram);
            found.gfx_ns += eng;
        }
        if (!matched) continue;
        if (found.name.empty())
            found.name = trim(first_line(slurp(pe.path().string() + "/comm")));
        acc[pid] = found;
    }
}

}  // namespace

void Sampler::sample_gpu(std::vector<GpuInfo>& gpus) {
    gpus.clear();

    // FAST PATH: probe for any supported GPU (NVIDIA smi, or an AMD/Intel DRM
    // card) exactly ONCE and cache the verdict. On machines with none — phones
    // (Mali/Adreno), headless boxes, VMs — the AMD+Intel DRM directory walks
    // would otherwise run every single tick and find nothing, burning CPU that
    // scales up as the refresh rate rises. Static init is thread-safe (this
    // collector only runs on the sampler thread anyway).
    static const bool have_gpu = [] {
        if (nvidia_smi_exists() && has_nvidia_card()) return true;
        std::error_code ec;
        for (int i = 0; i < 8; ++i) {
            std::string dev = "/sys/class/drm/card" + std::to_string(i) + "/device";
            if (fs::exists(dev + "/gpu_busy_percent", ec)) return true;  // amdgpu
            if (fs::exists(dev + "/gt_act_freq_mhz", ec)) return true;   // i915/xe
        }
        return false;
    }();
    if (!have_gpu) return;

    if (nvidia_smi_exists() && has_nvidia_card())
        collect_nvidia(gpus);
    collect_amd(gpus);
    collect_intel(gpus);

    // AMD / Intel per-process VRAM via DRM fdinfo (nvidia already has its own
    // apps list from pmon/query). Attribute to the first matching-vendor GPU.
    for (auto& g : gpus) {
        if (g.vendor != "AMD" && g.vendor != "Intel") continue;
        if (!g.procs.empty()) continue;
        const char* drv = g.vendor == "AMD" ? "amdgpu" : "i915";
        std::map<int, FdinfoProc> acc;
        scan_drm_fdinfo(drv, acc);
        if (acc.empty() && g.vendor == "Intel") { scan_drm_fdinfo("xe", acc); }
        for (auto& [pid, fp] : acc) {
            if (fp.vram == 0 && fp.gfx_ns == 0) continue;
            GpuProc gp;
            gp.pid = pid;
            gp.name = fp.name;
            gp.mem = Bytes{fp.vram};
            gp.type = 'G';
            g.procs.push_back(std::move(gp));
        }
        break;   // one integrated/discrete card owns the fdinfo attribution
    }

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

        // Keep the top GPU processes. Sort by live GPU-compute % first (the
        // apps actually working the card), then by VRAM held — so a busy
        // process outranks a big-but-idle one, and idle VRAM hogs still show.
        auto& ps = gpus[i].procs;
        std::sort(ps.begin(), ps.end(), [](const GpuProc& a, const GpuProc& b) {
            const double au = a.has_util ? a.sm.v : -1;
            const double bu = b.has_util ? b.sm.v : -1;
            if ((au > 0.005) != (bu > 0.005)) return au > bu;   // any-work first
            if (std::abs(au - bu) > 0.01)     return au > bu;    // then by work
            return a.mem.value > b.mem.value;                    // then by VRAM
        });
        if (ps.size() > 16) ps.resize(16);
    }
}

}  // namespace rockbottom
