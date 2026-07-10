// sampler.cpp — procfs/sysfs implementation of the metrics sampler.

#include "sampler.hpp"

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstring>
#include <dirent.h>
#include <fstream>
#include <pwd.h>
#include <sstream>
#include <string>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <sys/utsname.h>
#include <unistd.h>

namespace bottom {
namespace {

std::string slurp(const char* path) {
    std::ifstream f(path);
    if (!f) return {};
    std::stringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

std::string first_line(const std::string& s) {
    auto nl = s.find('\n');
    return nl == std::string::npos ? s : s.substr(0, nl);
}

std::string trim(std::string s) {
    auto a = s.find_first_not_of(" \t\r\n");
    auto b = s.find_last_not_of(" \t\r\n");
    return a == std::string::npos ? std::string{} : s.substr(a, b - a + 1);
}

// push a sample onto a fixed ring, keeping the newest `cap` values left-packed.
template <std::size_t N>
void push_hist(std::array<float, N>& ring, int& len, float v) {
    if (len < static_cast<int>(N)) { ring[static_cast<std::size_t>(len++)] = v; return; }
    for (std::size_t i = 1; i < N; ++i) ring[i - 1] = ring[i];
    ring[N - 1] = v;
}

std::string user_of(uid_t uid) {
    if (passwd* pw = ::getpwuid(uid)) return pw->pw_name;
    return std::to_string(uid);
}

}  // namespace

Sampler::Sampler() {
    clk_tck_ = ::sysconf(_SC_CLK_TCK);
    if (clk_tck_ <= 0) clk_tck_ = 100;
    page_size_ = ::sysconf(_SC_PAGESIZE);
    if (page_size_ <= 0) page_size_ = 4096;
    read_static();
}

void Sampler::read_static() {
    // hostname
    char host[256] = {};
    if (::gethostname(host, sizeof host - 1) == 0) hostname_ = host;

    // kernel
    utsname u{};
    if (::uname(&u) == 0) kernel_ = std::string(u.sysname) + " " + u.release;

    // cpu model + logical core count
    std::ifstream ci("/proc/cpuinfo");
    std::string line;
    int cores = 0;
    while (std::getline(ci, line)) {
        if (line.rfind("processor", 0) == 0) ++cores;
        else if (cpu_model_.empty() && line.rfind("model name", 0) == 0) {
            auto c = line.find(':');
            if (c != std::string::npos) cpu_model_ = trim(line.substr(c + 1));
        }
    }
    ncpu_ = std::max(1, cores);
    if (cpu_model_.empty()) cpu_model_ = "CPU";

    // total RAM
    std::ifstream mi("/proc/meminfo");
    while (std::getline(mi, line)) {
        if (line.rfind("MemTotal:", 0) == 0) {
            std::uint64_t kb = 0;
            std::sscanf(line.c_str(), "MemTotal: %lu kB", &kb);
            ram_total_ = Bytes{kb * 1024};
            break;
        }
    }
}

void Sampler::sample_cpu(CpuInfo& cpu) {
    cpu.model = cpu_model_;
    cpu.logical = ncpu_;

    std::ifstream st("/proc/stat");
    std::string line;
    std::vector<CpuTimes> cores;
    CpuTimes agg{};
    while (std::getline(st, line)) {
        if (line.rfind("cpu", 0) != 0) break;
        std::istringstream ss(line);
        std::string tag;
        ss >> tag;
        std::array<std::uint64_t, 10> v{};
        int n = 0;
        while (n < 10 && (ss >> v[static_cast<std::size_t>(n)])) ++n;
        std::uint64_t idle = v[3] + v[4];  // idle + iowait
        std::uint64_t total = 0;
        for (int i = 0; i < n; ++i) total += v[static_cast<std::size_t>(i)];
        CpuTimes t{idle, total};
        if (tag == "cpu") agg = t;
        else cores.push_back(t);
    }

    auto busy = [](CpuTimes now, CpuTimes prev) -> Ratio {
        std::uint64_t dt = now.total - prev.total;
        std::uint64_t di = now.idle - prev.idle;
        if (dt == 0) return Ratio{0};
        return Ratio{1.0 - static_cast<double>(di) / static_cast<double>(dt)};
    };

    if (!first_) cpu.total = busy(agg, prev_total_);
    prev_total_ = agg;

    // frequency (average of cores from cpuinfo_cur_freq, kHz) + temperature
    cpu.cores.resize(cores.size());
    if (prev_cores_.size() != cores.size()) prev_cores_.assign(cores.size(), CpuTimes{});

    for (std::size_t i = 0; i < cores.size(); ++i) {
        CpuCore& c = core_hist_[static_cast<int>(i)];
        if (!first_) c.usage = busy(cores[i], prev_cores_[i]);
        prev_cores_[i] = cores[i];

        // per-core frequency
        std::string fp = "/sys/devices/system/cpu/cpu" + std::to_string(i) +
                         "/cpufreq/scaling_cur_freq";
        std::string fs = first_line(slurp(fp.c_str()));
        if (!fs.empty()) c.freq = Hertz{std::strtoull(fs.c_str(), nullptr, 10) * 1000};

        push_hist(c.history, c.hist_len, static_cast<float>(c.usage.v));
        cpu.cores[i] = c;
    }

    push_hist(cpu.total_history, cpu.total_hist_len, static_cast<float>(cpu.total.v));
    // keep the sampler-owned copy in sync so history persists across ticks
    total_hist_ = cpu.total_history;
    total_hist_len_ = cpu.total_hist_len;
    cpu.total_history = total_hist_;
    cpu.total_hist_len = total_hist_len_;

    // loadavg
    std::ifstream la("/proc/loadavg");
    la >> cpu.loadavg[0] >> cpu.loadavg[1] >> cpu.loadavg[2];

    // temperature — scan thermal zones for a cpu/x86_pkg type
    for (int z = 0; z < 16; ++z) {
        std::string base = "/sys/class/thermal/thermal_zone" + std::to_string(z);
        std::string type = trim(first_line(slurp((base + "/type").c_str())));
        if (type.empty()) break;
        if (type.find("pkg") != std::string::npos || type.find("cpu") != std::string::npos ||
            type == "acpitz" || type.find("coretemp") != std::string::npos) {
            std::string t = first_line(slurp((base + "/temp").c_str()));
            if (!t.empty()) { cpu.temp_c = std::strtof(t.c_str(), nullptr) / 1000.0f; break; }
        }
    }
}

void Sampler::sample_mem(MemInfo& m) {
    std::ifstream mi("/proc/meminfo");
    std::string key;
    std::uint64_t val;
    std::string unit;
    std::unordered_map<std::string, std::uint64_t> kv;
    std::string line;
    while (std::getline(mi, line)) {
        std::istringstream ss(line);
        ss >> key >> val >> unit;
        if (!key.empty() && key.back() == ':') key.pop_back();
        kv[key] = val * 1024;  // kB → bytes
    }
    m.total     = Bytes{kv["MemTotal"]};
    m.available = Bytes{kv.count("MemAvailable") ? kv["MemAvailable"] : kv["MemFree"]};
    m.cached    = Bytes{kv["Cached"]};
    m.buffers   = Bytes{kv["Buffers"]};
    m.used      = Bytes{m.total.value > m.available.value ? m.total.value - m.available.value : 0};
    m.swap_total = Bytes{kv["SwapTotal"]};
    m.swap_used  = Bytes{kv["SwapTotal"] > kv["SwapFree"] ? kv["SwapTotal"] - kv["SwapFree"] : 0};
}

void Sampler::sample_disks(std::vector<DiskInfo>& disks) {
    std::ifstream mounts("/proc/mounts");
    std::string dev, mount, fstype, rest;
    while (mounts >> dev >> mount >> fstype) {
        std::getline(mounts, rest);
        // Only real block-backed filesystems the user cares about.
        static const char* skip[] = {"proc", "sysfs", "tmpfs", "devtmpfs", "devpts",
                                      "cgroup", "cgroup2", "overlay", "squashfs", "autofs",
                                      "mqueue", "hugetlbfs", "debugfs", "tracefs", "securityfs",
                                      "pstore", "bpf", "configfs", "fusectl", "ramfs", "efivarfs"};
        bool ignore = dev.rfind("/dev/", 0) != 0;
        for (auto* s : skip) if (fstype == s) ignore = true;
        if (ignore) continue;

        struct statvfs vfs{};
        if (::statvfs(mount.c_str(), &vfs) != 0) continue;
        std::uint64_t bs = vfs.f_frsize ? vfs.f_frsize : vfs.f_bsize;
        std::uint64_t total = vfs.f_blocks * bs;
        std::uint64_t avail = vfs.f_bavail * bs;
        if (total == 0) continue;

        DiskInfo d;
        d.device = dev; d.mount = mount; d.fstype = fstype;
        d.total = Bytes{total};
        d.used  = Bytes{total - vfs.f_bfree * bs};
        disks.push_back(std::move(d));
    }
    // Collapse btrfs/bind subvolumes: many mounts share one device+capacity.
    // Keep the shortest mount path per (device,total) — that's the "real" root.
    std::sort(disks.begin(), disks.end(), [](const DiskInfo& a, const DiskInfo& b) {
        if (a.device != b.device) return a.device < b.device;
        if (a.total.value != b.total.value) return a.total.value < b.total.value;
        return a.mount.size() < b.mount.size();
    });
    disks.erase(std::unique(disks.begin(), disks.end(),
        [](const DiskInfo& a, const DiskInfo& b) {
            return a.device == b.device && a.total.value == b.total.value;
        }), disks.end());
    std::sort(disks.begin(), disks.end(),
              [](const DiskInfo& a, const DiskInfo& b) { return a.used.value > b.used.value; });
    if (disks.size() > 5) disks.resize(5);
}

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
        push_hist(iface.rx_history, iface.hist_len, static_cast<float>(iface.rx.per_sec));
        // tx_history rides the same length counter — shift it in lockstep with rx.
        for (int i = 1; i < iface.hist_len; ++i)
            iface.tx_history[static_cast<std::size_t>(i - 1)] = iface.tx_history[static_cast<std::size_t>(i)];
        iface.tx_history[static_cast<std::size_t>(std::min(iface.hist_len - 1, 47))] =
            static_cast<float>(iface.tx.per_sec);

