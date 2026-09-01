# Contributing to rockbottom

Bug reports and patches welcome. This file is short on ceremony and long on the
few conventions that actually keep this codebase working.

## Building

```sh
git clone --recursive https://github.com/1ay1/rockbottom.git
cd rockbottom
cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
./build/rb
```

The `third_party/maya` submodule is required — a missing submodule is a
configure-time fatal error, not a confusing compile failure. On macOS the build
steers itself to Homebrew GCC (Apple Clang won't do `cxx_std_26`).

## Testing

```sh
ctest --test-dir build --output-on-failure   # unit + render + selfcheck
./build/rb --selfcheck                        # collectors, on this machine
./build/rb --topology                         # what the CPU probe decided
./build/rb --bench                            # sampler timing (RB_PHASE=1 for phases)
RB_DEBUG=1 ./build/rb --doctor                # per-collector health + why-empty
```

Warnings are errors in CI (`-Wall -Wextra -Wshadow -Werror`). Build clean
locally before pushing.

## Architecture in one screen

| Layer | Where | Rule |
|---|---|---|
| Units | `src/core/units.hpp` | Strong types. Mixing dimensions is a compile error. |
| Metrics | `src/core/metrics.hpp` | POD vocabulary only. No logic, no platform. |
| Collectors | `src/core/platform/<os>/*.cpp` | One file per domain; identical signatures across platforms. |
| Orchestration | `src/core/sampler.cpp` | Cadence, deltas, caches, threads. OS-agnostic only. |
| Diagnosis | `src/core/verdict.cpp` | Findings → the headline sentence. |
| UI | `src/ui/` | Elm architecture over maya. |

Adding a platform means adding `src/core/platform/<os>/` with the same set of
`Sampler::sample_*` definitions. CMake globs exactly one directory; core never
learns the OS.

## Conventions that are load-bearing

- **Degrade, never crash.** A collector that can't read its source leaves the
  field empty and returns. No exceptions escape a collector. If a probe comes
  back empty, record *why* in the doctor report rather than failing silently.
- **The sampler thread is what the UI waits on.** Anything slow, forkful, or
  capable of blocking in the kernel gets pushed off it — a cadence, a
  double-buffered future, or a `want_detail` gate. Never add an unbounded
  syscall to the hot path.
- **Sanitize at the data boundary.** Untrusted strings go through
  `sys::sanitize_display()` when collected, not when drawn.
- **Test pure logic as pure logic.** `verdict`, `proc_query`, `order_procs` and
  `units` are pure functions over PODs; new behaviour there needs a unit test in
  `tests/`. Rendering regressions get a headless canvas assertion.
- **Comment the *why*.** This codebase explains reasoning, not syntax. If a line
  looks odd because a platform is odd, say which platform and how it's odd.

## Pull requests

- One concern per PR, with a commit message that says what changed and why.
- CI must be green on all jobs — the portability assertions catch
  "works on my machine" before users do.
- If you touch a platform backend, say which OS/hardware you tested on. Nobody
  has every machine; an untested-on-Linux macOS fix is fine if you say so.
