# eMMC 圖片上傳 GUI — 操作說明

`tools/asset_upload.py` 透過既有的 J-Link / RTT 連線，把圖片資產直接寫進板子的
eMMC。更換首頁圖片、錶盤或地圖圖磚，**不需要重新編譯韌體，也不需要重新燒錄**。

English version: [README_asset_upload.md](README_asset_upload.md)

```
你的圖片（PNG/JPG/BMP...）
     │  自動轉檔（png_to_bin.py）
     ▼
  .BIN（原始像素）──經 J-Link 的 RTT──▶ 韌體（UploadTask）──▶ eMMC（FAT）──▶ 螢幕
```

> GUI 介面上的文字是英文。本文件中的按鈕名稱一律保留英文原文（粗體），方便對照。

---

## 1. 環境需求

**板子端**

- apollo510b_evb，J-Link 已透過 USB 接上電腦。
- 韌體以 `WATCHFACE_ASSET_UPLOAD=1` 編譯（`gcc/Makefile` 預設即為 1）並正在執行。
  開機 log 應看到 `[upload] task started (RTT ch0 down)`。
- eMMC 已掛載。若開機 log 顯示 asset store 為 `FAIL`，上傳也不會成功。

**電腦端**

| 項目 | 用途 | 說明 |
|---|---|---|
| Python 3（含 tkinter） | 必要 | python.org 的 Windows 安裝程式已內含 tkinter。 |
| SEGGER J-Link 軟體 | 必要 | 提供 `JLinkGDBServerCL`，由它開出 RTT 連接埠。 |
| Pillow | 加入圖片（PNG/JPG/...）時 | `pip install pillow`。若只上傳現成的 `.BIN` 則不需要。 |

---

## 2. 快速上手

1. 先啟動 RTT 伺服器（見第 3 節）。
2. 執行 GUI：

   ```powershell
   python tools\asset_upload.py
   ```

3. 按 **Connect**，右欄會列出 eMMC 目前的檔案。
4. 用 **Add image / asset...** 或 **Add assets_out** 加入檔案。
5. 按 **Upload**。
6. 重置（reset）板子，即可看到新圖。

---

## 3. 先啟動 RTT 伺服器

GUI 本身不直接操作 J-Link，而是連到一個執行中 `JLinkGDBServer` 的 RTT telnet
連接埠（`127.0.0.1:19021`）。以下兩種方式擇一：

**方式 A — 使用 `build_gui.py`（最簡單）**

1. 執行 `launch_build_gui.bat`（或 `python build_gui.py`）。
2. 切到 **RTT log** 分頁，按 **Start RTT**。
3. 上傳期間請保持這個視窗開著。

**方式 B — 命令列**

```powershell
JLinkGDBServerCL -device AP510NFA-CBR -if SWD -speed 4000 -rtttelnetport 19021
```

> 連接埠 `19021` 寫死在工具裡（`asset_upload.py` 開頭的 `PORT`）。若你在
> `build_gui.py` 改過 **Telnet Port**，請把 `PORT` 改成相同的值。

---

## 4. 視窗介紹

```
┌─ eMMC asset upload ───────────────────────────────────────────────┐
│ Connected 127.0.0.1:19021                            [Disconnect] │ ← 狀態列／連線按鈕
│ To upload                        On the eMMC                      │
│ ┌──────────────────────────────┐ ┌──────────────────────────────┐ │
│ │ HOME.BIN      412232 bytes   │ │ HANDHR.BIN      5400 bytes   │ │ ← 左：待上傳清單
│ │ WF4BG.BIN     412232 bytes   │ │ HOME.BIN      412232 bytes   │ │   右：板上現況
│ └──────────────────────────────┘ └──────────────────────────────┘ │
│ [██████████████░░░░░░░░░░░░░░░░░░░░░░░░░░░]                       │ ← 目前檔案的進度
│ ┌───────────────────────────────────────────────────────────────┐ │
│ │ sending HOME.BIN ...                                          │ │ ← 訊息記錄
│ │   OK   [upload] ok HOME.BIN                                   │ │
│ └───────────────────────────────────────────────────────────────┘ │
│ [Add image / asset...][Add assets_out][Remove selected][Clear]    │
│                                   [Read eMMC contents] [Upload]   │
└───────────────────────────────────────────────────────────────────┘
```

| 區域 | 內容 |
|---|---|
| 狀態列 | `Not connected`（未連線）、`Connected 127.0.0.1:19021`（已連線）、`Connection failed`（連線失敗）、`Disconnected`（已中斷）。 |
| **To upload**（左欄） | 待上傳的檔案與大小。可多選（Ctrl／Shift + 點選）。 |
| **On the eMMC**（右欄） | 從韌體讀回的板上檔案清單。標記意義見第 6 節。 |
| 進度條 | 目前傳送中檔案的進度。 |
| 訊息記錄 | 每個步驟、結果與錯誤。出問題時請先看這裡。 |

