#!/bin/sh
# Modified by fanctl contributors on 2026-11-07: exercise recovery with mock SMC state.
set -eu
script_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
temporary=$(/usr/bin/mktemp -d /private/tmp/delete_after_use_fanctl_test.XXXXXX)
trap '/bin/rm -rf "$temporary"' EXIT HUP INT TERM
if [ -f "$script_dir/../Bin/fanctl.c" ]; then
  source="$script_dir/../Bin/fanctl.c"
  include='"../Bin/fanctl.c"'
else
  source="$script_dir/../fanctl.c"
  include='"../fanctl.c"'
fi
xcrun clang -std=c17 -O3 -Wall -Wextra -Werror -framework IOKit -framework CoreFoundation "$source" -o "$temporary/fanctl"
"$temporary/fanctl" --help > "$temporary/help"
grep -q 'fanctl <RPM>' "$temporary/help"
if "$temporary/fanctl" invalid > "$temporary/invalid" 2>&1; then exit 1; fi
for mode in 0 1; do
  xcrun clang -std=c17 -O2 -Wall -Wextra -Werror -framework IOKit -framework CoreFoundation \
    "-DFANCTL_SOURCE_INCLUDE=$include" "-DFANCTL_USE_ADJACENT_AUTHORIZER=$mode" \
    "$script_dir/fanctl_test.c" -o "$temporary/authorization-$mode"
  expected=/usr/bin/sudo
  if [ "$mode" = 1 ]; then expected=/fixture/bin/admin_authorize; fi
  for command in stat auto max 3000; do
    output=$("$temporary/authorization-$mode" "$command")
    test "$output" = "authorize=$expected executable=/fixture/bin/fanctl argument=$command"
  done
  if FANCTL_TEST_EXEC_FAIL=1 "$temporary/authorization-$mode" stat > "$temporary/error" 2>&1; then exit 1; fi
  grep -q 'Cannot request administrator access' "$temporary/error"
  if "$temporary/authorization-$mode" -1 > "$temporary/range" 2>&1; then exit 1; fi
  grep -q 'RPM must be a non-negative integer' "$temporary/range"
done
xcrun clang -std=c17 -O2 -Wall -Wextra -Werror -framework IOKit -framework CoreFoundation \
  "-DFANCTL_SOURCE_INCLUDE=$include" "$script_dir/smc_result_test.c" -o "$temporary/smc-result"
test "$("$temporary/smc-result" success)" = 'write-returned'
test "$("$temporary/smc-result" optional-success)" = 'optional-present=1'
test "$("$temporary/smc-result" optional-missing)" = 'optional-present=0'
test "$("$temporary/smc-result" optional-read-missing)" = 'optional-present=0'
for scenario in metadata-error read-error write-error optional-metadata-error optional-read-error; do
  if "$temporary/smc-result" "$scenario" > "$temporary/smc-error" 2>&1; then exit 1; fi
  grep -q 'SMC result 0x01' "$temporary/smc-error"
done
if "$temporary/smc-result" transport-error > "$temporary/smc-error" 2>&1; then exit 1; fi
grep -q 'Cannot read SMC key metadata: 0x' "$temporary/smc-error"
for scenario in optional-metadata-transport-error optional-read-transport-error; do
  if "$temporary/smc-result" "$scenario" > "$temporary/smc-error" 2>&1; then exit 1; fi
  grep -q 'Cannot read optional SMC key.*: 0x' "$temporary/smc-error"
  if grep -q 'continued-after-optional-write' "$temporary/smc-error"; then exit 1; fi
done
xcrun clang -std=c17 -O2 -Wall -Wextra -Werror -framework IOKit -framework CoreFoundation \
  "-DFANCTL_SOURCE_INCLUDE=$include" "$script_dir/recovery_test.c" -o "$temporary/recovery"
for scenario in success max-success auto-success auto-optional-missing force-optional-missing \
  preflight-error preflight-optional-error preexisting-force transport-after-write partial-recovery \
  status-error auto-partial optional-disappears missing-optional-after-write rpm-overflow invalid-range \
  invalid-fan-count fractional-fan-count large-fan-count invalid-mode invalid-actual \
  short-preflight short-after-write short-recovery-write short-auto \
  preflight-fs-format preflight-ftst-format preflight-mode-format preflight-fs-size \
  optional-vanishes-before-write auto-fs-format type-changed deferred-auto prewrite-handshake \
  recovery-smc-error delayed-auto persistent-nonzero verification-key-vanishes; do
  "$temporary/recovery" "$scenario" > "$temporary/recovery-output" 2> "$temporary/recovery-$scenario"
done
grep -q 'automatic mode verified' "$temporary/recovery-transport-after-write"
grep -q 'automatic mode verified' "$temporary/recovery-status-error"
grep -q 'automatic mode verified' "$temporary/recovery-missing-optional-after-write"
grep -q 'automatic mode verified' "$temporary/recovery-short-after-write"
grep -q 'automatic mode verified' "$temporary/recovery-type-changed"
grep -q 'automatic recovery incomplete' "$temporary/recovery-partial-recovery"
grep -q 'F0Md' "$temporary/recovery-partial-recovery"
grep -q 'F0Md' "$temporary/recovery-auto-partial"
grep -q 'F0Md write SMC result: 0x01' "$temporary/recovery-recovery-smc-error"
grep -q 'F0Md verify attempt 1: status=0 type=ui8  size=1 bytes=0100 monotonic=' "$temporary/recovery-delayed-auto"
grep -q 'F0Md verify attempt 8: status=0 type=ui8  size=1 bytes=0100 monotonic=' "$temporary/recovery-persistent-nonzero"
grep -q 'FS!  verify attempt 2: status=1' "$temporary/recovery-verification-key-vanishes"
grep -q 'automatic recovery incomplete' "$temporary/recovery-optional-disappears"
grep -q 'automatic recovery incomplete' "$temporary/recovery-optional-vanishes-before-write"
grep -q 'automatic recovery incomplete' "$temporary/recovery-short-recovery-write"
grep -q 'FS! ' "$temporary/recovery-auto-fs-format"
printf 'fanctl: build/help, authorization handoff, synthetic SMC and recovery tests passed; no SMC access.\n'
