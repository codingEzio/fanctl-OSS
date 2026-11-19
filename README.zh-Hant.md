# fanctl
<!-- Modified by fanctl contributors on 2026-11-15: document control handoff and concise terminal output. -->

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

`3000`、`max` 和 `auto` 只顯示每隻風扇實測的「操作前 → 操作後」RPM。`stat` 只列目前轉速和模式；`stat --verbose` 才展開目標、範圍和原始模式值。風扇從 1 編號。數字目標必須落在每隻風扇的允許範圍內；`max` 使用各自上限。

```text
Fan 01  ▰▰▰▰▰···········   2000 RPM  Manual
        from 2000 RPM  ·  target 2500
```

互動終端以琥珀色至紅色的分段表顯示實測轉速，滿格對應該風扇的最高轉速。窄終端保留數字與模式。重新導向輸出為無動畫的純文字。`NO_COLOR` / `--no-color` 關閉顏色，`TERM=dumb` 關閉終端裝飾。登入使用者的 macOS「減少動態效果」設定會關閉進度動畫。CLI 保留英文。 自動化可加 `--non-interactive`：禁止密碼提示，授權不足時直接失敗。

交權最多輪詢 30 秒，所有風扇一起處理；僅對已知的保護模式回覆（`0x82`）重試，其他寫入錯誤仍回報失敗。模式 `3` 屬於系統自動控制，不是手動。預設在控制器確認後立即取樣並返回，加 `--wait` 可繼續觀察轉速最多八秒；最終數字均為實測值，尚未達到的手動目標另行註明。變更中收到 `SIGINT` 或 `SIGTERM`，會先嘗試恢復並驗證自動模式再結束。訊號必須到達 fanctl；僅終止授權包裝程式不屬於已驗證的取消路徑。

## 建置精選來源

在含 `fanctl.c` 的匯出來源資料夾中，用 Apple Command Line Tools 建置，不下載第三方套件。

```sh
xcrun clang -std=c17 -O3 -Wall -Wextra -Werror \
  -framework IOKit -framework CoreFoundation fanctl.c -o fanctl
./fanctl --help
sh tests/fanctl_test.sh
```

## 權限與狀態

`stat` 先以目前使用者讀取，遇到 IOKit 權限拒絕才要求授權。調速仍需管理員權限，獨立建置使用 `/usr/bin/sudo`。主人託管建置先無提示檢查系統 sudo 授權快取；快取有效時用非互動 sudo，否則本機請求使用 `sudo -A`，由相鄰原生 helper 提供 askpass，讓 sudo 正常建立快取；傳入 SSH 保留 helper 的終端流程。不儲存密碼，也不修改 sudo 策略。`--help` 與無效參數不要求權限。`FANCTL_USE_ADJACENT_AUTHORIZER` 是建置選擇：預設 `0`，主人安裝器為 `1`，其他值拒絕編譯；選定 helper 不存在會明確失敗，不自動換路徑。 重用範圍遵循 sudo 工作階段策略；不同終端工作階段可能需要各自授權。

每條指令完成後 fanctl 就結束；強制目標持續到 `auto`、macOS 或其他控制器更改它。沒有常駐程式、登入項目、儲存設定或遙測。相容性取決於 Mac 的 AppleSMC 鍵；這是個人工具，不聲稱所有 Mac 都已驗收。

若控制器寫入後變更失敗，fanctl 會嘗試讓所有風扇回到自動模式並驗證控制鍵。復原中的解鎖確認和釋放確認，各有 30 秒輪詢窗口。已接受的解鎖仍在途中時，先等它生效再釋放，防止復原完成後才延遲解鎖。已自動的值不重複寫入；未知寫入失敗或無法讀取的鍵仍會報錯。無法確認的鍵會明確回報；它不會還原先前的手動設定。無法識別的控制鍵格式或不完整的 SMC 回應會明確報錯。強制終止、斷電或控制器無法使用可能阻止復原。若復原不完整，請執行 `fanctl auto` 並用 `fanctl stat` 檢查。

已在一台 Mac 實測設定暫時轉速目標並切回自動；寫入失敗後的復原僅經模擬，其他 Mac 尚未驗證。

## 來源與檢查

測試使用模擬授權，並拒絕任何 SMC 存取；檢查建置／help、參數轉交與失敗處理，不改變轉速，也不證明真實硬體行為。CLI 輸出維持英文。精選程式、文件、測試及原創圖示採 [GPL-2.0-only](LICENSE)，保留[第三方署名](THIRD_PARTY_NOTICES.md)。