### 按鈕說明

| 按鈕 | 功能 |
|---|---|
| **Connect / Disconnect** | 連線／中斷 RTT 連接埠。連線後會自動讀取一次 eMMC 內容。 |
| **Add image / asset...** | 開啟檔案選擇視窗。`.BIN` 直接加入；圖片（PNG、JPG、BMP、WEBP...）會先自動轉成 `.BIN` 再加入。可一次選多個檔案。 |
| **Add assets_out** | 一次加入 `tools/assets_out/` 下的所有檔案——也就是完整的現成資產組（21 個檔案，約 3.4 MB）。 |
| **Remove selected** | 從待上傳清單移除選取的項目。不會動到 eMMC。 |
| **Clear** | 清空待上傳清單。不會動到 eMMC。 |
| **Read eMMC contents** | 向韌體查詢檔案清單，並更新右欄。 |
| **Upload** | 開始上傳。左欄**有選取項目時只傳選取的**；沒有選取時全部上傳。 |

---

## 5. 操作步驟

### 5.1 連線

按 **Connect**。約 0.4 秒後工具會讀取 eMMC，訊息記錄會印出摘要，例如：

```
eMMC holds 21 files, 21 files, 7654321 KB free of 7812500 KB
  all 17 assets present.
```

若有缺檔，則會列出缺少哪些：

```
  missing assets (5/17): WF4BG.BIN, WF4CAP.BIN, WF4HR.BIN, WF4MIN.BIN, WF4SEC.BIN
```

### 5.2 加入檔案

**現成的 `.BIN` 檔**——用 **Add image / asset...** 選取，或用 **Add assets_out**
加入整組。

**圖片**——用 **Add image / asset...** 選取圖片，工具會自動轉檔：

- **由檔名決定目標。** `HOME.png` 對應 `HOME.BIN`，`WF4CAP.png` 對應
  `WF4CAP.BIN`（不分大小寫；`HOME.BIN.png` 也可以）。
- **其他檔名會跳出對話框** *Which asset should this image replace?*（這張圖要
  取代哪個資產？）。選好目標後按 **OK**（或雙擊、按 Enter）。按
  **Skip this one**（或 Esc）則略過這張圖。
- **尺寸**：圖片尺寸與目標不符時，會以 LANCZOS 自動縮放。訊息記錄會顯示
  `converted my_photo.png -> HOME.BIN`。
- **格式**：由目標決定（RGB565 或 RGBA8888），不需要自己設定。
- **錶盤置中**：`WATCHBG.BIN`、`WF2BG.BIN`、`WF3BG.BIN` 會自動把圓形錶盤的圓心
  移到圖片正中央，讓指針交會在錶心。`WF4BG.BIN` **不會**自動置中，請先用
  `tools/face4_prep.py` 處理。

轉出的檔案寫在 `tools/assets_out/<目標>.BIN`，並以這個路徑加入清單。同一個目標
再加入第二張圖時，會更新原本那一列，不會重複出現。

### 5.3 檢查清單

每一列顯示檔名與大小。結尾出現 **`<- size mismatch!`** 的列，表示該 `.BIN`
長度與韌體預期不符，上傳時會被拒絕（尺寸規格見第 7 節）。

### 5.4 上傳

- 全部上傳：確認左欄沒有選取任何項目，按 **Upload**。
- 只傳部分檔案：在左欄選取要傳的項目（Ctrl／Shift + 點選），再按 **Upload**。
  訊息記錄會顯示 `Sending only the N selected (clear the selection to send everything).`

每個檔案都是獨立傳送。只換一張圖，就只需要傳那一個檔案，其他檔案不必重傳。

傳送中進度條會逐檔推進，訊息記錄顯示：

```
sending HOME.BIN ...
  OK   [upload] ok HOME.BIN
Done: 1 / 1 succeeded.
Reboot (or switch to the map page) to see the new assets.
```

速度受 RTT 限制，每 2 MB 約十幾秒；整組上傳約需數十秒。傳送期間 GUI 不會回應
點擊，屬正常現象。

### 5.5 確認結果

上傳成功後右欄會自動重新讀取，顯示的大小就是板子的實際狀態。也可以隨時按
**Read eMMC contents** 手動更新。

### 5.6 在螢幕上看到新圖

