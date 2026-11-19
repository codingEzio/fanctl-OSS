# fanctl
<!-- Modified by fanctl contributors on 2026-11-15: document control handoff and concise terminal output. -->

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

`fanctl`은 사용법을, `fanctl stat`은 팬별 현재 RPM을 표시합니다. 숫자는 모든 팬의 허용 범위 안에 있어야 합니다. `max`는 팬별 실측값과 각각의 최고 속도를 함께 표시합니다.

```text
Fan 1  2000 RPM
Fan 2  2016 RPM
```

암호 없는 설정에서는 새 요청을 `queued`로 표시하고 바로 터미널로 돌아갑니다. `set`은 제어기의 목표값을 확인했다는 뜻이며 팬은 계속 가속할 수 있습니다. 백그라운드에서 마지막 요청을 적용합니다. 실측값과 오류는 `fanctl stat`에서 확인할 수 있습니다. 암호 없는 설정이 없으면 확인 후 종료합니다.

## 선택 소스 빌드

`fanctl.c`가 포함된 내보낸 소스 폴더에서 Apple Command Line Tools를 사용합니다. 타사 패키지를 다운로드하지 않습니다.

```sh
xcrun clang -std=c17 -O3 -Wall -Wextra -Werror \
  -framework IOKit -framework CoreFoundation fanctl.c -o fanctl
./fanctl --help
sh tests/fanctl_test.sh
```

## 권한과 상태

조회와 이미 적용된 수동 목표값의 재지정은 읽기만 합니다. 변경 시 sudo가 기존 인증을 재사용합니다. 관리용 빌드는 로컬 암호 창을 사용할 수 있고 SSH는 원격 터미널을 사용합니다. 터미널이 없고 권한이 부족하면 즉시 종료합니다. root 소유의 `/Library/PrivilegedHelperTools/fanctl`에 숫자, `auto`, `max` 전용 권한을 부여할 수 있습니다.

명령이 끝나면 종료합니다. 강제 목표는 `auto`, macOS 또는 다른 제어기가 바꿀 때까지 유지됩니다. 데몬, 로그인 항목, 저장 프로필, 원격 측정은 설치하지 않습니다. 호환성은 Mac의 AppleSMC 키에 달려 있으며 모든 Mac의 실기기 검수를 주장하지 않습니다.

컨트롤러에 쓴 뒤 변경이 실패하면 fanctl은 모든 팬을 자동 모드로 되돌리고 제어 키를 확인하려고 시도합니다. 복구 중 잠금 해제 반영 확인과 제어 해제 확인에는 각각 30초의 확인 시간을 둡니다. 수락된 잠금 해제 요청이 대기 중이면 반영 후 해제 플래그를 되돌려 복구 뒤 늦게 활성화되는 것을 막습니다. 이미 자동인 값은 다시 쓰지 않습니다. 예상하지 못한 쓰기 실패나 읽기 실패는 오류로 처리합니다. 확인하지 못한 키를 보고하며 이전 수동 설정은 복원하지 않습니다. 알 수 없는 제어 키 형식이나 불완전한 SMC 응답은 오류로 보고합니다. 강제 종료, 정전 또는 컨트롤러 사용 불가로 복구하지 못할 수 있습니다. 복구가 불완전하면 `fanctl auto`를 실행하고 `fanctl stat`으로 확인하세요.

Mac 한 대에서 임시 목표 회전수 설정과 자동 모드 전환을 실제로 확인했습니다. 쓰기 실패 후 복구는 모의 환경에서만 확인했으며 다른 Mac에서는 검증하지 않았습니다.

## 소스와 확인

테스트는 인증을 모의하고 SMC 접근을 거부하며 빌드／help, 인수 전달과 실패 처리를 확인합니다. 팬 속도를 바꾸거나 실제 하드웨어 동작을 입증하지 않습니다. CLI 출력은 영어입니다. 선택된 코드, 문서, 테스트와 자체 마크는 [GPL-2.0-only](LICENSE)이며 [타사 고지](THIRD_PARTY_NOTICES.md)를 보존합니다.
