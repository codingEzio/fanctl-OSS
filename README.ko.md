# fanctl
<!-- Modified by fanctl contributors on 2026-11-07: document automatic recovery. -->

[English](README.md) · [简体中文](README.zh-Hans.md) · [繁體中文](README.zh-Hant.md) · [日本語](README.ja.md) · [한국어](README.ko.md) · [Español](README.es.md)

<p><img src="Assets/icon.svg" alt="fanctl" width="96" height="96"></p>

Mac 터미널에서 팬 상태를 읽고 임시 회전수를 지정하려고 직접 쓰는 작은 명령입니다. 제품 자체가 CLI입니다. 짧은 명령 네 개로 쓰며 앱 창을 계속 열어 둘 필요가 없습니다.

## 명령 네 개

```sh
fanctl stat       # read current state
fanctl 3000       # one RPM target for all fans
fanctl max        # each fan's own maximum
fanctl auto       # hand control back to macOS
```

`stat`은 현재 회전수, 목표, 허용 범위와 자동／강제 모드를 보여 줍니다. 숫자 목표는 모든 팬의 허용 범위 안에서만 받습니다. 실제 RPM이 목표에 도달하려면 시간이 걸릴 수 있습니다. `max`는 팬별 최대값을 사용합니다.

## 선택 소스 빌드

`fanctl.c`가 포함된 내보낸 소스 폴더에서 Apple Command Line Tools를 사용합니다. 타사 패키지를 다운로드하지 않습니다.

```sh
xcrun clang -std=c17 -O3 -Wall -Wextra -Werror \
  -framework IOKit -framework CoreFoundation fanctl.c -o fanctl
./fanctl --help
sh tests/fanctl_test.sh
```

## 권한과 상태

독립 빌드는 `stat`을 포함한 SMC 명령에 `/usr/bin/sudo`를 사용합니다. 소유자 관리 빌드는 기존 인접 인증 helper를 명시적으로 선택합니다. `--help`와 잘못된 인수는 권한을 요청하지 않습니다. 빌드 선택 `FANCTL_USE_ADJACENT_AUTHORIZER`의 기본값은 `0`, 소유자 설치기는 `1`이며 다른 값은 컴파일 오류입니다. 선택한 helper가 없으면 다른 경로로 바꾸지 않고 오류를 표시합니다.

명령이 끝나면 종료합니다. 강제 목표는 `auto`, macOS 또는 다른 제어기가 바꿀 때까지 유지됩니다. 데몬, 로그인 항목, 저장 프로필, 원격 측정은 설치하지 않습니다. 호환성은 Mac의 AppleSMC 키에 달려 있으며 모든 Mac의 실기기 검수를 주장하지 않습니다.

컨트롤러에 쓴 뒤 변경이 실패하면 fanctl은 모든 팬을 자동 모드로 되돌리고 제어 키를 확인하려고 시도합니다. 유효하지만 0이 아닌 키는 최대 8회 확인하며 쓰기나 읽기 실패는 오류로 처리합니다. 확인하지 못한 키를 보고하며 이전 수동 설정은 복원하지 않습니다. 알 수 없는 제어 키 형식이나 불완전한 SMC 응답은 오류로 보고합니다. 강제 종료, 정전 또는 컨트롤러 사용 불가로 복구하지 못할 수 있습니다. 복구가 불완전하면 `fanctl auto`를 실행하고 `fanctl stat`으로 확인하세요.

Mac 한 대에서 임시 목표 회전수 설정과 자동 모드 전환을 실제로 확인했습니다. 쓰기 실패 후 복구는 모의 환경에서만 확인했으며 다른 Mac에서는 검증하지 않았습니다.

## 소스와 확인

테스트는 인증을 모의하고 SMC 접근을 거부하며 빌드／help, 인수 전달과 실패 처리를 확인합니다. 팬 속도를 바꾸거나 실제 하드웨어 동작을 입증하지 않습니다. CLI 출력은 영어입니다. 선택된 코드, 문서, 테스트와 자체 마크는 [GPL-2.0-only](LICENSE)이며 [타사 고지](THIRD_PARTY_NOTICES.md)를 보존합니다.
