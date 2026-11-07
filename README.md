# fanctl
<!-- Modified by fanctl contributors on 2026-11-07: document automatic recovery. -->

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

`stat` prints current speed, requested target, allowed range and auto/forced mode. A numeric target applies only when it is inside every fan's allowed range. Current RPM can take time to reach the target. `max` uses each fan’s own maximum.

## Build the selected source

In the exported source folder containing `fanctl.c`, use Apple’s Command Line Tools. No third-party package download is needed.

```sh
xcrun clang -std=c17 -O3 -Wall -Wextra -Werror \
  -framework IOKit -framework CoreFoundation fanctl.c -o fanctl
./fanctl --help
sh tests/fanctl_test.sh
```

## Permissions and state

Standalone builds use `/usr/bin/sudo` for all SMC commands, including `stat`. The managed owner build explicitly selects its existing adjacent authorization helper. `--help` and invalid arguments do not request access. `FANCTL_USE_ADJACENT_AUTHORIZER` is a build choice: default `0`, owner installer `1`; other values fail compilation. A missing chosen helper fails visibly without silently switching paths.

fanctl exits after each command. A forced target remains until `auto`, macOS or another controller changes it. It installs no daemon, login item, saved profile or telemetry. Compatibility depends on the Mac’s AppleSMC keys; this is a personal tool, with no all-Mac acceptance claim.

If a change fails after a controller write, fanctl attempts to return every fan to automatic mode and verifies the control keys. It may recheck valid nonzero keys up to eight times; failed writes or unreadable keys remain errors. It reports keys it cannot confirm; it does not restore earlier manual settings. Unknown control-key formats and incomplete SMC replies fail visibly. Forced termination, power loss or an unavailable controller can prevent recovery. If recovery is incomplete, run `fanctl auto` and check `fanctl stat`.

On one Mac, a temporary target and the command to return to automatic mode were checked on real hardware. Recovery after a failed write has only been simulated; other Macs are unverified.

## Source and checks

The test uses fake authorization and rejects any SMC access. It checks build/help, argument transfer and failure handling. It does not change fan speed or prove live hardware behavior. The CLI output remains English. Selected code, docs, tests and the original mark use [GPL-2.0-only](LICENSE); preserve [third-party notices](THIRD_PARTY_NOTICES.md).
