# fanctl
<!-- Modified by fanctl contributors on 2026-11-15: document control handoff and concise terminal output. -->

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

`3000`、`max` 和 `auto` 只显示每只风扇实测的“操作前 → 操作后”RPM。`stat` 只列当前转速和模式；`stat --verbose` 才展开目标、范围和原始模式值。风扇从 1 编号。数字目标必须落在每只风扇的允许范围内；`max` 使用各自上限。

```text
🌀 Fan 1   1200 →  2997 RPM
🌀 Fan 2   1200 →  3002 RPM
```

交互终端显示轻量进度、风扇图标和颜色；重定向输出为无动画的纯文本。`NO_COLOR` / `--no-color` 关闭颜色，`TERM=dumb` 关闭终端装饰。登录用户的 macOS“减少动态效果”设置会关闭进度动画。CLI 保留英文。

交权最多轮询 30 秒，所有风扇一起处理；仅对已知的保护模式回复（`0x82`）重试，其他写入错误仍报错。模式 `3` 属于系统自动控制，不是手动。最多观察实际转速八秒；最终数字均为实测值，尚未达到的手动目标单独注明。变更中收到 `SIGINT` 或 `SIGTERM`，会先尝试恢复并验证自动模式再退出。信号必须到达 fanctl；仅终止授权包装程序不属于已验证的取消路径。

## 构建精选来源

在含 `fanctl.c` 的导出来源文件夹中，用 Apple Command Line Tools 构建，不下载第三方包。

```sh
xcrun clang -std=c17 -O3 -Wall -Wextra -Werror \
  -framework IOKit -framework CoreFoundation fanctl.c -o fanctl
./fanctl --help
sh tests/fanctl_test.sh
```

## 权限与状态

独立构建对所有 SMC 指令（包括 `stat`）使用 `/usr/bin/sudo`。主人托管构建先无提示检查系统 sudo 授权缓存；缓存有效时用非交互 sudo，否则使用现有的相邻授权 helper。不保存密码，也不修改 sudo 策略。`--help` 与无效参数不请求权限。`FANCTL_USE_ADJACENT_AUTHORIZER` 是构建选择：默认 `0`，主人安装器为 `1`，其他值拒绝编译；选定 helper 不存在会明确失败，不自动换路径。

每条指令完成后 fanctl 就退出；强制目标持续到 `auto`、macOS 或其他控制器更改它。没有常驻程序、登录项、保存配置或遥测。兼容性取决于 Mac 的 AppleSMC 键；这是个人工具，不声称所有 Mac 都已验收。

如果控制器写入后更改失败，fanctl 会尝试让所有风扇回到自动模式并验证控制键。恢复中的解锁确认和释放确认，各有 30 秒轮询窗口。已接受的解锁仍在途中时，先等它生效再释放，防止恢复完成后才延迟解锁。已自动的值不重复写入；未知写入失败或无法读取的键仍会报错。无法确认的键会明确报告；它不会恢复此前的手动设置。无法识别的控制键格式或不完整的 SMC 响应会明确报错。强制终止、断电或控制器不可用可能阻止恢复。若恢复不完整，请运行 `fanctl auto` 并用 `fanctl stat` 检查。

已在一台 Mac 实测设置暂时转速目标并切回自动；写入失败后的恢复仅经过模拟，其他 Mac 尚未验证。

## 来源与检查

测试使用模拟授权，并拒绝任何 SMC 访问；检查构建／help、参数转交与失败处理，不改变转速，也不证明真实硬件行为。CLI 输出维持英文。精选代码、文档、测试及原创图标采用 [GPL-2.0-only](LICENSE)，保留[第三方署名](THIRD_PARTY_NOTICES.md)。
