<div align="center">

```
██████╗  ██████╗  ██████╗██╗  ██╗██████╗  ██████╗ ████████╗████████╗ ██████╗ ███╗   ███╗
██╔══██╗██╔═══██╗██╔════╝██║ ██╔╝██╔══██╗██╔═══██╗╚══██╔══╝╚══██╔══╝██╔═══██╗████╗ ████║
██████╔╝██║   ██║██║     █████╔╝ ██████╔╝██║   ██║   ██║      ██║   ██║   ██║██╔████╔██║
██╔══██╗██║   ██║██║     ██╔═██╗ ██╔══██╗██║   ██║   ██║      ██║   ██║   ██║██║╚██╔╝██║
██║  ██║╚██████╔╝╚██████╗██║  ██╗██████╔╝╚██████╔╝   ██║      ██║   ╚██████╔╝██║ ╚═╝ ██║
╚═╝  ╚═╝ ╚═════╝  ╚═════╝╚═╝  ╚═╝╚═════╝  ╚═════╝    ╚═╝      ╚═╝    ╚═════╝ ╚═╝     ╚═╝
▇▆▅▄▃▂▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁
```

</div>

<p align="center">
  <b>rb</b> — a system monitor for people who open <code>htop</code>, whisper <b>"what in god's name,"</b> and close it forever.<br>
  Your computer hit rock bottom. So did we. Now we watch it hit rock bottom <i>together</i>, and rockbottom reads the scary numbers <i>for</i> you and says, slowly and kindly: <b>"your computer is fine, buddy."</b>
</p>

<p align="center">
  <img src="assets/rockbottom.gif" alt="rockbottom in action" width="800">
</p>

<p align="center">
  <i>Built in type-theoretic C++26 on the <a href="https://github.com/1ay1/maya">maya</a> TUI framework — the code did a PhD so you could keep eating crayons.</i>
</p>

---

## The competition (a loving roast)

Let's go around the room.

- **`top`** — released the year *Ghostbusters* came out, and it looks it. A wall
  of beige numbers that refreshes with the grace of a slideshow at a funeral. It
  answers "is my computer okay?" by simply refusing to. `top` is not a tool, it's
  a hazing ritual for people who peaked in 1984.
- **`htop`** — `top` but in *color*, congratulations. Now the wall of numbers is
  a **rainbow** wall of numbers. You still have no idea what any of it means, but
  now it's festive. You will press `F5`, be delighted by the tree view for eight
  seconds, and then close it forever, exactly like last time.
- **`btop`** — okay, `btop` is *gorgeous*. Genuinely. It's also forty gauges, six
  graphs, braille art, and a theme engine, and it answers "**is something
  wrong?**" with "here are 900 data points, one of them is the answer, xoxo." It's
  a Ferrari dashboard bolted to a tricycle you don't know how to ride. Stunning.
  Useless at 2am when the fan sounds like a jet.
