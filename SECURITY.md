# Security Policy

## Scope

`rb` is a local system monitor. It runs as an ordinary user, reads kernel
telemetry (`/proc`, `/sys`, sysctl, SMC, NVMe SMART), and can send signals to
processes the invoking user already has permission to signal. It opens no
sockets, serves nothing, and phones nowhere.

The interesting attack surface is therefore narrow but real:

- **Untrusted strings rendered to a terminal.** Process names, command lines,
  GPU process names and Wi-Fi SSIDs are attacker-controlled. Anything that
  reaches the screen must pass `sys::sanitize_display()`, which is UTF-8
  structural and strips C0, DEL and the C1 block (8-bit CSI/OSC/DCS/ST) — a
  terminal-escape-injection vector. A naive byte scan corrupts valid multi-byte
  text, so do not "simplify" it.
- **Signal fan-out.** `signal_process()` rejects non-positive pids: `kill(0,…)`
  hits our own process group and `kill(-1,…)` every process we may signal. A
  corrupted selection must never fan a signal out that way.
- **PID reuse.** A kill armed against pid N must not fire at a *different*
  process N that was born while the user sat on the confirmation prompt. The
  confirm path re-checks process start time and refuses when it cannot verify.
- **Subprocesses.** External helpers (`nvidia-smi`, `getent`) are spawned with
  `posix_spawn` and an absolute path or a fixed argv — never through a shell,
  and never with a `PATH` lookup that a caller could influence. This matters
  because users run `rb` under `sudo`.
- **Parsing hostile input.** Every collector parses text it did not write.
  Reads are bounded, deltas are guarded against counter wrap, and a malformed
  file must degrade to "no data" rather than to a crash.

## Reporting a vulnerability

Please **do not** open a public issue for a security problem.

Email **tfeayush@gmail.com** with:

- what the issue is and which file/function it lives in,
- how to reproduce it (a crafted process name, a `/proc` fixture, …),
- what you think the impact is.

You should get an acknowledgement within a few days. Fixes for anything
exploitable go out in a patch release, and you will be credited in the release
notes unless you would rather not be.

## Supported versions

The latest tagged release is supported. `rb` is a single binary with no runtime
dependencies, so upgrading is replacing one file.
