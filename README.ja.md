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

`fanctl` または `fanctl stat` で各ファンの現在の回転数を表示します。指定値は全ファンの許容範囲に収めてください。`max` は各ファンの最高回転数を使います。

```text
Fan 1  2000 RPM
Fan 2  2016 RPM
```

パスワード不要の制御を設定すると、実測回転数を表示してすぐに端末に戻ります。短時間だけ動くプロセスが最後に指定した設定を適用して確認します。失敗した場合は次の `fanctl` 実行時に表示します。停止状態からの切り替えには数秒かかる場合があります。パスワード不要の設定がなければ、確認後に終了します。

## 選択ソースのビルド

`fanctl.c` を含むエクスポート先で Apple Command Line Tools を使います。外部パッケージのダウンロードは不要です。

```sh
xcrun clang -std=c17 -O3 -Wall -Wextra -Werror \
  -framework IOKit -framework CoreFoundation fanctl.c -o fanctl
./fanctl --help
sh tests/fanctl_test.sh
```

## 権限と状態

状態と既に有効な手動設定の確認は読み取りだけで完了します。変更時は sudo が認証を再利用します。管理用ビルドはローカルのパスワード画面を使用でき、SSH は接続先の端末を使います。端末がなく認証できない場合は直ちに終了します。root 所有の `/Library/PrivilegedHelperTools/fanctl` に数値、`auto`、`max` だけの権限を付与できます。

各コマンド後に終了します。固定目標は `auto`、macOS、別の制御ツールが変更するまで残ります。デーモン、ログイン項目、保存プロファイル、テレメトリーはありません。対応可否は Mac の AppleSMC キー次第で、全機種の実機確認を主張しません。

コントローラーへの書き込み後に変更が失敗した場合、fanctl は全ファンを自動モードに戻し、制御キーを検証しようとします。復旧では解除要求の反映確認と制御フラグの復帰確認に、それぞれ30秒の確認枠を設けます。受理済みの解除要求が保留中なら、反映を待ってから解除フラグを戻し、復旧後に遅れて有効になることを防ぎます。既に自動の値は再書き込みしません。予期しない書き込み失敗や読み取り失敗はエラーとして扱います。確認できないキーは報告しますが、以前の手動設定は復元しません。不明な制御キー形式や不完全な SMC 応答はエラーとして報告します。強制終了、停電、コントローラーへの接続不能により復旧できない場合があります。復旧が不完全なときは `fanctl auto` を実行し、`fanctl stat` で確認してください。

1台の Mac 実機で一時的な目標回転数の設定と自動モードへの切り替えを確認しました。書き込み失敗後の復旧は模擬環境のみで検証しており、他の Mac では未検証です。

## ソースと検査

テストは認証を模擬し、SMC アクセスを拒否してビルド／help、引数転送、失敗処理を確認します。回転数は変更せず、実機動作の証明ではありません。CLI 出力は英語です。選択したコード、文書、テスト、独自マークは [GPL-2.0-only](LICENSE)。[第三者の表示](THIRD_PARTY_NOTICES.md)を保持します。
