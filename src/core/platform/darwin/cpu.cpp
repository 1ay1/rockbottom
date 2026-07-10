// platform/darwin/cpu.cpp — mach host_processor_info + sysctl + getloadavg.
//
// macOS has no /proc/stat. Per-core busy fractions come from
// host_processor_info(PROCESSOR_CPU_LOAD_INFO), which returns cumulative tick
// counters (user/system/idle/nice) per logical CPU — we delta them exactly the
// way the Linux backend deltas /proc/stat. The aggregate is the tick-weighted
// sum. loadavg comes from getloadavg(3). macOS doesn't expose a stable
// per-core clock or an iowait split without private SMC/IOReport access, so
// those stay at their sentinels and the panes omit them — the same
// graceful-degradation contract every backend follows.

#include "../../sampler.hpp"
#include "mach_util.hpp"

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <ctime>
#include <string>
#include <vector>

#include <mach/mach.h>
#include <mach/processor_info.h>
#include <sys/utsname.h>
#include <unistd.h>

namespace rockbottom {

using namespace mac;

void Sampler::read_static() {
    char host[256] = {};
    if (::gethostname(host, sizeof host - 1) == 0) hostname_ = host;

    utsname u{};
    if (::uname(&u) == 0) kernel_ = std::string(u.sysname) + " " + u.release;

    cpu_model_ = sys::trim(sysctl_str("machdep.cpu.brand_string"));
    if (cpu_model_.empty()) cpu_model_ = sys::trim(sysctl_str("hw.model"));
    if (cpu_model_.empty()) cpu_model_ = "CPU";

    ncpu_ = static_cast<int>(sysctl_num<std::int32_t>("hw.logicalcpu").value_or(1));
    if (ncpu_ < 1) ncpu_ = 1;

    ram_total_ = Bytes{sysctl_num<std::uint64_t>("hw.memsize").value_or(0)};
}

// Seconds since boot: now - kern.boottime (a struct timeval set at boot).
std::uint64_t Sampler::uptime_sec() const {
    timeval bt{};
    std::size_t len = sizeof bt;
    int mib[2] = {CTL_KERN, KERN_BOOTTIME};
    if (::sysctl(mib, 2, &bt, &len, nullptr, 0) != 0 || bt.tv_sec == 0) return 0;
    time_t now = ::time(nullptr);
    return now > bt.tv_sec ? static_cast<std::uint64_t>(now - bt.tv_sec) : 0;
}

void Sampler::sample_cpu(CpuInfo& cpu) {
    cpu.model = cpu_model_;
    cpu.logical = ncpu_;

    natural_t                ncpus = 0;
    processor_info_array_t   info = nullptr;
    mach_msg_type_number_t   info_cnt = 0;
    kern_return_t kr = ::host_processor_info(host_port(), PROCESSOR_CPU_LOAD_INFO,
                                             &ncpus, &info, &info_cnt);
    if (kr != KERN_SUCCESS || !info) return;

    auto* loads = reinterpret_cast<processor_cpu_load_info_t>(info);
    std::vector<CpuTimes> cores(ncpus);
    CpuTimes agg{};
    for (natural_t i = 0; i < ncpus; ++i) {
        const auto& t = loads[i].cpu_ticks;
        std::uint64_t idle  = t[CPU_STATE_IDLE];
        std::uint64_t total = t[CPU_STATE_USER] + t[CPU_STATE_SYSTEM] +
                              t[CPU_STATE_IDLE] + t[CPU_STATE_NICE];
        cores[i] = {idle, total};
        agg.idle += idle;
        agg.total += total;
    }
    ::vm_deallocate(::mach_task_self(), reinterpret_cast<vm_address_t>(info),
                    info_cnt * sizeof(int));

    auto busy = [](CpuTimes now, CpuTimes prev) -> Ratio {
        std::uint64_t dt = now.total - prev.total;
        std::uint64_t di = now.idle - prev.idle;
        if (dt == 0) return Ratio{0};
        return Ratio{1.0 - static_cast<double>(di) / static_cast<double>(dt)};
    };

    if (!first_) cpu.total = busy(agg, prev_total_);
    prev_total_ = agg;
    // iowait has no macOS analogue; leave cpu.iowait at 0 (panes omit it).

    cpu.cores.resize(cores.size());
    if (prev_cores_.size() != cores.size()) prev_cores_.assign(cores.size(), CpuTimes{});
    for (std::size_t i = 0; i < cores.size(); ++i) {
        CpuCore& c = core_hist_[static_cast<int>(i)];
        if (!first_) c.usage = busy(cores[i], prev_cores_[i]);
        prev_cores_[i] = cores[i];
        // No stable per-core cur-freq surface on Apple Silicon / Intel Macs
        // without private frameworks; leave c.freq at 0.
        sys::push_hist(c.history, c.hist_len, static_cast<float>(c.usage.v));
        cpu.cores[i] = c;
    }

    sys::push_hist(total_hist_, total_hist_len_, static_cast<float>(cpu.total.v));
    cpu.total_history = total_hist_;
    cpu.total_hist_len = total_hist_len_;

    double la[3] = {0, 0, 0};
    if (::getloadavg(la, 3) == 3) {
        cpu.loadavg[0] = la[0];
        cpu.loadavg[1] = la[1];
        cpu.loadavg[2] = la[2];
    }
    // Package temperature needs SMC/IOReport (private); left at 0.
}

}  // namespace rockbottom