        prev_net_[name] = {rx, tx};
    }

    // Surface only interfaces that have ever moved bytes, busiest first.
    for (auto& [k, v] : net_hist_)
        if (v.rx_total.value || v.tx_total.value)
            nets.push_back(v);
    std::sort(nets.begin(), nets.end(), [](const NetIface& a, const NetIface& b) {
        return (a.rx.per_sec + a.tx.per_sec) > (b.rx.per_sec + b.tx.per_sec);
    });
    if (nets.size() > 4) nets.resize(4);
}

void Sampler::sample_procs(Snapshot& snap, SortKey sort, int top_n, double dt) {
    DIR* proc = ::opendir("/proc");
    if (!proc) return;

    std::vector<ProcInfo> out;
    std::unordered_map<int, ProcPrev> cur;
    int total_procs = 0, total_threads = 0, running = 0;
    double dt_ticks = dt * static_cast<double>(clk_tck_) * static_cast<double>(ncpu_);

    dirent* e;
    while ((e = ::readdir(proc)) != nullptr) {
        if (e->d_name[0] < '0' || e->d_name[0] > '9') continue;
        int pid = std::atoi(e->d_name);
        std::string base = "/proc/" + std::string(e->d_name);

        std::string stat = slurp((base + "/stat").c_str());
        if (stat.empty()) continue;

        // comm is in parens and may contain spaces/parens → parse around them.
        auto lp = stat.find('('), rp = stat.rfind(')');
        if (lp == std::string::npos || rp == std::string::npos) continue;
        std::string comm = stat.substr(lp + 1, rp - lp - 1);
        std::istringstream ss(stat.substr(rp + 2));
        char state = '?';
        ss >> state;
        // skip fields 4..13 (ppid..cstime start): we want utime(14) stime(15)
        std::string skip;
        for (int i = 0; i < 10; ++i) ss >> skip;
        std::uint64_t utime = 0, stime = 0;
        ss >> utime >> stime;
        // fields 16..19, then num_threads(20)
        for (int i = 0; i < 4; ++i) ss >> skip;
        int threads = 0; ss >> threads;

        ++total_procs;
        total_threads += std::max(1, threads);
        if (state == 'R') ++running;

        std::uint64_t cpu_ticks = utime + stime;
        cur[pid] = {cpu_ticks};
        double cpu_pct = 0;
        if (!first_ && prev_proc_.count(pid) && dt_ticks > 0) {
            std::uint64_t d = cpu_ticks > prev_proc_[pid].cpu_ticks
                                  ? cpu_ticks - prev_proc_[pid].cpu_ticks : 0;
            cpu_pct = 100.0 * static_cast<double>(d) / dt_ticks * static_cast<double>(ncpu_);
        }

        // RSS from statm (pages)
        std::uint64_t rss_pages = 0;
        {
            std::ifstream sm(base + "/statm");
            std::uint64_t total_pages = 0;
            sm >> total_pages >> rss_pages;
        }
        Bytes rss{rss_pages * static_cast<std::uint64_t>(page_size_)};

        ProcInfo p;
        p.pid = pid;
        p.name = comm;
        p.state = state;
        p.threads = std::max(1, threads);
        p.cpu = cpu_pct;
        p.rss = rss;
        p.mem_share = Ratio::of(rss, ram_total_);

        // owner
        struct stat_dummy {};
        struct ::stat stbuf{};
        if (::stat(base.c_str(), &stbuf) == 0) p.user = user_of(stbuf.st_uid);

        out.push_back(std::move(p));
    }
    ::closedir(proc);

    prev_proc_ = std::move(cur);
    snap.proc_count = total_procs;
    snap.thread_count = total_threads;
    snap.running = running;

    using Cmp = bool (*)(const ProcInfo&, const ProcInfo&);
    auto by = [](SortKey k) -> Cmp {
        switch (k) {
            case SortKey::Cpu:  return [](const ProcInfo& a, const ProcInfo& b){ return a.cpu > b.cpu; };
            case SortKey::Mem:  return [](const ProcInfo& a, const ProcInfo& b){ return a.rss.value > b.rss.value; };
            case SortKey::Pid:  return [](const ProcInfo& a, const ProcInfo& b){ return a.pid < b.pid; };
            case SortKey::Name: return [](const ProcInfo& a, const ProcInfo& b){ return a.name < b.name; };
        }
        return [](const ProcInfo& a, const ProcInfo& b){ return a.cpu > b.cpu; };
    };
    std::sort(out.begin(), out.end(), by(sort));
    if (static_cast<int>(out.size()) > top_n) out.resize(static_cast<std::size_t>(top_n));
    snap.procs = std::move(out);
}

