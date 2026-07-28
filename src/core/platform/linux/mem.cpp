// collectors/mem.cpp — /proc/meminfo.

#include "../../sampler.hpp"
#include "procfs.hpp"

#include <cstdint>
#include <fstream>
#include <sstream>
#include <string>
#include <unordered_map>

namespace rockbottom {

void Sampler::sample_mem(MemInfo& m) {
    std::ifstream mi("/proc/meminfo");
    std::unordered_map<std::string, std::uint64_t> kv;
    std::string line, key, unit;
    std::uint64_t val;
    while (std::getline(mi, line)) {
        std::istringstream ss(line);
        // A line that fails to parse must not recycle the PREVIOUS line's
        // val into this key — only ingest rows whose number actually read.
        if (!(ss >> key >> val)) continue;
        ss >> unit;
        if (!key.empty() && key.back() == ':') key.pop_back();
        kv[key] = val * 1024;  // kB → bytes
    }
    m.total      = Bytes{kv["MemTotal"]};
    m.available  = Bytes{kv.count("MemAvailable") ? kv["MemAvailable"] : kv["MemFree"]};
    m.cached     = Bytes{kv["Cached"]};
    m.buffers    = Bytes{kv["Buffers"]};
    m.used       = Bytes{m.total.value > m.available.value ? m.total.value - m.available.value : 0};
    m.swap_total = Bytes{kv["SwapTotal"]};
    // Only derive used-swap when BOTH fields are present. If SwapFree is missing
    // (some sandboxed/again-restricted /proc views expose SwapTotal but not
    // SwapFree), kv[] would default it to 0 and report the swap as 100% full —
    // a phantom that can trip the verdict's thrashing warning. Absent → 0 used.
    m.swap_used  = Bytes{(kv.count("SwapTotal") && kv.count("SwapFree")
                          && kv["SwapTotal"] > kv["SwapFree"])
                             ? kv["SwapTotal"] - kv["SwapFree"] : 0};

    // Hugepages. meminfo reports counts (already *1024'd above harmlessly —
    // they're dimensionless counts, so undo the kB scaling), plus Hugepagesize
    // which IS a size. The leak signature the issue asks for: pages that are
    // Free but not Rsvd sit reserved in the pool doing nothing, memory the
    // kernel cannot reclaim for normal use.
    auto count = [&](const char* k) -> std::uint64_t {
        auto it = kv.find(k);
        return it == kv.end() ? 0 : it->second / 1024;  // undo kB scaling
    };
    m.huge_total = count("HugePages_Total");
    m.huge_free  = count("HugePages_Free");
    m.huge_rsvd  = count("HugePages_Rsvd");
    m.huge_size  = Bytes{kv.count("Hugepagesize") ? kv["Hugepagesize"] : 0};
    std::uint64_t idle_pages = m.huge_free > m.huge_rsvd ? m.huge_free - m.huge_rsvd : 0;
    m.huge_idle  = Bytes{idle_pages * m.huge_size.value};

    procfs::push_hist(mem_hist_, mem_hist_len_, static_cast<float>(m.usage().v));
    m.usage_history = mem_hist_;
    m.hist_len = mem_hist_len_;
}

void Sampler::sample_mem_rates(MemInfo& m, double dt) {
    // /proc/vmstat pswpin/pswpout are cumulative PAGE counts. Their delta is
    // live paging traffic — the true "thrashing" signal. A machine can sit at
    // 60% swap harmlessly for weeks; it cannot page 50MB/s harmlessly.
    std::ifstream vs("/proc/vmstat");
    std::string key;
    std::uint64_t val, in = 0, out = 0, pgin = 0, pgout = 0, faults = 0;
    std::uint64_t numa_hint = 0;
    while (vs >> key >> val) {
        if (key == "pswpin") in = val;
        else if (key == "pswpout") out = val;
        else if (key == "pgpgin") pgin = val;      // file-backed page-ins (kB units)
        else if (key == "pgpgout") pgout = val;
        else if (key == "pgfault") faults = val;   // all minor+major faults
        else if (key == "numa_hint_faults") numa_hint = val;  // auto-NUMA balancer churn
    }
    // vmstat counts pswpin/pswpout in PAGES — use the real kernel page size
    // (16K on new Android, 64K on some ARM64 server kernels), not a hardcoded
    // 4096: the swap-traffic rate feeds the verdict's thrashing thresholds in
    // absolute bytes/sec, so a 16K kernel would under-report paging 4x.
    const std::uint64_t page = static_cast<std::uint64_t>(page_size_);
    std::uint64_t di = in > prev_pswpin_ ? in - prev_pswpin_ : 0;
    std::uint64_t dout = out > prev_pswpout_ ? out - prev_pswpout_ : 0;
    m.swap_in  = first_ ? ByteRate{0} : rate(Bytes{di * page}, dt);
    m.swap_out = first_ ? ByteRate{0} : rate(Bytes{dout * page}, dt);
    // pgpgin/pgpgout are in KILOBYTES (not pages) per the kernel's vmstat.
    std::uint64_t dpi = pgin > prev_pgin_ ? pgin - prev_pgin_ : 0;
    std::uint64_t dpo = pgout > prev_pgout_ ? pgout - prev_pgout_ : 0;
    m.page_in  = first_ ? ByteRate{0} : rate(Bytes{dpi * 1024}, dt);
    m.page_out = first_ ? ByteRate{0} : rate(Bytes{dpo * 1024}, dt);
    std::uint64_t df = faults > prev_faults_ ? faults - prev_faults_ : 0;
    m.faults_ps = first_ || dt <= 0 ? 0.0 : static_cast<double>(df) / dt;
    // NUMA auto-balancing hint-fault rate. Only meaningful when the balancer is
    // on (single-socket boxes report 0). numa_balancing is read live — cheap,
    // one tiny sysctl file — so a runtime toggle is reflected immediately.
    std::uint64_t dnh = numa_hint > prev_numa_hint_faults_ ? numa_hint - prev_numa_hint_faults_ : 0;
    m.numa_hint_faults_ps = first_ || dt <= 0 ? 0.0 : static_cast<double>(dnh) / dt;
    {
        std::string nb = procfs::trim(procfs::first_line(
            procfs::slurp("/proc/sys/kernel/numa_balancing")));
        m.numa_on = !nb.empty() && nb != "0";
    }
    prev_pswpin_ = in;
    prev_pswpout_ = out;
    prev_pgin_ = pgin;
    prev_pgout_ = pgout;
    prev_faults_ = faults;
    prev_numa_hint_faults_ = numa_hint;
}

}  // namespace rockbottom
