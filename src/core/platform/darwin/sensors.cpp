// platform/darwin/sensors.cpp — temperature sensors on macOS.
//
// macOS has no public temperature API: CPU/NVMe temps live behind the private
// AppleSMC framework (undocumented 4-char keys, subject to change), which we
// deliberately don't link. So this collector is a clean no-op — the Sensors
// section simply doesn't appear. The GPU pane still shows GPU temp via the
// public IOAccelerator stats, and the battery pane its own figure; those don't
// route through here. Kept as a real TU so the orchestrator calls the same
// sample_sensors() on both platforms.

#include "../../sampler.hpp"

namespace rockbottom {

void Sampler::sample_sensors(std::vector<Sensor>& out) {
    out.clear();
    // No public SMC access — intentionally empty on macOS.
}

}  // namespace rockbottom
