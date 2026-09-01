// platform/darwin/cpu.cpp — mach host_processor_info + sysctl + getloadavg.
//
// macOS has no /proc/stat. Per-core busy fractions come from
// host_processor_info(PROCESSOR_CPU_LOAD_INFO), which returns cumulative tick
// counters (user/system/idle/nice) per logical CPU — we delta them exactly the
// way the Linux backend deltas /proc/stat. The aggregate is the tick-weighted
// sum. loadavg comes from getloadavg(3). macOS has no iowait analogue, so that
// stays at its sentinel and the panes omit it — the same graceful-degradation
// contract every backend follows.
//
// Per-core CLOCK is recovered in freq.hpp from the IORegistry DVFS tables plus
// IOReport residency counters; see that header for why it isn't simply a
// sysctl.

#include "../../sampler.hpp"
#include "freq.hpp"
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

    probe_topology();
}

// Apple Silicon splits its cores into performance / efficiency clusters, which
// the kernel describes via the hw.perflevelN.* sysctl family: perflevel0 is
// always the FASTEST tier and each level reports its logical-cpu count plus a
// human name ("Performance" / "Efficiency").
//
// The mapping from perf level to mach cpu INDEX is the subtle part. XNU
// enumerates host_processor_info() with the efficiency cluster FIRST — on an
// M1, cpu0-3 are the E cores and cpu4-7 the P cores. That is the reverse of
// the sysctl order, and getting it backwards mislabels every core on the
// machine. Verified empirically: a single spinning thread (which the scheduler
// places on a P core) lights up cpu4-7, never cpu0-3.
//
// Intel Macs have no perflevel sysctls at all — nperflevels is absent or 1,
// the vectors stay Unknown, and the UI shows the flat homogeneous view.
void Sampler::probe_topology() {
    const std::size_t n = static_cast<std::size_t>(std::max(1, ncpu_));
    core_kind_.assign(n, CoreKind::Unknown);
    // macOS exposes no logical->physical core map (and Apple Silicon has no
    // SMT, so there are no siblings to resolve); leave both empty.
    core_phys_.clear();
    phys_siblings_.clear();
    core_id_siblings_.clear();

    const int levels = static_cast<int>(sysctl_num<std::int32_t>("hw.nperflevels").value_or(0));
    if (levels < 2) { core_kind_.clear(); return; }

    const int nperf = static_cast<int>(sysctl_num<std::int32_t>("hw.perflevel0.logicalcpu").value_or(0));
    const int neff  = static_cast<int>(sysctl_num<std::int32_t>("hw.perflevel1.logicalcpu").value_or(0));
    if (nperf <= 0 || neff <= 0) { core_kind_.clear(); return; }

    // Efficiency cluster occupies the LOW mach indices; performance the high.
    // Clamp rather than assume the two counts sum to ncpu_ (a 3-tier part
    // would report more levels than we model — the remainder stays Unknown).
    for (std::size_t i = 0; i < n; ++i) {
        if (i < static_cast<std::size_t>(neff))            core_kind_[i] = CoreKind::Eff;
        else if (i < static_cast<std::size_t>(neff + nperf)) core_kind_[i] = CoreKind::Perf;
    }

    perf_label_ = sys::trim(sysctl_str("hw.perflevel0.name"));
    eff_label_  = sys::trim(sysctl_str("hw.perflevel1.name"));
    if (perf_label_.empty()) perf_label_ = "Performance";
    if (eff_label_.empty())  eff_label_  = "Efficiency";
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

void Sampler::sample_cpu(CpuInfo& cpu, bool fast) {
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
        std::uint64_t user  = t[CPU_STATE_USER] + t[CPU_STATE_NICE];
        std::uint64_t sys   = t[CPU_STATE_SYSTEM];
        std::uint64_t total = user + sys + idle;
        cores[i] = {idle, total, user, sys};
        agg.idle += idle;
        agg.total += total;
        agg.user += user;
        agg.system += sys;
    }
    ::vm_deallocate(::mach_task_self(), reinterpret_cast<vm_address_t>(info),
                    info_cnt * sizeof(int));

    auto busy = [](CpuTimes now, CpuTimes prev) -> Ratio {
        std::uint64_t dt = now.total - prev.total;
        std::uint64_t di = now.idle - prev.idle;
        if (dt == 0) return Ratio{0};
        return Ratio{1.0 - static_cast<double>(di) / static_cast<double>(dt)};
    };

    if (!first_) {
        cpu.total = busy(agg, prev_total_);
        const std::uint64_t dt = agg.total - prev_total_.total;
        if (dt > 0) {
            cpu.user   = Ratio{static_cast<double>(agg.user - prev_total_.user) / static_cast<double>(dt)};
            cpu.system = Ratio{static_cast<double>(agg.system - prev_total_.system) / static_cast<double>(dt)};
        }
    }
    prev_total_ = agg;
    // iowait has no macOS analogue; leave cpu.iowait at 0 (panes omit it).

    cpu.cores.resize(cores.size());
    if (prev_cores_.size() != cores.size()) prev_cores_.assign(cores.size(), CpuTimes{});

    // Live per-core clock. One sampler for the process lifetime: it holds an
    // IOReport subscription and the previous residency counters, and reports
    // the time-weighted mean clock over the interval between ticks. Empty
    // until the second tick (a delta needs two samples) and empty forever on a
    // machine where IOReport isn't available — in both cases freq stays 0 and
    // the UI omits the column, exactly as it did before.
    //
    // SKIPPED on the fast startup prime. Constructing the sampler dlopen's
    // libIOReport and builds an IOReport subscription over every CPU channel —
    // ~110ms one-time — and per-core clock is a detail-pane column that is
    // empty on the first tick anyway (it needs a delta). Deferring it to the
    // first real background tick keeps that cost off the path to first paint.
    std::vector<std::uint64_t> freqs;
    if (!fast) {
        static macfreq::Sampler freq_sampler;
        freqs = freq_sampler.sample();
    }

    for (std::size_t i = 0; i < cores.size(); ++i) {
        CpuCore& c = core_hist_[static_cast<int>(i)];
        if (!first_) c.usage = busy(cores[i], prev_cores_[i]);
        prev_cores_[i] = cores[i];
        if (i < freqs.size() && freqs[i] > 0) c.freq = Hertz{freqs[i]};
        sys::push_hist(c.history, c.hist_len, static_cast<float>(c.usage.v));
        cpu.cores[i] = c;
    }

    // Label each core with its cluster from the topology probed at startup.
    apply_topology(cpu);

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
