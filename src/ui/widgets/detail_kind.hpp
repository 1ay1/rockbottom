// widgets/detail_kind.hpp — the Detail enum, split out so lightweight headers
// (hit_ids.hpp) can name it without pulling in the whole detail-pane stack.

#pragma once

namespace rockbottom::ui {

// Which drill-down pane is open (None = main dashboard).
enum class Detail { None, Cpu, Mem, Net, Gpu, Disk, Proc };

}  // namespace rockbottom::ui
