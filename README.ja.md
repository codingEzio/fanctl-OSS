# fanctl
<!-- Modified by fanctl contributors on 2026-11-07: document automatic recovery. -->

[English](README.md) · [简体中文](README.zh-Hans.md) · [繁體中文](README.zh-Hant.md) · [日本語](README.ja.md) · [한국어](README.ko.md) · [Español](README.es.md)

<p><img src="Assets/icon.svg" alt="fanctl" width="96" height="96"></p>

Mac のターミナルでファンの状態を読み、一時的な回転数を指定するために自分で使う小さなコマンドです。製品は CLI そのもの。短い4コマンドで、開いたままのアプリ画面はありません。

## 4つのコマンド

```sh
fanctl stat       # read current state
fanctl 3000       # one RPM target for all fans
fanctl max        # each fan's own maximum
fanctl auto       # hand control back to macOS
```

`stat` は現在値、目標値、許容範囲、自動／固定モードを表示します。数値指定は全ファンの許容範囲内だけ受け付けます。現在の RPM が目標に達するまで時間がかかる場合があります。`max` は各ファン固有の最大値です。

## 選択ソースのビルド

`fanctl.c` を含むエクスポート先で Apple Command Line Tools を使います。外部パッケージのダウンロードは不要です。

```sh
xcrun clang -std=c17 -O3 -Wall -Wextra -Werror \
  -framework IOKit -framework CoreFoundation fanctl.c -o fanctl
./fanctl --help
sh tests/fanctl_test.sh
```

## 権限と状態

単独ビルドは `stat` を含む SMC コマンドに `/usr/bin/sudo` を使います。所有者管理ビルドは既存の隣接認証 helper を明示選択します。`--help` と無効な引数は権限を要求しません。ビルド設定 `FANCTL_USE_ADJACENT_AUTHORIZER` は既定 `0`、所有者用は `1`。他の値はコンパイル失敗です。指定 helper がなければ別経路へ切り替えず失敗を表示します。

各コマンド後に終了します。固定目標は `auto`、macOS、別の制御ツールが変更するまで残ります。デーモン、ログイン項目、保存プロファイル、テレメトリーはありません。対応可否は Mac の AppleSMC キー次第で、全機種の実機確認を主張しません。

コントローラーへの書き込み後に変更が失敗した場合、fanctl は全ファンを自動モードに戻し、制御キーを検証しようとします。有効だがゼロでないキーは最大8回確認し、書き込みや読み取りの失敗はエラーとして扱います。確認できないキーは報告しますが、以前の手動設定は復元しません。不明な制御キー形式や不完全な SMC 応答はエラーとして報告します。強制終了、停電、コントローラーへの接続不能により復旧できない場合があります。復旧が不完全なときは `fanctl auto` を実行し、`fanctl stat` で確認してください。

1台の Mac 実機で一時的な目標回転数の設定と自動モードへの切り替えを確認しました。書き込み失敗後の復旧は模擬環境のみで検証しており、他の Mac では未検証です。

## ソースと検査

テストは認証を模擬し、SMC アクセスを拒否してビルド／help、引数転送、失敗処理を確認します。回転数は変更せず、実機動作の証明ではありません。CLI 出力は英語です。選択したコード、文書、テスト、独自マークは [GPL-2.0-only](LICENSE)。[第三者の表示](THIRD_PARTY_NOTICES.md)を保持します。
