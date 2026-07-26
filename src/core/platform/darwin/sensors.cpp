// platform/darwin/sensors.cpp — die temperatures via AppleSMC.
//
// macOS has no /sys and no public temperature API, so this used to return an
// empty vector and every temperature in the UI rendered as a dash — on a
// platform whose thermals are the single most interesting thing about it.
//
// AppleSMC is a public IOKit service that opens for an ordinary uid, so the
// readings are available without root, without an entitlement, and without
// linking a private framework. smc.hpp does the protocol; this file decides
// which of the ~1600 published keys are worth showing and what to call them.
//
// Apple Silicon key families (verified on an M1):
//   Tp**  performance-cluster CPU die      Te**  efficiency-cluster CPU die
//   TG**  GPU die                          TB**  battery
//   Ts**  enclosure/skin                   TH**  storage
// There are several sensors per domain — different physical points on the die —
// and some read exactly 0 when their power domain is gated. We average the LIVE
// ones per domain instead of picking one, because any single point is noisy and
// a zero would drag a naive mean to nonsense.

#include "../../sampler.hpp"
#include "smc.hpp"

#include <algorithm>
#include <map>
#include <string>
#include <vector>

namespace rockbottom {
namespace {

// Which domain does an SMC temperature key belong to? Returns an empty name to
// drop the key entirely — most of the 1600 are voltages, currents, fan and
// power-management internals that would be noise in a system monitor.
struct Domain {
    const char* label;   // shown in the UI
    const char* zone;    // grouping key, matching the Linux collector's zones
};

Domain domain_of(const std::string& k) {
    if (k.size() < 2) return {nullptr, nullptr};

    // CPU clusters. The digit identifies the core/cluster index; we fold the
    // whole family into one figure per cluster because a per-key breakdown is
    // more detail than a reader can use.
    if (k[0] == 'T' && k[1] == 'p') return {"CPU performance", "cpu"};
    if (k[0] == 'T' && k[1] == 'e') return {"CPU efficiency",  "cpu"};

    if (k.rfind("TG", 0) == 0) return {"GPU", "gpu"};
    if (k.rfind("TB", 0) == 0) return {"Battery", "battery"};
    if (k.rfind("TH", 0) == 0) return {"Storage", "drive"};
    if (k.rfind("Ts", 0) == 0) return {"Enclosure", "skin"};
    // TCMz / TCHP style package-level CPU sensors.
    if (k.rfind("TC", 0) == 0) return {"CPU package", "cpu"};
    return {nullptr, nullptr};
}

}  // namespace

void Sampler::sample_sensors(std::vector<Sensor>& out) {
    out.clear();

    // One connection for the process lifetime: opening the user client is the
    // expensive part, and this collector runs on a 2s timer.
    static smc::Connection smc_conn;
    if (!smc_conn.ok()) return;   // locked-down environment: degrade to no sensors

    const std::vector<smc::Reading> readings = smc_conn.temperatures();
    if (readings.empty()) return;

    // Average the live sensors within each domain. `first` keeps insertion
    // order stable so the UI doesn't reshuffle between frames.
    struct Acc { double sum = 0; int n = 0; int first = 0; const char* zone = ""; };
    std::map<std::string, Acc> by_label;
    int seq = 0;

    for (const smc::Reading& r : readings) {
        const Domain d = domain_of(r.key);
        if (!d.label) continue;
        // A sensor reading exactly 0 means its power domain is gated, not that
        // the silicon is at freezing. Including it would drag the average down.
        if (r.value <= 1.0f) continue;

        Acc& a = by_label[d.label];
        if (a.n == 0) { a.first = seq++; a.zone = d.zone; }
        a.sum += r.value;
        ++a.n;
    }

    out.reserve(by_label.size());
    for (const auto& [label, a] : by_label) {
        Sensor s;
        s.label  = label;
        s.zone   = a.zone;
        s.temp_c = static_cast<float>(a.sum / a.n);
        // Apple publishes no per-sensor thresholds; leave them zero so the UI
        // falls back to its own scale rather than drawing a fake red line.
        s.high_c = 0;
        s.crit_c = 0;
        out.push_back(std::move(s));
    }

    // Group by zone, hottest first within a zone — same ordering contract the
    // Linux collector provides, so the UI reads identically on both.
    std::sort(out.begin(), out.end(), [](const Sensor& a, const Sensor& b) {
        if (a.zone != b.zone) return a.zone < b.zone;
        return a.temp_c > b.temp_c;
    });
}

}  // namespace rockbottom
