# fanctl
<!-- Modified by fanctl contributors on 2026-11-15: document control handoff and concise terminal output. -->

[English](README.md) · [简体中文](README.zh-Hans.md) · [繁體中文](README.zh-Hant.md) · [日本語](README.ja.md) · [한국어](README.ko.md) · [Español](README.es.md)

<p><img src="Assets/icon.svg" alt="fanctl" width="96" height="96"></p>

A small macOS command I use to read fan state and request a temporary fan speed from the terminal. The product is the CLI: four short commands, no app window to keep open.

## Four commands

```sh
fanctl stat       # read current state
fanctl 3000       # one RPM target for all fans
fanctl max        # each fan's own maximum
fanctl auto       # hand control back to macOS
```

`3000`, `max` and `auto` show each fan’s measured RPM before → after. `stat` shows only current RPM and mode; `stat --verbose` adds targets, ranges and raw mode values. Fans are numbered from 1. A numeric target must fit every fan’s allowed range; `max` uses each fan’s own maximum.

```text
🌀 Fan 1   1200 →  2997 RPM
🌀 Fan 2   1200 →  3002 RPM
```

Interactive terminals show a small progress spinner, a fan icon and color. Redirected output is plain text with no animation. `NO_COLOR` / `--no-color` disables color; `TERM=dumb` disables terminal decoration. The logged-in user’s macOS Reduce Motion setting disables the spinner. The CLI remains English.

Control handoff is polled for up to 30 seconds, with all fans handled together. Only the known protected-mode reply (`0x82`) is retried; other write errors remain failures. Mode `3` is system-controlled Auto, not manual. RPM is observed for up to eight seconds; final numbers are actual measurements, and an unmet manual target is shown separately. Receiving `SIGINT` / `SIGTERM` during a change attempts verified automatic recovery before exit. Signals must reach fanctl; stopping only an authorization wrapper is not a verified cancellation path.

## Build the selected source

In the exported source folder containing `fanctl.c`, use Apple’s Command Line Tools. No third-party package download is needed.

```sh
xcrun clang -std=c17 -O3 -Wall -Wextra -Werror \
  -framework IOKit -framework CoreFoundation fanctl.c -o fanctl
./fanctl --help
sh tests/fanctl_test.sh
```

## Permissions and state

Standalone builds use `/usr/bin/sudo` for all SMC commands, including `stat`. The managed owner build first checks the existing system sudo cache without prompting. While that cache is valid it uses noninteractive sudo; otherwise it selects the existing adjacent authorization helper. It stores no password and does not change sudo policy. `--help` and invalid arguments do not request access. `FANCTL_USE_ADJACENT_AUTHORIZER` is a build choice: default `0`, owner installer `1`; other values fail compilation. A missing chosen helper fails visibly without silently switching paths.

fanctl exits after each command. A forced target remains until `auto`, macOS or another controller changes it. It installs no daemon, login item, saved profile or telemetry. Compatibility depends on the Mac’s AppleSMC keys; this is a personal tool, with no all-Mac acceptance claim.

If a change fails after a controller write, fanctl attempts to return every fan to automatic mode and verifies the control keys. Each recovery transition has a 30-second polling budget. If an accepted unlock is still pending, it waits for that transition before releasing it; this prevents a late unlock from landing after recovery. Already-automatic values are not rewritten. Unexpected write failures and unreadable keys remain errors. It reports keys it cannot confirm; it does not restore earlier manual settings. Unknown control-key formats and incomplete SMC replies fail visibly. Forced termination, power loss or an unavailable controller can prevent recovery. If recovery is incomplete, run `fanctl auto` and check `fanctl stat`.

On one Mac, a temporary target and the command to return to automatic mode were checked on real hardware. Recovery after a failed write has only been simulated; other Macs are unverified.

## Source and checks

The test uses fake authorization and rejects any SMC access. It checks build/help, argument transfer and failure handling. It does not change fan speed or prove live hardware behavior. The CLI output remains English. Selected code, docs, tests and the original mark use [GPL-2.0-only](LICENSE); preserve [third-party notices](THIRD_PARTY_NOTICES.md).
