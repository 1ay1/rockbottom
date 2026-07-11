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
        ss >> key >> val >> unit;
        if (!key.empty() && key.back() == ':') key.pop_back();
        kv[key] = val * 1024;  // kB → bytes
    }
    m.total      = Bytes{kv["MemTotal"]};
    m.available  = Bytes{kv.count("MemAvailable") ? kv["MemAvailable"] : kv["MemFree"]};
    m.cached     = Bytes{kv["Cached"]};
    m.buffers    = Bytes{kv["Buffers"]};
    m.used       = Bytes{m.total.value > m.available.value ? m.total.value - m.available.value : 0};
    m.swap_total = Bytes{kv["SwapTotal"]};
    m.swap_used  = Bytes{kv["SwapTotal"] > kv["SwapFree"] ? kv["SwapTotal"] - kv["SwapFree"] : 0};

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
    while (vs >> key >> val) {
        if (key == "pswpin") in = val;
        else if (key == "pswpout") out = val;
        else if (key == "pgpgin") pgin = val;      // file-backed page-ins (kB units)
        else if (key == "pgpgout") pgout = val;
        else if (key == "pgfault") faults = val;   // all minor+major faults
    }
    const std::uint64_t page = 4096;   // vmstat counts are in pages
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
    prev_pswpin_ = in;
    prev_pswpout_ = out;
    prev_pgin_ = pgin;
    prev_pgout_ = pgout;
    prev_faults_ = faults;
}

}  // namespace rockbottom
