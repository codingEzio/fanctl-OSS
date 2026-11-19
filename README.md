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

Run `fanctl` for usage and `fanctl stat` for current RPM. A numeric target must fit every fan. `max` shows each fan’s current RPM and its own maximum.

```text
Fan 1  2000 RPM
Fan 2  2016 RPM
```

With passwordless control, a new target is marked `queued`; the terminal is immediately available. `set` means the controller target was verified. The motor may still be accelerating. A short-lived worker applies the latest request. Check `fanctl stat` for RPM and any background failure. Without passwordless control, the change waits for verification.

## Build the selected source

In the exported source folder containing `fanctl.c`, use Apple’s Command Line Tools. No third-party package download is needed.

```sh
xcrun clang -std=c17 -O3 -Wall -Wextra -Werror \
  -framework IOKit -framework CoreFoundation fanctl.c -o fanctl
./fanctl --help
sh tests/fanctl_test.sh
```

## Permissions and state

Status and an exact, already-applied manual target use a read-only check. Changes use system sudo, which reuses its normal authorization. A managed build can supply a local password prompt. SSH uses the controlling terminal. Without a terminal, sudo either has access or returns immediately. An optional root-owned helper at `/Library/PrivilegedHelperTools/fanctl` can be authorized separately for numeric targets, `max` and `auto`.

fanctl exits after each command. A forced target remains until `auto`, macOS or another controller changes it. It installs no daemon, login item, saved profile or telemetry. Compatibility depends on the Mac’s AppleSMC keys; this is a personal tool, with no all-Mac acceptance claim.

If a change fails after a controller write, fanctl attempts to return every fan to automatic mode and verifies the control keys. Each recovery transition has a 30-second polling budget. If an accepted unlock is still pending, it waits for that transition before releasing it; this prevents a late unlock from landing after recovery. Already-automatic values are not rewritten. Unexpected write failures and unreadable keys remain errors. It reports keys it cannot confirm; it does not restore earlier manual settings. Unknown control-key formats and incomplete SMC replies fail visibly. Forced termination, power loss or an unavailable controller can prevent recovery. If recovery is incomplete, run `fanctl auto` and check `fanctl stat`.

On one Mac, a temporary target and the command to return to automatic mode were checked on real hardware. Recovery after a failed write has only been simulated; other Macs are unverified.

## Source and checks

The test uses fake authorization and rejects any SMC access. It checks build/help, argument transfer and failure handling. It does not change fan speed or prove live hardware behavior. The CLI output remains English. Selected code, docs, tests and the original mark use [GPL-2.0-only](LICENSE); preserve [third-party notices](THIRD_PARTY_NOTICES.md).