Verdict Sampler::judge(const Snapshot& s) const {
    // Translate raw numbers into a plain-language answer to "what's going on?".
    Verdict v;
    double cpu = s.cpu.total.percent();
    double mem = s.mem.usage().percent();
    double swap = s.mem.swap_usage().percent();
    double la1 = s.cpu.loadavg[0];
    double la_ratio = ncpu_ ? la1 / ncpu_ : la1;

    // Find the loudest CPU and memory consumers among sampled procs.
    const ProcInfo* top_cpu = nullptr;
    const ProcInfo* top_mem = nullptr;
    for (const auto& p : s.procs) {
        if (!top_cpu || p.cpu > top_cpu->cpu) top_cpu = &p;
        if (!top_mem || p.rss.value > top_mem->rss.value) top_mem = &p;
    }

    auto culprit_cpu = [&]() -> std::string {
        if (top_cpu && top_cpu->cpu > 20)
            return top_cpu->name + " (pid " + std::to_string(top_cpu->pid) + ") is using " +
                   std::to_string(static_cast<int>(top_cpu->cpu)) + "% CPU";
        return {};
    };
    auto culprit_mem = [&]() -> std::string {
        if (top_mem && top_mem->mem_share.percent() > 10)
            return top_mem->name + " holds " + humanize_bytes(top_mem->rss) + " of RAM";
        return {};
    };

    if (cpu > 90 || mem > 92 || swap > 50 || la_ratio > 2.5) {
        v.level = Health::Critical;
        if (mem > 92 || swap > 50) { v.headline = "Memory is critically tight"; v.detail = culprit_mem(); }
        else                       { v.headline = "The CPU is maxed out";       v.detail = culprit_cpu(); }
    } else if (cpu > 70 || mem > 80 || la_ratio > 1.5) {
        v.level = Health::Stressed;
        if (cpu > 70)  { v.headline = "Working hard — CPU is heavily loaded"; v.detail = culprit_cpu(); }
        else           { v.headline = "Memory is filling up";                 v.detail = culprit_mem(); }
    } else if (cpu > 35 || mem > 60 || (top_cpu && top_cpu->cpu > 40)) {
        v.level = Health::Busy;
        v.headline = "Busy but comfortable";
        v.detail = culprit_cpu();
        if (v.detail.empty()) v.detail = culprit_mem();
    } else {
        v.level = Health::Calm;
        v.headline = "All calm — nothing is straining this machine";
        v.detail = "CPU " + std::to_string(static_cast<int>(cpu)) + "%, RAM " +
                   std::to_string(static_cast<int>(mem)) + "%";
    }
    if (v.detail.empty())
        v.detail = "CPU " + std::to_string(static_cast<int>(cpu)) + "%  ·  RAM " +
                   std::to_string(static_cast<int>(mem)) + "%  ·  load " +
                   [&]{ char b[16]; std::snprintf(b, sizeof b, "%.2f", la1); return std::string(b); }();
    return v;
}

Snapshot Sampler::sample(SortKey sort, int top_n) {
    auto now = std::chrono::steady_clock::now();
    double dt = first_ ? 0.0
                       : std::chrono::duration<double>(now - last_time_).count();
    if (dt <= 0 && !first_) dt = 0.001;
    last_time_ = now;

    Snapshot s;
    s.hostname = hostname_;
    s.kernel = kernel_;

    std::string up = first_line(slurp("/proc/uptime"));
    s.uptime_sec = static_cast<std::uint64_t>(std::strtod(up.c_str(), nullptr));

    sample_cpu(s.cpu);
    sample_mem(s.mem);
    sample_disks(s.disks);
    sample_net(s.nets, dt);
    sample_procs(s, sort, top_n, dt);
    s.verdict = judge(s);

    first_ = false;
    return s;
}

}  // namespace bottom
