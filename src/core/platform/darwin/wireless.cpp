// collectors/wireless.cpp — macOS has no portable public signal/link API that
// maps cleanly onto Wireless (CoreWLAN is app-entitlement gated and cellular
// isn't exposed at all on Macs). The net collector already reports interface
// rates, so this backend is a deliberate no-op: Wireless stays empty and the
// UI omits the section, exactly as on a desktop Linux box without Termux.

#include "../../sampler.hpp"

namespace rockbottom {

void Sampler::sample_wireless(Wireless&) {}

}  // namespace rockbottom