- **`nvtop`** — great! For the four minutes a year you remember it exists and it's
  installed and your GPU driver is in the mood. Otherwise it's a `command not
  found` with excellent intentions. (We just... put the GPU in the main app. Press
  `4`. You're welcome.)
- **`bottom`** (the Rust one, `btm`) — respectfully, they took the good name and
  built `htop` with rounded corners. It's fine! It's *fine.* It's a very polite
  restatement of the problem. We took the name back, added a **rock**, and
  actually answered the question. Rewrite it in Rust; we'll be over here telling
  people what's wrong with their computer.

Every one of them shows you the numbers. **None of them read the numbers.** That's
the whole bit. That's the entire gap in the market, and we drove a truck through it.

## Are you the target audience? A quiz

1. When your fan spins up, do you (a) check the process tree, or (b) sniff the
   air and go "is something burning?"
2. Is "have you tried turning it off and on again" not a joke to you but your
   entire IT strategy?
3. Have you ever killed the wrong process with such confidence, and then such
   immediate, profound regret?
4. Do you think "the cloud" is a place with weather?

If you answered anything other than a stunned, guilty silence: **welcome home.**
You are exactly who we built this for. We are not mad. We are *proud* of you for
getting this far into a README.

## The problem

`htop` and `btop` are beautiful. They are also forty gauges, six graphs, and a
300-row table of hexadecimal-looking gibberish, and they answer the question
"**is my computer okay?**" with a confident, resounding *"you figure it out,
champ."*

So you stare. You nod. You develop a deep, respectful fear of the numbers. You
pick a random process, kill it, and either the problem goes away (you are a
wizard) or your audio dies forever (you are not a wizard). This is not a system
monitor. This is a hostage situation with graphs.

## The fix: we read it for you

rockbottom employs a guy whose entire job is to stare at all that garbage, turn
to you, and say one (1) sentence:

```
╭──────────────────────────────────────────────────────────────╮
│ ▲ Working hard — CPU is heavily loaded   chrome (pid 4160) …  │
╰──────────────────────────────────────────────────────────────╯
```

That's the **verdict**. It's colour-coded, so even if you cannot read — no
judgment, you got this far by looking at the shapes — the *colour* tells you
everything:

- 🟢 **Calm** — everything's fine. Put the fire extinguisher down.
- 🟡 **Busy** — something's working. It's allowed. Breathe.
- 🟠 **Stressed** — okay, *now* you may look at the thing we highlighted in red.
- 🔴 **Critical** — this is a "close the tabs" situation. You have too many tabs.
  You always have too many tabs. It's a lifestyle at this point.

And the process actually causing the mess? We slap a big `»` on it and paint it
red so you don't have to walk down the list pointing at each row whispering
"is it you?? is it YOU???" like a detective who peaked in kindergarten. **It's
Chrome.** It is *always* Chrome. It was Chrome the whole time. `htop` knew and
said nothing. We're better than `htop`.

**You read the one sentence. You know. That's the app.** Everything below exists
for the two days a year you feel brave.

## Press a number, get the whole story (the detail panes)

Every panel has a **full-screen drill-down** that carries *more detail than htop,
btop, nvtop, or that polite Rust fellow* — and then, unlike all of them, reads it
for you. Hit a number — or just *click the panel* — and rockbottom throws the
doors open. Every pane has a system strip up top (host / kernel / uptime /
process census) and ends in a plain-language verdict, so you never leave staring
at digits wondering *so what?*

- **`1` CPU** — a big load-over-time mountain with a real y-axis, live total +
  iowait bars, and the 1/5/15 load averages **interpreted against your core
  count** ("oversubscribed — tasks are waiting for a free core"), because `htop`
  will show you `load average: 12.4` and let you *feel* about it. Plus package
  temp, a **distribution strip** (busiest / quietest / median / average / spread /
  active cores — it'll even rat out a single-threaded hog pinning one core), and
  **every core** as its own labelled meter with its live clock speed.
- **`2` MEMORY** — the usage trend graph (spot a leak before it OOMs you), a real
  physical breakdown (used / cache / buffers / available) with the *available*
  verdict that actually matters, the full swap story with a thrash detector, the
  kernel's PSI memory-pressure numbers, **and the top memory-hungry processes
  right in the pane** so you don't go hunting through a table like it's 1997.
- **`3` NETWORK** — an all-interfaces aggregate line (total down/up, lifetime
  bytes, links up), then **every interface** with live rx/tx sparklines, lifetime
  totals, a burst detector, and up/down state.
- **`4` GPU** — **it replaces `nvtop` entirely, and it's already installed,
  because it's just here.** Utilisation-over-time graph, core + VRAM meters, a
  full telemetry strip (temperature, power draw vs limit, core & memory clocks,
  fan, perf-state, NVENC/NVDEC encode/decode load), the processes actually holding
  VRAM, and a verdict ("▲ VRAM is nearly full" / "pinned at full load — this is
  your bottleneck"). Works with NVIDIA (`nvidia-smi`), AMD (`amdgpu` sysfs), and
  Intel; whatever your card won't cough up is simply left out. Multiple GPUs stack.
- **`5` DISK** — system-wide read/write sparklines, I/O pressure with a
  bottleneck verdict, **every filesystem** with its backing device, free space,
  used/size, fstype, and a fullness meter, the fullest-mount callout, and the
  **processes actually driving disk traffic** right now.
- **`6` / `Enter` PROCESS** — drill into the selected process: full command line,
  cpu + memory bars, **its rank against every other process** (so you instantly
  know if it's *the* hog: "#1 CPU consumer on the machine right now"), disk
  read/write, thread count, a *plain-language* run-state ("◆ uninterruptible —
  wedged on I/O, can't even be killed", not a cryptic `D` that you'll Google and
  forget), owner, and its listening ports. `x`/`K` still end it from right here;
  `↑↓` walks the list.

Every pane is **responsive** (it reflows for your terminal — more core columns on
a wide screen, tighter layout on a small one) and **scrollable** (`↑↓` / `PgUp` /
`PgDn` / `g` / `G` / the wheel, with a live scrollbar), so no matter how dense the
data or how small the window, nothing ever clips off the edge like it does in you-
know-what. `Esc` (or a click) closes; the number keys switch panes without going
back. It's the "okay, tell me *everything*" button — and it's gorgeous, and it
still tells you what it *means*.

## Everything else (for your brave days)

- **Verdict banner** — Calm / Busy / Stressed / Critical, with the guilty party
  named out loud in front of everyone. Public shaming as a service.
- **PSI pressure chips** — the Linux kernel keeps a little diary of how much your
  stuff is stuck waiting around (`/proc/pressure`). "tasks stalled on I/O 40% of
  the last 10s" is the kernel tattling on your disk. `htop` and `btop` are too
  polite to ask. We are not polite. We read the diary out loud.
- **CPU** — one big number ("am I busy?"), a bunch of little numbers ("which of
  my brains is busy?"), and a graph shaped like a mountain range that goes up when
  you do things and down when you don't. It's basically a mood ring.
- **Memory** — RAM + swap meters, plus the cache/available breakdown, so you can
  finally learn the sacred truth: **"used" memory is a filthy lie.** Linux hoards
  RAM like a dragon on a pile of gold. It is *supposed* to be almost full. Every
  other monitor lets you panic about this. We don't. Put the credit card down;
  you don't need more RAM.
- **Disk** — read/write speeds and your actual drives, fullest one first (btrfs
  subvolumes deduped, so you don't see the same disk nine times and think you own
  nine computers). Answers "why slow" and "am I out of room" in one look.
- **Network** — download ▼ and upload ▲ per interface, with cute little graphs.
  Watch the number go up. Feel like a hacker in a movie. You've earned it.
- **Processes** — pick things, filter things, and **kill** things. The loudest one
  comes pre-highlighted so you kill the *right* thing for once in your life. Dots
  tell you who's actually working (●) vs. who's just stuck waiting on the disk (◆),
  so you stop murdering sleeping programs on a hunch like some kind of process
  serial killer.
- **Per-process disk I/O** — the real, honest "who is sanding my SSD down to a
  nub" number, straight from `/proc/<pid>/io`. Sort by it with `i`. The verdict
  even hands you the culprit's name when the disk is the problem. (It's a backup
  you set up in 2019 and forgot. It's fine. It loves you. It just wanted
  attention.)
- **Ports** — which program is squatting on `:8080` like a smug little goblin
  (`:8080 +2`), so next time you get "address already in use" you can find the
  goblin instead of restarting your entire machine like a caveman. Sortable with
  `o`.
- **Battery** — a charge chip in the corner, because sometimes "why is it slow"
  has the deeply humbling answer "it's on 3% and screaming for a charger."

## "Wait, is it actually smart underneath, or just mean?" Both.

You get to be casual because the code is a paranoid genius. Every number has a
**type that knows what it is**, so the program *physically cannot* mix up bytes
and hertz and tell you your hard drive runs at 3.6 gigabytes-per-second of clock
speed. It won't compile. The compiler stands at the door like a bouncer and goes
"nah." Try that in a bash one-liner.

```cpp
using Bytes = Strong<BytesTag, std::uint64_t>;      // storage / memory
using Hertz = Strong<HertzTag, std::uint64_t>;      // clock speed
struct Ratio { double v; };                         // a proportion, clamped [0,1]
template <class Q> struct Rate { double per_sec; }; // Q, but per second

