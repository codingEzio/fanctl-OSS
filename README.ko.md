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

`3000`, `max`, `auto`는 팬마다 실제 RPM을 “변경 전 → 변경 후”로 표시합니다. `stat`은 현재 RPM과 모드만 표시하고, `stat --verbose`는 목표, 범위, 원시 모드 값을 추가합니다. 팬 번호는 1부터 시작합니다. 숫자 목표는 모든 팬의 허용 범위 안이어야 하며 `max`는 각 팬의 최대값을 사용합니다.

```text
Fan 01  ▰▰▰▰▰···········   2000 RPM  Manual
        from 2000 RPM  ·  target 2500
```

대화형 터미널은 팬별 최대 회전수를 기준으로 실측 RPM을 황색에서 적색으로 변하는 막대에 표시합니다. 폭이 좁으면 숫자와 모드를 표시합니다. 리디렉션 출력은 애니메이션 없는 일반 텍스트입니다. `NO_COLOR` / `--no-color`는 색상을 끄고 `TERM=dumb`는 터미널 장식을 끕니다. 로그인 사용자의 macOS 동작 줄이기 설정도 진행 애니메이션을 끕니다. CLI는 영어를 유지합니다. 자동화에서는 `--non-interactive`로 비밀번호 프롬프트를 금지하며 인증이 없으면 실패합니다.

제어권 전환은 모든 팬을 함께 처리하며 최대 30초 동안 확인합니다. 알려진 보호 모드 응답(`0x82`)만 재시도하고 다른 쓰기 오류는 실패로 처리합니다. 모드 `3`은 수동이 아닌 시스템 자동 제어입니다. 기본적으로 제어가 확인되면 회전수를 한 번 읽고 종료합니다. `--wait`를 붙이면 최대 8초 동안 관찰합니다. 최종 숫자는 실측값입니다. 아직 도달하지 못한 수동 목표는 따로 표시합니다. 변경 중 `SIGINT` 또는 `SIGTERM`을 받으면 자동 모드 복구와 확인을 시도한 뒤 종료합니다. 신호가 fanctl에 도달해야 하며 인증 래퍼만 종료하는 경로는 검증하지 않았습니다.

## 선택 소스 빌드

`fanctl.c`가 포함된 내보낸 소스 폴더에서 Apple Command Line Tools를 사용합니다. 타사 패키지를 다운로드하지 않습니다.

```sh
xcrun clang -std=c17 -O3 -Wall -Wextra -Werror \
  -framework IOKit -framework CoreFoundation fanctl.c -o fanctl
./fanctl --help
sh tests/fanctl_test.sh
```

## 권한과 상태

`stat`은 현재 사용자 권한으로 읽고 IOKit이 접근을 거부할 때만 인증을 요청합니다. 속도 변경에는 관리자 권한이 필요합니다(독립 빌드에서는 `/usr/bin/sudo` 사용). 소유자 관리 빌드는 먼저 시스템 sudo 인증 캐시를 프롬프트 없이 확인합니다. 유효하면 비대화형 sudo를 사용하고 없으면 로컬 요청은 인접 네이티브 helper를 askpass로 지정해 `sudo -A`를 사용하며 sudo가 정상 캐시를 만듭니다. 수신 SSH는 기존 터미널 인증을 유지합니다. 비밀번호를 저장하거나 sudo 정책을 바꾸지 않습니다. `--help`와 잘못된 인수는 권한을 요청하지 않습니다. 빌드 선택 `FANCTL_USE_ADJACENT_AUTHORIZER`의 기본값은 `0`, 소유자 설치기는 `1`이며 다른 값은 컴파일 오류입니다. 선택한 helper가 없으면 다른 경로로 바꾸지 않고 오류를 표시합니다. 재사용 범위는 sudo 세션 정책을 따르며 다른 터미널 세션은 별도 인증이 필요할 수 있습니다.

명령이 끝나면 종료합니다. 강제 목표는 `auto`, macOS 또는 다른 제어기가 바꿀 때까지 유지됩니다. 데몬, 로그인 항목, 저장 프로필, 원격 측정은 설치하지 않습니다. 호환성은 Mac의 AppleSMC 키에 달려 있으며 모든 Mac의 실기기 검수를 주장하지 않습니다.

컨트롤러에 쓴 뒤 변경이 실패하면 fanctl은 모든 팬을 자동 모드로 되돌리고 제어 키를 확인하려고 시도합니다. 복구 중 잠금 해제 반영 확인과 제어 해제 확인에는 각각 30초의 확인 시간을 둡니다. 수락된 잠금 해제 요청이 대기 중이면 반영 후 해제 플래그를 되돌려 복구 뒤 늦게 활성화되는 것을 막습니다. 이미 자동인 값은 다시 쓰지 않습니다. 예상하지 못한 쓰기 실패나 읽기 실패는 오류로 처리합니다. 확인하지 못한 키를 보고하며 이전 수동 설정은 복원하지 않습니다. 알 수 없는 제어 키 형식이나 불완전한 SMC 응답은 오류로 보고합니다. 강제 종료, 정전 또는 컨트롤러 사용 불가로 복구하지 못할 수 있습니다. 복구가 불완전하면 `fanctl auto`를 실행하고 `fanctl stat`으로 확인하세요.

Mac 한 대에서 임시 목표 회전수 설정과 자동 모드 전환을 실제로 확인했습니다. 쓰기 실패 후 복구는 모의 환경에서만 확인했으며 다른 Mac에서는 검증하지 않았습니다.

## 소스와 확인

테스트는 인증을 모의하고 SMC 접근을 거부하며 빌드／help, 인수 전달과 실패 처리를 확인합니다. 팬 속도를 바꾸거나 실제 하드웨어 동작을 입증하지 않습니다. CLI 출력은 영어입니다. 선택된 코드, 문서, 테스트와 자체 마크는 [GPL-2.0-only](LICENSE)이며 [타사 고지](THIRD_PARTY_NOTICES.md)를 보존합니다.
