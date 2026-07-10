// collectors/psi.cpp — /proc/pressure/{cpu,memory,io}.
//
// PSI (pressure stall information) is the kernel's own measurement of how
// much time tasks spent *waiting* for a resource — the exact question a
// human asks when the machine feels slow. Neither htop nor btop surfaces
// this; for bottom it feeds the verdict directly.

#include "../sampler.hpp"
#include "../procfs.hpp"

#include <cstdio>
#include <string>

namespace bottom {

using namespace procfs;

namespace {

PsiEntry parse_psi(const std::string& path) {
    PsiEntry e;
    std::string body = slurp(path.c_str());
    if (body.empty()) return e;   // kernel without CONFIG_PSI
    e.available = true;
    // Format:
    //   some avg10=0.00 avg60=0.00 avg300=0.00 total=12345
    //   full avg10=0.00 ...            (absent for cpu)
    std::size_t pos = 0;
    while (pos < body.size()) {
        std::size_t eol = body.find('\n', pos);
        std::string line = body.substr(pos, eol == std::string::npos ? std::string::npos : eol - pos);
        double avg10 = 0;
        if (line.rfind("some", 0) == 0)
            { std::sscanf(line.c_str(), "some avg10=%lf", &avg10); e.some_avg10 = avg10; }
        else if (line.rfind("full", 0) == 0)
            { std::sscanf(line.c_str(), "full avg10=%lf", &avg10); e.full_avg10 = avg10; }
        if (eol == std::string::npos) break;
        pos = eol + 1;
    }
    return e;
}

}  // namespace

void Sampler::sample_psi(Psi& p) {
    p.cpu = parse_psi("/proc/pressure/cpu");
    p.mem = parse_psi("/proc/pressure/memory");
    p.io  = parse_psi("/proc/pressure/io");
}

}  // namespace bottom
