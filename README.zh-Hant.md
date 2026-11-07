# fanctl
<!-- Modified by fanctl contributors on 2026-11-07: document automatic recovery. -->

[English](README.md) · [简体中文](README.zh-Hans.md) · [繁體中文](README.zh-Hant.md) · [日本語](README.ja.md) · [한국어](README.ko.md) · [Español](README.es.md)

<p><img src="Assets/icon.svg" alt="fanctl" width="96" height="96"></p>

我自己在 Mac 終端讀取風扇狀態、暫時指定轉速時會用的小命令。產品就是 CLI：四條短指令，不用留著一個 App 視窗。

## 四條指令

```sh
fanctl stat       # read current state
fanctl 3000       # one RPM target for all fans
fanctl max        # each fan's own maximum
fanctl auto       # hand control back to macOS
```

`stat` 列出實際轉速、目標、允許範圍與自動／強制模式。數字目標必須落在每個風扇的允許範圍內；實際 RPM 可能需要一段時間才達到目標。`max` 使用各風扇自己的上限。

## 建置精選來源

在含 `fanctl.c` 的匯出來源資料夾中，用 Apple Command Line Tools 建置，不下載第三方套件。

```sh
xcrun clang -std=c17 -O3 -Wall -Wextra -Werror \
  -framework IOKit -framework CoreFoundation fanctl.c -o fanctl
./fanctl --help
sh tests/fanctl_test.sh
```

## 權限與狀態

獨立建置對所有 SMC 指令（包含 `stat`）使用 `/usr/bin/sudo`。主人託管建置明確選擇現有的相鄰授權 helper。`--help` 與無效參數不要求權限。`FANCTL_USE_ADJACENT_AUTHORIZER` 是建置選擇：預設 `0`，主人安裝器為 `1`，其他值拒絕編譯；選定 helper 不存在會明確失敗，不自動換路徑。

每條指令完成後 fanctl 就結束；強制目標持續到 `auto`、macOS 或其他控制器更改它。沒有常駐程式、登入項目、儲存設定或遙測。相容性取決於 Mac 的 AppleSMC 鍵；這是個人工具，不聲稱所有 Mac 都已驗收。

若控制器寫入後變更失敗，fanctl 會嘗試讓所有風扇回到自動模式並驗證控制鍵。有效但非零的鍵最多重新檢查八次；寫入失敗或無法讀取的鍵仍會報錯。無法確認的鍵會明確回報；它不會還原先前的手動設定。無法識別的控制鍵格式或不完整的 SMC 回應會明確報錯。強制終止、斷電或控制器無法使用可能阻止復原。若復原不完整，請執行 `fanctl auto` 並用 `fanctl stat` 檢查。

已在一台 Mac 實測設定暫時轉速目標並切回自動；寫入失敗後的復原僅經模擬，其他 Mac 尚未驗證。

## 來源與檢查

測試使用模擬授權，並拒絕任何 SMC 存取；檢查建置／help、參數轉交與失敗處理，不改變轉速，也不證明真實硬體行為。CLI 輸出維持英文。精選程式、文件、測試及原創圖示採 [GPL-2.0-only](LICENSE)，保留[第三方署名](THIRD_PARTY_NOTICES.md)。