| 資產 | 韌體何時讀取 | 做法 |
|---|---|---|
| 首頁圖片、所有錶盤檔案 | 開機時讀一次 | 重置板子。 |
| 地圖圖磚 | 地圖頁需要該圖磚時 | 原本缺少的圖磚，進入地圖頁就會出現。上傳前已顯示過的圖磚可能還在快取中，請重置板子。 |

不確定時，重置板子即可。

---

## 6. 「On the eMMC」欄的標記

| 標記 | 意義 |
|---|---|
| （無標記） | 目前使用中的資產，大小正確。 |
| `<- size mismatch!` | 已知資產但大小錯誤。韌體開機時會印 `short read`，且**不會畫出這張圖**。請重新上傳正確檔案。 |
| `(kept, not loaded by the firmware)` | `WF2*.BIN`（「aurora」錶盤）。它的位置已讓給 Ambiq 錶盤，檔案保留是為了日後可切回。不影響運作。 |
| `(not an asset)` | 韌體不使用的檔案，例如殘留的 `~UPLOAD.TMP`。可用 `--delete` 刪除（見第 9 節）。 |
| `(empty)` | eMMC 上沒有任何檔案。請用 **Add assets_out** 上傳整組。 |
| `read failed` | 韌體沒有回應。見第 8 節。 |

---

## 7. 資產規格表

資產是**沒有檔頭**的原始像素資料，尺寸與格式必須與 `src/asset_store.h` 中的
`ASSET_*` 完全一致。執行 `python tools/png_to_bin.py --targets` 可印出這份清單。

| 目標檔名 | 尺寸（px） | 格式 | 位元組 | 用途 |
|---|---|---|---:|---|
| `HOME.BIN` | 454×454 | RGB565 | 412,232 | 首頁圖片 |
| `WATCHBG.BIN` | 454×454 | RGB565 | 412,232 | 「chrono」錶盤——錶面（自動置中） |
| `HANDHR.BIN` | 15×90 | RGBA8888 | 5,400 | 「chrono」錶盤——時針 |
| `HANDMIN.BIN` | 15×150 | RGBA8888 | 9,000 | 「chrono」錶盤——分針 |
| `HANDSEC.BIN` | 13×179 | RGBA8888 | 9,308 | 「chrono」錶盤——秒針 |
| `WF4BG.BIN` | 454×454 | RGB565 | 412,232 | 「ambiq」錶盤——錶面（用 `face4_prep.py` 處理） |
| `WF4HR.BIN` | 30×130 | RGBA8888 | 15,600 | 「ambiq」錶盤——時針 |
| `WF4MIN.BIN` | 30×193 | RGBA8888 | 23,160 | 「ambiq」錶盤——分針 |
| `WF4SEC.BIN` | 18×126 | RGBA8888 | 9,072 | 「ambiq」錶盤——秒針 |
| `WF4CAP.BIN` | 46×46 | RGBA8888 | 8,464 | 「ambiq」錶盤——中心蓋 |
| `WF3BG.BIN` | 454×454 | RGB565 | 412,232 | 「numbers」錶盤——錶面（自動置中） |
| `WF3HR.BIN` | 23×107 | RGBA8888 | 9,844 | 「numbers」錶盤——時針 |
| `WF3MIN.BIN` | 33×178 | RGBA8888 | 23,496 | 「numbers」錶盤——分針 |
| `WF3SEC.BIN` | 18×213 | RGBA8888 | 15,336 | 「numbers」錶盤——秒針 |
| `MAPTILE0.BIN` – `MAPTILE2.BIN` | 468×468 | RGB565 | 每個 438,048 | 地圖頁圖磚 |
| `WF2BG.BIN` | 390×390 | RGB565 | 304,200 | 「aurora」錶盤（保留，未載入） |
| `WF2HR.BIN` / `WF2MIN.BIN` / `WF2SEC.BIN` | 22×104 / 28×153 / 18×219 | RGBA8888 | 9,152 / 17,136 / 15,768 | 「aurora」錶盤（保留，未載入） |

**美術素材注意事項**

- **指針**：請用透明背景（RGBA）。每根指針的旋轉軸心固定寫在韌體裡
  （`src/render_task.c` 的 `PIVOT_*`），所以新指針必須**尺寸相同**，且軸心落在
  與原圖**相同的像素位置**。尺寸不同的指針經自動縮放後，軸心會跑掉。
- **錶面**：`WF3BG.BIN` 請用 `src/generated/face3_dial_454_centered.png`；
  `WF4BG.BIN` 請用 `src/generated/face4_dial_454_centered.png`。旁邊其他的 PNG
  是已畫上指針的設計預覽圖，或圓心偏移的原圖——工具不會報錯，但上板後畫面會錯。
- **地圖圖磚**：替換圖磚只會改變底圖。導航路線寫在 `src/map_route.h`，不會跟著改。