// The ONE legal way to turn Bytes into Bytes-per-second has a name and a
// hall pass, signed by a teacher:
constexpr ByteRate rate(Bytes delta, double dt_sec) noexcept;
```

That's why `4.2G` and `118M/s` and `3.60GHz` are always right — not because a
tired human remembered the unit at 2am, but because the *type* remembered it,
forever, so nobody has to. You are safe here. The nerds have handled it.

## Architecture (you may skip this; you were going to anyway)

rockbottom is a maya **Elm-style `Program`** — pure functions, no sneaky
mutations, the kind of clean code that would make your therapist proud:

| Layer | Where | Role |
|-------|-------|------|
| Units | `src/core/units.hpp` | Strong dimensional types + the humanizers that turn scary numbers into `4.2G` |
| Data | `src/core/metrics.hpp` | Pure `Snapshot` value types + the almighty `Verdict` |
| Sampler | `src/core/sampler.*` + `verdict.cpp` | OS-agnostic: orchestration, cross-tick deltas, and the diagnosis engine |
| Platform | `src/core/platform/<os>/` | The one grubby place that touches the real world — `linux/` reads `/proc` + `/sys`; `darwin/` uses mach / sysctl / libproc / IOKit. CMake compiles exactly one |
| App | `src/ui/app.hpp` | `Model` + `Msg` + `update` + a pure `view` |
| Widgets | `src/ui/widgets/` | Panels, meters, graphs, and the detail panes (one file each) |

No pane hand-counts breakpoints. Every layout is *measured*: maya's responsive
toolkit (`fit_row` sheds the header **and** the footer hints down as they
narrow, `pick` re-renders the detail tab bar at the richest density that
actually fits, `col` stacks the narrow-mode stat band, `solve_columns` keeps the
process table's header and rows on one width plan, `fill` sizes each graph to
exactly the height left over) means the thing measured is the thing drawn — so
nothing drifts off its rail at some cursed terminal width. See maya's
[Responsive Layouts](third_party/maya/docs/15-responsive.md) guide.

`update()` ticks once a second like a very calm heartbeat. `view()` is a pure
function of the state. The sampler remembers the last sample, so every rate is a
real delta and not a wild guess. It's genuinely tidy in here. Please take your
shoes off.

## Building

Needs a C++26 compiler (GCC 15+; built on GCC 16) and CMake 3.28+. Yes, it's a
fancy compiler. The type theory has *demands.* It's an artist.

**Linux:**

```bash
git clone --recurse-submodules https://github.com/1ay1/rockbottom
cd rockbottom
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
./build/rb
```

**macOS** (Apple Silicon or Intel) builds with Homebrew GCC — the same
toolchain maya's own CI uses. That's the whole story: no Clang, no libc++
gymnastics.

```bash
brew install gcc cmake
git clone --recurse-submodules https://github.com/1ay1/rockbottom
cd rockbottom
cmake -B build -DCMAKE_BUILD_TYPE=Release   # auto-picks Homebrew g++
cmake --build build -j
./build/rb
```

CMake finds Homebrew's newest `g++-N` for you (override with
`-DCMAKE_CXX_COMPILER=g++-15` or `CXX=...`). Apple's kernel headers use the C11
`_Static_assert` spelling that g++ rejects at namespace scope, so the macOS
backend aliases it to C++'s `static_assert` before including any SDK header
(`src/core/platform/darwin/mach_util.hpp`) — one line that lets plain GCC parse
all of mach / IOKit / libproc. Why not just use Clang? Apple/LLVM libc++ still
doesn't ship `std::move_only_function`, which maya requires; libstdc++ does. So
GCC it is.

Cloned it and forgot `--recurse-submodules`, you beautiful disaster?

```bash
git submodule update --init --recursive
```

There. We fixed it. You're welcome. We're not mad.

## Keys (mostly one hand — we know the other one's holding a snack)

| Key | Action |
|-----|--------|
| `↑↓` / `j k` | pick a process (`j k` is for people who use vim and won't shut up about it) |
| `/` | filter by name or pid (`Esc` un-does it when you inevitably typo) |
| `x` / `Del` | politely ask a process to leave (SIGTERM — it asks you first) |
| `K` | *aggressively* remove a process (SIGKILL — still asks first, we're not savages) |
| `y` / `n` | yes, commit the crime / no, I panicked, nevermind |
| `s` | cycle sort · `c` cpu · `m` mem · `i` i/o · `n` name · `P` pid · `o` port |
| `1`–`6` / `Enter` | open a detail pane (cpu · mem · net · gpu · disk · process) |
| `↑↓` / `PgUp`/`PgDn` / `g`/`G` | scroll the detail pane (the wheel works too) |
| `p` / `Space` | pause / resume (freeze the chaos so you can point at it) |
| `?` / `h` | help, for when all of this immediately leaves your brain |
| `q` / `Esc` | leave, go outside, touch grass |

**Or just use your mouse, you absolute animal.** Full mouse support, zero misses:

| Do this | Get that |
|---------|----------|
| **click a process** | selects it (no more arrow-key marathons) |
| **click a column header** | sorts by it (CPU, MEM, DISK, PORT, NAME) |
| **click a panel** | opens its detail pane |
| **click a footer hint** | fires that action — `?·help`, `space·pause`, `s·sort`, `x·end`, `K·kill`, `/·filter`, `q·quit` |
| **right-click a process** | arms an end (SIGTERM — still asks first) |
| **scroll wheel** | rolls the process list (or the detail pane, if one's open) |

The click-to-row math is anchored to the layout, so a click lands on *exactly*
the row you clicked, at every terminal size. No "close enough." We checked. `btop`
did not check.

## Platform

**Linux and macOS.** rockbottom is built as a thin OS-agnostic core (the
verdict engine, the Elm loop, the strong-typed units) sitting on a **platform
backend** — one directory per OS under `src/core/platform/`, and CMake compiles
exactly one. Adding an OS is adding a directory; the verdict never changes.

- **Linux** — reads `/proc` and `/sys` with its bare hands. Full fidelity: PSI
  pressure, per-core clocks, iowait split, per-process disk I/O, the works.
- **macOS** — talks to the kernel the native way: `host_processor_info` /
  `host_statistics64` (CPU + memory), `sysctl` (swap, uptime, model), `libproc`
  (processes + bound ports), IOKit (disk throughput, GPU, battery), `getifaddrs`
  (network). A few Linux-only signals have no macOS analogue — PSI, the iowait
  split, per-core clock — so those panes simply omit them, the same way the app
  already hides a GPU field your card won't report. Everything else is there.

No ncurses, no runtime dependencies, no phoning home to sell your fan speeds to
advertisers. On Windows? This still isn't for you — but honestly, props for
reading this far.

## License

MIT. Do whatever you want; we're not your dad. Vendored maya is MIT too.
Everybody's chill. Go be free.

---

<p align="center">
  <i>rockbottom: for when "is my computer okay?" deserved an actual answer, and you deserved to not have to think about it.</i>
</p>
