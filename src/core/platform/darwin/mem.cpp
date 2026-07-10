// platform/darwin/mem.cpp — host_statistics64(HOST_VM_INFO64) + vm.swapusage.
//
// The Darwin memory model maps onto rockbottom's MemInfo like so:
//   total     = hw.memsize
//   wired+active+compressed  → "used" (pages the kernel can't cheaply reclaim)
//   inactive + free          → "available" (reclaimable, matching MemAvailable)
//   cached    = external/file-backed pages (Linux's page cache analogue)
//   swap      = vm.swapusage (xsw_usage struct: total / used, in bytes)
// Swap-traffic rates come from the cumulative swapins/swapouts page counters in
// the same vm_statistics64 snapshot — the exact analogue of vmstat pswpin/out.

#include "../../sampler.hpp"
#include "mach_util.hpp"

#include <cstdint>

#include <mach/mach.h>
#include <sys/sysctl.h>

namespace rockbottom {

using namespace mac;

void Sampler::sample_mem(MemInfo& m) {
    vm_statistics64_data_t vm{};
    mach_msg_type_number_t cnt = HOST_VM_INFO64_COUNT;
    if (::host_statistics64(host_port(), HOST_VM_INFO64,
                            reinterpret_cast<host_info64_t>(&vm), &cnt) != KERN_SUCCESS)
        return;

    const std::uint64_t page = sysctl_num<std::uint64_t>("hw.pagesize")
                                   .value_or(static_cast<std::uint64_t>(page_size_));
    auto B = [page](std::uint64_t pages) { return Bytes{pages * page}; };

    m.total     = ram_total_;
    m.cached    = B(vm.external_page_count);
    m.buffers   = B(vm.purgeable_count);
    m.wired     = B(vm.wire_count);
    m.compressed = B(vm.compressor_page_count);
    m.app       = B(vm.internal_page_count > vm.purgeable_count
                        ? vm.internal_page_count - vm.purgeable_count
                        : vm.internal_page_count);
    // Reclaimable memory ≈ free + inactive + speculative + file-backed cache.
    const std::uint64_t avail_pages = static_cast<std::uint64_t>(vm.free_count) +
                                      vm.inactive_count + vm.speculative_count +
                                      vm.external_page_count;
    m.available = B(avail_pages);
    m.used      = Bytes{m.total.value > m.available.value ? m.total.value - m.available.value : 0};

    xsw_usage sw{};
    std::size_t swlen = sizeof sw;
    if (::sysctlbyname("vm.swapusage", &sw, &swlen, nullptr, 0) == 0) {
        m.swap_total = Bytes{sw.xsu_total};
        m.swap_used  = Bytes{sw.xsu_used};
    }

    sys::push_hist(mem_hist_, mem_hist_len_, static_cast<float>(m.usage().v));
    m.usage_history = mem_hist_;
    m.hist_len = mem_hist_len_;
}

void Sampler::sample_mem_rates(MemInfo& m, double dt) {
    vm_statistics64_data_t vm{};
    mach_msg_type_number_t cnt = HOST_VM_INFO64_COUNT;
    if (::host_statistics64(host_port(), HOST_VM_INFO64,
                            reinterpret_cast<host_info64_t>(&vm), &cnt) != KERN_SUCCESS)
        return;

    const std::uint64_t page = sysctl_num<std::uint64_t>("hw.pagesize")
                                   .value_or(static_cast<std::uint64_t>(page_size_));
    std::uint64_t in = vm.swapins, out = vm.swapouts;
    std::uint64_t di   = in  > prev_pswpin_  ? in  - prev_pswpin_  : 0;
    std::uint64_t dout = out > prev_pswpout_ ? out - prev_pswpout_ : 0;
    m.swap_in  = first_ ? ByteRate{0} : rate(Bytes{di * page}, dt);
    m.swap_out = first_ ? ByteRate{0} : rate(Bytes{dout * page}, dt);
    prev_pswpin_ = in;
    prev_pswpout_ = out;

    // File-backed paging traffic + total fault rate — the "how hard is the
    // VM system working" trio Process-Hacker-style panes want.
    std::uint64_t pgin = vm.pageins, pgout = vm.pageouts, flt = vm.faults;
    std::uint64_t dpi = pgin  > prev_pgin_  ? pgin  - prev_pgin_  : 0;
    std::uint64_t dpo = pgout > prev_pgout_ ? pgout - prev_pgout_ : 0;
    std::uint64_t dfl = flt   > prev_faults_ ? flt  - prev_faults_ : 0;
    m.page_in   = first_ ? ByteRate{0} : rate(Bytes{dpi * page}, dt);
    m.page_out  = first_ ? ByteRate{0} : rate(Bytes{dpo * page}, dt);
    m.faults_ps = first_ || dt <= 0 ? 0 : static_cast<double>(dfl) / dt;
    prev_pgin_ = pgin;
    prev_pgout_ = pgout;
    prev_faults_ = flt;
}

}  // namespace rockbottom