---

## 8. 疑難排解

| 訊息記錄中的訊息 | 原因 | 處理方式 |
|---|---|---|
| `cannot connect to 127.0.0.1:19021 -- ...` | 沒有執行中的 `JLinkGDBServer`，或連接埠不同。 | 啟動 RTT（第 3 節）；確認連接埠。 |
| `failed to read the eMMC contents: no reply from the target ...` | 韌體沒在跑、編譯時沒開 `WATCHFACE_ASSET_UPLOAD=1`，或 RTT 還沒接上。 | 檢查開機 log 是否有 `[upload] task started`；再按一次 **Read eMMC contents**。 |
| `failed to read the eMMC contents: the target reported a read failure` | eMMC 未掛載。 | 檢查開機 log 的 asset store 狀態；重置板子。 |
| `FAIL size mismatch: N, expected M ...` | `.BIN` 大小與目標不符。 | 改加入原始圖片（會自動縮放），或用 `png_to_bin.py` 重新產生 `.BIN`。 |
| `FAIL file name longer than 12 characters` | 檔名必須是 8.3 格式。 | 使用第 7 節的目標檔名。 |
| `FAIL [upload] abort <name>: timeout` | 板子端傳輸停滯。 | 重新上傳該檔案。 |
| `FAIL the target stopped responding at X/Y bytes` | 板子在傳輸中途停止回應。 | 確認板子仍在運作；重新連線後再上傳。 |
| `FAIL [upload] err <name>: sum ... != ...` | 資料在傳輸中損毀。 | 重新上傳，新檔會覆蓋舊檔。 |
| `FAIL no reply from the firmware ...` | 同上方「no reply」的情況。 | 同上。 |
| `Adding an image directly needs Pillow` | 未安裝 Pillow。 | `pip install pillow`，或先用 `png_to_bin.py` 轉檔再加入 `.BIN`。 |
| `skipped <file> (no target chosen)` | 在選擇目標的對話框按了 **Skip this one**。 | 重新加入該圖片並選擇目標。 |
| 上傳成功但螢幕仍是舊圖 | 資產在開機時讀取。 | 重置板子。 |
| 上傳成功但該區塊一片黑 | 大小不符，或缺檔。 | 按 **Read eMMC contents**，找 `<- size mismatch!` 與 `missing assets` 那一行。 |
| 在 `build_gui.py` 按了 **Stop RTT** 之後出錯或沒反應 | 停止 RTT 會關掉 GDB server，本工具的連線也跟著斷了。 | 重新 **Start RTT**，再到本工具按 **Disconnect**、**Connect**。 |

上傳中斷或失敗都是安全的：韌體先寫入暫存檔（`~UPLOAD.TMP`），整個檔案收完後
才取代正式檔案，所以舊版本會留在 eMMC 上。

---

## 9. 命令列模式

同一支工具也能不開 GUI 使用，適合寫腳本，或用來刪除檔案（GUI 沒有刪除按鈕）。

```powershell
python tools\asset_upload.py                          # 開啟 GUI
python tools\asset_upload.py --list                   # 列出 eMMC 內容（也可用 -l）
python tools\asset_upload.py tools\assets_out\HOME.BIN # 上傳 .BIN 檔
python tools\asset_upload.py HOME.png WF4CAP.png      # 圖片先轉檔再上傳
python tools\asset_upload.py --delete ~UPLOAD.TMP     # 從 eMMC 刪除檔案
```

- 命令列上的圖片必須以目標命名（如 `HOME.png`）。命令列無法跳出對話框詢問，
  其他檔名會直接報錯並列出可用的目標名稱。
- 全部成功時結束碼為 `0`，任一檔案失敗則為 `1`。

只轉檔、不上傳：

```powershell
python tools\png_to_bin.py my_photo.png HOME.BIN      # 單張圖 -> tools\assets_out\HOME.BIN
python tools\png_to_bin.py --dir my_images\           # 整個資料夾，依檔名對應
python tools\png_to_bin.py --targets                  # 列出所有目標
```

---

## 10. 注意事項

- **圖片轉檔會覆寫 `tools/assets_out/<目標>.BIN`**，而那是納入 git 的現成資產組。
  要還原原檔：`git checkout -- tools/assets_out/`。
- **安全性**：`WATCHFACE_ASSET_UPLOAD` 讓任何能接上 SWD 的人都能改寫 eMMC。
  量產韌體請在 `gcc/Makefile` 將它設為 `0`。
- **全新的板子**：用 **Add assets_out** → **Upload** 上傳整組，然後重置。
- 傳輸原理（協定、流量控制）：見 `src/asset_upload.h` 與 `tools/asset_upload.py`
  檔頭說明。
