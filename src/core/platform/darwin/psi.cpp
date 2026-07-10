// platform/darwin/psi.cpp — no PSI on macOS.
//
// Pressure Stall Information is a Linux-kernel feature (/proc/pressure/*). macOS
// exposes no equivalent, so every entry stays unavailable. The verdict engine
// already guards on PsiEntry::available and simply weighs its other signals
// (CPU / memory / swap / load) more heavily when PSI is absent — so this empty
// implementation is correct, not a gap.

#include "../../sampler.hpp"

namespace rockbottom {

void Sampler::sample_psi(Psi& p) {
    p.cpu.available = false;
    p.mem.available = false;
    p.io.available  = false;
}

}  // namespace rockbottom
