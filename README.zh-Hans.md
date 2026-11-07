# fanctl
<!-- Modified by fanctl contributors on 2026-11-07: document automatic recovery. -->

[English](README.md) · [简体中文](README.zh-Hans.md) · [繁體中文](README.zh-Hant.md) · [日本語](README.ja.md) · [한국어](README.ko.md) · [Español](README.es.md)

<p><img src="Assets/icon.svg" alt="fanctl" width="96" height="96"></p>

我自己在 Mac 终端读取风扇状态、临时指定转速时会用的小命令。产品就是 CLI：四条短指令，不用留着一个 App 窗口。

## 四条指令

```sh
fanctl stat       # read current state
fanctl 3000       # one RPM target for all fans
fanctl max        # each fan's own maximum
fanctl auto       # hand control back to macOS
```

`stat` 列出实际转速、目标、允许范围与自动／强制模式。数字目标必须落在每个风扇的允许范围内；实际 RPM 可能需要一段时间才达到目标。`max` 使用各风扇自己的上限。

## 构建精选来源

在含 `fanctl.c` 的导出来源文件夹中，用 Apple Command Line Tools 构建，不下载第三方包。

```sh
xcrun clang -std=c17 -O3 -Wall -Wextra -Werror \
  -framework IOKit -framework CoreFoundation fanctl.c -o fanctl
./fanctl --help
sh tests/fanctl_test.sh
```

## 权限与状态

独立构建对所有 SMC 指令（包括 `stat`）使用 `/usr/bin/sudo`。主人托管构建明确选择现有的相邻授权 helper。`--help` 与无效参数不请求权限。`FANCTL_USE_ADJACENT_AUTHORIZER` 是构建选择：默认 `0`，主人安装器为 `1`，其他值拒绝编译；选定 helper 不存在会明确失败，不自动换路径。

每条指令完成后 fanctl 就退出；强制目标持续到 `auto`、macOS 或其他控制器更改它。没有常驻程序、登录项、保存配置或遥测。兼容性取决于 Mac 的 AppleSMC 键；这是个人工具，不声称所有 Mac 都已验收。

如果控制器写入后更改失败，fanctl 会尝试让所有风扇回到自动模式并验证控制键。有效但非零的键最多重新检查八次；写入失败或无法读取的键仍会报错。无法确认的键会明确报告；它不会恢复此前的手动设置。无法识别的控制键格式或不完整的 SMC 响应会明确报错。强制终止、断电或控制器不可用可能阻止恢复。若恢复不完整，请运行 `fanctl auto` 并用 `fanctl stat` 检查。

已在一台 Mac 实测设置暂时转速目标并切回自动；写入失败后的恢复仅经过模拟，其他 Mac 尚未验证。

## 来源与检查

测试使用模拟授权，并拒绝任何 SMC 访问；检查构建／help、参数转交与失败处理，不改变转速，也不证明真实硬件行为。CLI 输出维持英文。精选代码、文档、测试及原创图标采用 [GPL-2.0-only](LICENSE)，保留[第三方署名](THIRD_PARTY_NOTICES.md)。
