# fanctl
<!-- Modified by fanctl contributors on 2026-11-15: document control handoff and concise terminal output. -->

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

`3000`、`max`、`auto` は各ファンの実測 RPM を「変更前 → 変更後」で表示します。`stat` は現在値とモードだけを表示し、`stat --verbose` で目標、範囲、モードの生値を確認できます。ファン番号は 1 からです。数値目標は全ファンの許容範囲内に限り、`max` は各ファン固有の上限を使います。

```text
🌀 Fan 1   1200 →  2997 RPM
🌀 Fan 2   1200 →  3002 RPM
```

対話端末では小さな進捗アニメーション、ファンのアイコン、色を使います。リダイレクト時は装飾のないテキストです。`NO_COLOR` / `--no-color` は色を無効にし、`TERM=dumb` は端末装飾を無効にします。ログインユーザーの macOS「視差効果を減らす」設定では進捗アニメーションを止めます。CLI は英語のままです。

制御の引き渡しは全ファンをまとめて最大30秒確認します。既知の保護モード応答（`0x82`）だけを再試行し、他の書き込みエラーは失敗とします。モード `3` は手動ではなくシステムの自動制御です。実際の回転数を最大8秒確認し、最終値には実測値を使い、未到達の手動目標は別記します。変更中に `SIGINT`／`SIGTERM` を受け取ると、自動モードへの復旧と確認を試みてから終了します。シグナルが fanctl に届く必要があり、認証ラッパーだけを停止する経路は未検証です。

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

コントローラーへの書き込み後に変更が失敗した場合、fanctl は全ファンを自動モードに戻し、制御キーを検証しようとします。復旧では解除要求の反映確認と制御フラグの復帰確認に、それぞれ30秒の確認枠を設けます。受理済みの解除要求が保留中なら、反映を待ってから解除フラグを戻し、復旧後に遅れて有効になることを防ぎます。既に自動の値は再書き込みしません。予期しない書き込み失敗や読み取り失敗はエラーとして扱います。確認できないキーは報告しますが、以前の手動設定は復元しません。不明な制御キー形式や不完全な SMC 応答はエラーとして報告します。強制終了、停電、コントローラーへの接続不能により復旧できない場合があります。復旧が不完全なときは `fanctl auto` を実行し、`fanctl stat` で確認してください。

1台の Mac 実機で一時的な目標回転数の設定と自動モードへの切り替えを確認しました。書き込み失敗後の復旧は模擬環境のみで検証しており、他の Mac では未検証です。

## ソースと検査

テストは認証を模擬し、SMC アクセスを拒否してビルド／help、引数転送、失敗処理を確認します。回転数は変更せず、実機動作の証明ではありません。CLI 出力は英語です。選択したコード、文書、テスト、独自マークは [GPL-2.0-only](LICENSE)。[第三者の表示](THIRD_PARTY_NOTICES.md)を保持します。
