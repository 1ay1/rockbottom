// platform/linux/procfs.hpp — Linux /proc + /sys reading helpers.
//
// The portable primitives (slurp/trim/first_line/push_hist/user_of) live in
// platform/common/sys_util.hpp; this header re-exports them under the historic
// `procfs::` namespace so every Linux collector keeps compiling unchanged, and
// is the natural home for any /proc-specific parsing helpers Linux backends add.

#pragma once

#include "../common/sys_util.hpp"

namespace rockbottom::procfs {

using rockbottom::sys::first_line;
using rockbottom::sys::push_hist;
using rockbottom::sys::push_hist2;
using rockbottom::sys::slurp;
using rockbottom::sys::trim;
using rockbottom::sys::user_of;

}  // namespace rockbottom::procfs
