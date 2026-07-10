// collectors/mem.cpp — /proc/meminfo.

#include "../sampler.hpp"
#include "../procfs.hpp"

#include <cstdint>
#include <fstream>
#include <sstream>
#include <string>
#include <unordered_map>

namespace bottom {

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
}

}  // namespace bottom
