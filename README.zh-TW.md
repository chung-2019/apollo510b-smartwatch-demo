# nemagfx_watchface — Apollo510B EVB

<p align="center">
  <a href="https://youtu.be/N_bFMPd-4I8"><img src="https://i.ytimg.com/vi/N_bFMPd-4I8/maxresdefault.jpg" alt="在 YouTube 觀看 Apollo510B Smart Watch Demo" width="73%"></a>
  <a href="https://www.youtube.com/shorts/pJUpLRMOmOk"><img src="https://i.ytimg.com/vi/pJUpLRMOmOk/oardefault.jpg" alt="在 YouTube Shorts 觀看 Linux BLE Am62P connect to Apollo 510B" width="23%"></a>
</p>

在 YouTube 觀看：[Apollo510B Smart Watch Demo](https://youtu.be/N_bFMPd-4I8) ·
[Linux BLE Am62P connect to Apollo 510B](https://www.youtube.com/shorts/pJUpLRMOmOk)（Shorts）。
兩支影片都放在 YouTube 上，不包含在這個原始碼 repo 裡。

這是一套在 apollo510b_evb 上執行的五頁式 FreeRTOS 智慧手錶使用者介面，以 NemaGFX / NemaVG
為基礎，整合 BLE（Cordio + EM9305）、TMP119 溫度感測器、CHSC5816 觸控面板、
外部 PSRAM，以及以 eMMC 儲存的素材庫。

本專案從 AmbiqSuite 的 `nemagfx_watchface` 範例發展為完整應用程式。原始碼中的註解記錄了
每項決策背後的**原因**，也包含數個實際花費許多時間除錯才釐清的 SDK 行為，因此這份程式碼
除了可供編譯，也希望能讓讀者理解其中的設計與取捨。

---

## 1. 硬體

| 功能區塊 | 元件 | 介面 |
|---|---|---|
| MCU | Apollo510B（AP510NFA-CBR） | — |
| 顯示器 | CO5300 AMOLED，468x468 圓形螢幕 | MIPI DSI |
| 觸控 | CHSC5816 | IOM2（SCL GP25、SDA GP26、INT GP98、RST GP99） |
| 溫度感測器 | TI TMP119（mikroBUS） | IOM1（SCL GP8、SDA GP9） |
| BLE | EM9305 | IOM6、INT GPIO117 |
| PSRAM | APS51216BA，64 MB，x16 hex DDR | MSPI0，XIP 位址為 0x60000000 |
| 素材儲存裝置 | IS21EF08G eMMC，8 GB，FAT | SDIO0 |
| 按鈕 | BTN0 / GP46（SW1）、BTN1 / GP29（SW3） | GPIO |
| LED | LED2/GP11、LED3/GP19（BLE）、LED4/GP83 | GPIO，開汲極，低電位有效 |

> **擴充板腳位配置注意事項。** Apollo510B 的觸控 INT/RST 為 GP98/GP99。
> AP510DISP 擴充板資料表將它們標示為 77/78，這是 Apollo510（非 B 版）的腳位編號，
> 不適用於此處。

---

## 2. 快速開始

### 2.1 先備工具

| 工具 | 版本 | 用途或原因 |
|---|---|---|
| Arm GNU Toolchain | 任何支援 `-mcpu=cortex-m55` 的版本 | Apollo510 使用 Cortex-M55 |
| MSYS2 `make` | — | Makefile 使用 GNU make 與 bash 語法 |
| SEGGER J-Link | **V8.40 或更新版本** | 舊版沒有 AP510NFA-CBR 的 MRAM 燒錄載入程式 |

`gcc/check_toolchain.sh` 會檢查上述三項工具，包括已安裝的 J-Link 是否確實支援
`AP510NFA-CBR`：

```bash
cd gcc
bash check_toolchain.sh
```

### 2.2 放置專案目錄

本範例需要一份 AmbiqSuite 5.2.0 原始碼，因為建置時會使用 SDK 的原始碼：

```
AmbiqSuite_5.2.0/
  boards/apollo510b_evb/bsp/
  boards/apollo510b_evb/examples/graphics/nemagfx_watchface/   <- 本儲存庫
  third_party/ThinkSi/NemaGFX_SDK/
  third_party/cordio/
  third_party/FatFs/
  mcu/apollo510/
```

Makefile **不會寫死**上述目錄深度。它會向上搜尋 SDK 根目錄（`SDK_ROOT`），並據此取得 BSP
路徑，因此即使專案再多放一至兩層目錄，例如將儲存庫複製到另一個子目錄中，也能建置。
必要時可明確指定其他位置：

```bash
make SDK_ROOT=/c/AmbiqSuite_5.2.0
```

若搜尋失敗，Makefile 會停止並提示應設定哪個變數，而不會等到後續才出現
`No rule to make target '.../am_devices_chsc5816_ap5.o'` 之類的錯誤。

### 2.3 建置與燒錄

```bash
cd gcc
make -j8                 # -> bin/nemagfx_watchface.bin
make flash               # 透過 J-Link 載入至 0x00410000
```

專案也提供 Tkinter 圖形介面 `build_gui.py`，整合建置、重新建置、清理與燒錄功能，
可自動偵測工具鏈路徑，並內建 RTT 日誌檢視器：

```bash
python build_gui.py      # 或按兩下 launch_build_gui.bat
```

> **關於 `TMP`。** GCC 會將中間檔案寫入 `$TMP`，但 MSYS2 make 經常完全沒有傳遞 `TMP`。
> 此時 GCC 會改用 `C:\Windows\`，並因為
> `Cannot create temporary file in C:\Windows\: Permission denied` 而失敗。
> 這個訊息看起來像編譯錯誤，實際上卻是環境問題。現在 Makefile 會依序改用 `$TEMP`，
> 再改用 `gcc/` 目錄本身，因此不需要額外設定；若自行設定 `TMP`，仍會優先採用。
> 請注意，GCC 是原生 Windows 執行檔，因此不能將 `/tmp` 這類 MSYS 格式路徑作為其值。

### 2.4 日誌

日誌透過 SEGGER RTT（`AM_USE_RTT_LOG`）輸出，而非 ITM/SWO。
`build_gui.py` 的 RTT 檢視器會啟動 `JLinkGDBServerCL` 並讀取通道 0。

由於素材上傳也共用 RTT 緩衝區，兩個緩衝區都已從預設大小加大為
`BUFFER_SIZE_UP/DOWN=4096`；詳見第 5 節。

---

## 3. 儲存庫目錄結構

```
src/                     應用程式原始碼
  nemagfx_watchface.c    main()：時脈、顯示器、eMMC、PSRAM、排程器
  rtos.c                 建立任務、優先權、idle hook、開機資訊
  render_task.c          所有繪圖：5 個頁面、覆蓋面板、錶面選擇器、
                         頁面總覽（最大的檔案，含大量註解）
  display_task.c         DC/DSI 喚醒、畫面傳輸、套用亮度
  touch.c / touch_task.c CHSC5816 驅動程式封裝與手勢分類
  map_task.c / .h        地圖圖磚快取（eMMC -> PSRAM）與導航模擬
  sensor_task.c          TMP119 取樣並存入快取
  asset_store.c          eMMC 上的 FAT：掛載、讀取、寫入、列出檔案、串流寫入
  asset_upload.c         接收主機透過 RTT 傳入的素材
  asset_provision.c      可選的首次開機素材佈建，來源為內建陣列
  radio_task.c, hts_main.c, ble_*.c   Cordio BLE：以 TMP119 提供 HTS、LESC 配對
  ble_diag.c / .h        透過 GNU ld --wrap 建立 HCI 事件環形緩衝區，處理 INT 卡住
  psram.c, backlight.c, led_task.c, tmp119.c, rtt_log.c
  generated/             建置實際需要的素材標頭檔
  font/                  供 NemaVG 使用的 DejaVuSans 向量字型

gcc/                     Makefile、連結器腳本、啟動程式、J-Link 腳本
tools/                   主機端 Python 工具：素材轉換與上傳
  png_to_bin.py          PNG -> .BIN（所有轉換流程的共同輸出步驟）
  asset_upload.py        透過 RTT 將 .BIN 傳入 eMMC（提供 GUI 與 CLI）
  asset_export.py        將韌體的 C 陣列匯出為 .BIN
  face2/3/4_prep.py      各款錶面的素材前處理
  map_tile_gen.py        地圖 PNG -> RGB565 C 陣列
  assets_out/            五個頁面所需的現成 .BIN 檔案
  nemagfx_ui_designer/   可輸出 NemaGFX/NemaVG C 程式碼的 PySide6 UI 編輯器
build_gui.py             建置／燒錄／RTT 圖形介面
```

---

## 4. 執行架構

### 4.1 任務

| 任務 | 優先權 | 堆疊（word） | 職責 |
|---|---|---|---|
| RadioTask | HIGHEST（15） | 2048 | Cordio WSF 事件派送 |
| TouchTask | HIGHEST（15） | 512 | GP98 中斷 -> I2C 讀取 -> 手勢判定 |
| RenderTask | HIGH（14） | 1024 | 所有繪圖、UI 狀態機 |
| DisplayTask | MIDDLE（13） | 1024 | DC/DSI 喚醒、畫面傳輸、DCS 亮度設定 |
| SensorTask | LOW（12） | 512 | 每 1 秒取樣 TMP119、讀出並輸出診斷紀錄 |
| MapTask | LOW（12） | 512 | 圖磚快取與導航模擬 |
| UploadTask | LOWEST（11） | 768 | 透過 RTT 接收素材 |
| LedTask | LOWEST（11） | 512 | 按鈕去彈跳 -> GPIO |

TouchTask 刻意設為與 RadioTask 相同的優先權，因為它必須能搶占 RenderTask；後者在
`draw_*()` 與 `nema_cl_wait()` 內可能連續占用 CPU 數毫秒。TouchTask 幾乎所有時間
都在阻塞等待任務通知，因此不會對 BLE 造成負擔。設計理由記錄於 `touch_task.h` 的檔頭註解。

`configTOTAL_HEAP_SIZE` 為 48 KB，`configTIMER_QUEUE_LENGTH` 為 16；兩者都比 SDK
範例的設定值大，調整原因記錄於 `FreeRTOSConfig.h`。

### 4.2 記憶體

```
MRAM     程式與 const 資料，燒錄位址為 0x00410000
TCM      FreeRTOS heap、任務堆疊
SSRAM    VMEM_SIZE = 0x2A0000（2.75 MB）的 NemaGFX 記憶體池：
           NEMA_MEM_POOL_FB       畫面緩衝區與頁面總覽縮圖
           NEMA_MEM_POOL_ASSETS   從 eMMC 載入的紋理
PSRAM    地圖圖磚，XIP 位址為 0x60000000，供 GPU 直接取樣
eMMC     FAT：每個素材各存成一個檔案
```

此平台的 GPU **無法對 MRAM 進行取樣**，因此每個紋理都必須先複製到 SSRAM
（地圖圖磚則存入 PSRAM，GPU *可以*透過 MSPI XIP 對它取樣）。
直接將 const 陣列位址綁定給 GPU，在 Apollo4P 上可以運作，但在這裡會產生閃爍的紅色畫面。

`RenderTask()` 中的記憶體配置順序是刻意安排的，先前曾因順序問題造成兩次硬體執行故障：
畫面緩衝區與縮圖緩衝區都必須**先於**非必要素材完成配置，如此一來，記憶體池不足時，
最多只會讓錶面變黑，而不會讓 GPU 綁定至 NULL 並造成開發板停止回應。
該呼叫位置的註解記錄了這兩次事件。

### 4.3 頁面

| 頁面 | 內容 |
|---|---|
| HOME | EdgePilot 圖像，搭配地平線上的加法混色掃光效果 |
| WATCH | 指針式錶面；向下滑動可開啟錶面選擇器（3 款錶面） |
| TEMP | TMP119 讀值、環形儀表、BLE 狀態（5 種顯示狀態，包含 LESC 配對） |
| TREND | 時鐘、日期，以及帶有剪切投影效果的 60 秒溫度曲線 |
| MAP | 使用 PSRAM 圖磚顯示台北 101 -> 台北車站導航 |

覆蓋面板包含：亮度面板（所有頁面皆可從底部邊緣向上滑動開啟）、控制中心
（僅首頁可從頂部邊緣向下滑動開啟）、快速頁面選單（僅首頁可向右滑動開啟），
以及頁面總覽（所有頁面皆可按 BTN0 開啟）。

---

## 5. 素材工作流程

素材以不含標頭的原始像素資料儲存在 eMMC，因此尺寸與格式必須完全符合
`src/asset_store.h` 中的 `ASSET_*` 定義。若不相符，開機日誌會顯示 `short read`，
且該圖片完全不會繪出。

整個流程的任何步驟都不需要重新建置韌體：

```
PNG --png_to_bin.py--> .BIN --asset_upload.py--> eMMC --> GPU
```

```bash
# 轉換整個資料夾的 PNG（依檔名對應目標，必要時縮放）
python tools/png_to_bin.py --dir my_images/
python tools/png_to_bin.py --targets        # 列出所有目標及其規格

# 傳送至開發板（必須先啟動 JLinkGDBServer；
# build_gui.py 的 RTT 視窗也會啟動它）
python tools/asset_upload.py --list              # 列出 eMMC 目前的檔案
python tools/asset_upload.py tools/assets_out/*  # 傳送
python tools/asset_upload.py HOME.png            # 先轉換，再傳送
python tools/asset_upload.py                     # 圖形介面
```

`tools/assets_out/` 已包含五個頁面所需的完整素材。

### 流量控制是其中的關鍵

RTT 下行緩衝區大小為 4 KB，**填滿時 J-Link 會丟棄多出的資料，而不會讓主機暫停傳送**。
因此韌體每接收 1 KB 就會輸出一行 `[upload] ack <n>`，而主機會將尚未確認接收的資料量
控制在緩衝區容量以下；若沒有這個機制，所有超過 4 KB 的檔案都會以
`abort <name>: timeout` 失敗。確認訊息經由上行緩衝區傳送，而該緩衝區採用
`NO_BLOCK_SKIP`，滿載時會丟棄訊息行，因此韌體也會在傳輸停滯 250 ms 後重新回報進度。
兩端的處理方式分別記錄於 `src/asset_upload.h` 與 `tools/asset_upload.py`。

### 建置開關（`gcc/Makefile`）

| 定義 | 預設值 | 說明 |
|---|---|---|
| `WATCHFACE_ASSET_PROVISION` | 0 | 設為 1 時，也會將素材編入 MRAM（增加 2.16 MB），並在開機時寫入缺少的檔案。可在空白開發板上使用一次。 |
| `WATCHFACE_ASSET_FORCE_WRITE` | 0 | 設為 1 時，無條件覆寫既有檔案；替換相同尺寸的圖像時需要使用。完成後請設回 0。 |
| `WATCHFACE_ASSET_UPLOAD` | 1 | RTT 接收任務。正式產品韌體可將其關閉，因為此功能讓任何可存取 SWD 的人都能改寫 eMMC。 |
| `VMEM_SIZE` | 0x2A0000 | 位於 SHARED_SRAM 的 NemaGFX 記憶體池大小 |
| `WSF_TRACE_ENABLED` | 關閉 | **請維持關閉**，原因見下文 |
| `SDK_ROOT` | 自動搜尋 | AmbiqSuite 根目錄；見 2.2 節 |
| `BSP_ROOT` | `$(SDK_ROOT)/boards/apollo510b_evb/bsp` | 開發板 BSP |

---

## 6. 值得留意的實測發現

以下列出實測行為與文件描述或合理預期不同的地方，每一項都已在相關呼叫位置留下完整註解。

**`am_util_stdio_snprintf()` 不會寫入 NUL 結尾字元。** 它複製字元後就返回，並非標準的
`snprintf`。這曾造成兩個問題：HUD 距離顯示成 `4.9 kmC -`，以及地圖圖磚檔名變成
`MAPTILE0.BIN` 後面再接上堆疊中的雜亂資料，導致 `f_open()` 回傳 `FR_NO_FILE`，
使地圖「有時」要過數秒才出現。地圖頁面相關的每個呼叫位置都會先以 memset 清空緩衝區。
相關檔案：`src/map_task.c`、`src/render_task.c`。

**`WSF_TRACE_ENABLED` 會增加 10 秒開機時間。** 啟用後，DM 重設完成時間會從
t=1062 ms 延後到 t=11067 ms。Cordio 在重設期間輸出大量追蹤訊息，
`am_util_stdio_printf()` 會先格式化每一行再寫出，而 RadioTask 在 printf 期間不會
主動讓出 CPU。相關檔案：`gcc/Makefile`。

**漏掉的 GPIO 邊緣不會再觸發一次。** Apollo5 的 GPIO 中斷只支援邊緣觸發，
沒有電位觸發模式。EM9305 INT（GPIO117，僅上升緣）只要漏掉一次轉換，就得等待
10 秒的 `hciCmdTimeout()` 逾時；這正是「有時開機就開始廣播，有時要等 10 或 20 秒」
這種階梯式延遲的來源。`ble_diag.c` 會在繪圖迴圈中偵測「INT 維持高電位，但中斷計數
不再增加」的狀況，並重新送出事件，將 10 秒延遲縮短為數十毫秒。
TouchTask 中的兩個逾時機制也是基於同樣的考量。
相關檔案：`src/ble_diag.h`、`src/touch_task.h`。

> 完整分析與用來找出問題的 RTT 擷取紀錄，請見：
> **[docs/BLE_HCI_MISSED_INTERRUPT.md](docs/BLE_HCI_MISSED_INTERRUPT.md)**
> （另有[繁體中文版本](docs/BLE_HCI_MISSED_INTERRUPT.zh-TW.md)）

**不修改 SDK 即可加入 HCI 量測。** `[bledbg]` 環形緩衝區透過 GNU ld 的 `--wrap`
攔截五個 Cordio 符號（見 `gcc/Makefile` 的 LFLAGS），因此不需要更動 `third_party/`
下的任何一行程式碼。攔截函式只寫入 RAM，再由 SensorTask 讀出並輸出紀錄，
因為 `am_util_stdio_printf()` 共用一個未加鎖的靜態緩衝區；若在 RadioTask 中格式化輸出，
既會破壞其他任務的輸出，也會拉長原本想要量測的執行時序。
相關檔案：`src/ble_diag.c`。

**eMMC 匯流排設定採逐項嘗試，而非直接假設可用。** SDK 的預設設定
（1.8 V / SDR50 / 48 MHz / x8）會讓這塊開發板上的 `am_hal_card_cfg_set()` 失敗，
但已確認 eMMC 本身正常。`AM_HAL_CARD_DEBUG` 原本能提供原因，然而 HAL 是以
預先建置的 `libam_hal.a` 提供，當中並未編入這些訊息。因此韌體會依設定表逐項嘗試，
採用第一個成功的組合；已驗證可用的是 `1.8V legacy 24M x8`。此外，VCCQ/VCC
經由 GPIO220/221 控制的負載開關供電，而 RSTn 的上拉電阻（R46）並未裝設，
因此電源開關與重設訊號都必須由韌體驅動。相關檔案：`src/asset_store.c`。

**`nema_texture_size()` 不等於 `w * h * bpp`。** 使用後者估算會低估所需空間，
而 GPU 記憶體池又是 `NOLOAD` 陣列，因此設定值太小不會產生任何連結錯誤，
只會在執行時出錯。這也是第四款錶面會取代第二款，而非直接新增的原因。
相關設定：`gcc/Makefile` 的 VMEM_SIZE。

**`nema_vg_string_get_bbox()` 會將結果限制在 `max_w` 參數範圍內。** 因此，
若以元件本身的方框寬度進行量測，就無法判斷「這一行太寬」，自動縮小功能也會在沒有
任何錯誤提示的情況下始終不執行。UI Designer 曾遇到這個問題，對應的回歸測試位於
`tools/nemagfx_ui_designer/tests/`。

**NemaVG 將字型大小視為點（point）單位。** 三次獨立的硬體量測都得到相同分母：
`1536 = units_per_em * 3/4`，對應 96 DPI 下點與像素之間的 72/96 關係。
這與 `nema_vg_get_ascender_pt()` 回傳點單位的行為一致，但
`nema_vg_set_font_size()` 並未明確說明這一點。推導過程記錄於
`tools/nemagfx_ui_designer/font_metrics.py`；用來取得這些數據的裝置端量測函式，
則是 `render_task.c` 中的 `design_font_probe()`。

**兩個驅動程式共用一條 IRQ 線。** GP98（觸控）與 GPIO117（EM9305）都對應到
`GPIO0_607F_IRQn`，而 GP46（BTN0）與顯示器 TE 共用 `GPIO0_203F_IRQn`。
對任一共用 IRQ 再定義第二個 ISR，都會造成重複定義的連結錯誤，因此本專案不另外定義
這兩個 ISR，而是依靠 `am_hal_gpio_interrupt_service()` 按腳位派送中斷。
相關檔案：`src/radio_task.c`、`src/led_task.c`。

**`am_bsp_psLEDs[]` 的極性設定缺少開汲極旗標。** 它包含 `AM_DEVICES_LED_ON_LOW`，
卻沒有 `AM_DEVICES_LED_POL_OPEN_DRAIN`，導致 `am_devices_led_init()` 進入三態分支，
覆寫 BSP 本身的開汲極腳位設定。因此本專案改為直接套用 `g_AM_BSP_GPIO_LED*`。
相關檔案：`src/ble_led.c`、`src/led_task.c`。

---

## 7. UI Designer

`tools/nemagfx_ui_designer/` 是以 PySide6 製作的編輯器，可為本專案輸出
NemaGFX / NemaVG C99 程式碼。它提供 454x454 圓形畫布
（手錶實際顯示的圓形範圍，產生程式碼時會自動置中於 468x468 畫面緩衝區），
支援 Label / Image / Button / Rectangle / Circle / Line / Arc / Gauge、
多場景、由按鈕觸發並帶有緩動效果的圖片旋轉，以及互動式預覽。

產生的檔案輸出至 `src/generated/ui_designer/`，並附帶自己的 `ui_designer.mk`，
因此專案 Makefile 只需要加入一行 `include`，新增素材時不必再做其他修改。
應用程式邏輯可透過覆寫弱符號回呼函式接入；每次產生時，輸出檔案都會被完整覆寫。

```bash
pip install PySide6 pillow
python tools/nemagfx_ui_designer/main.py
python -m unittest discover -s tools/nemagfx_ui_designer/tests
```

畫布預覽會讀取硬體所用 NemaVG 字型中的字元前進寬度表（advance table），
讓「這一行是否會被裁切」的判斷與裝置一致，而不受電腦系統字型影響。
詳細資訊請見該目錄內的 `README.md` 與 `CHANGELOG.md`。

---

## 8. 本儲存庫未包含的內容

- `docs/`：程式碼所引用的開發筆記，包含 BLE 啟動調查、eMMC 素材庫、記憶體架構、
  地圖導航頁面與錶面選擇器。註解會以路徑引用這些文件，其中的設計理由已摘要整理至
  程式碼註解中。
- 地圖圖磚與錶面的原始 PNG。`tools/assets_out/` 已提供五個頁面轉換完成的 `.BIN`
  檔案，這些才是開發板實際需要的資料。專案保留 `map_tile_gen.py` 與 `face*_prep.py`，
  以便檢視轉換流程，但未附上它們使用的輸入檔案。
- `src/generated/` 下的實驗性素材版本。僅複製了建置過程實際透過 `#include` 引用的標頭檔。

---

## 9. 已知待改善事項

- 時間是以「開機後經過的秒數」加上 `WATCH_START_HOUR` 計算；這塊開發板沒有 RTC。
  趨勢頁面的日期也採用相同方式，且未處理閏年。兩者都已標記，待接上 RTC 後移除。
- 所選錶面尚未持久化儲存；eMMC 目前只存放素材，沒有設定檔。
- BLE 配對固定使用 `TMPS_PAIRING_MODE = 2`（綁定 + LE Secure Connections）。
  模式 1 與 0 已實作，並在 `hts_main.c` 中說明，但兩者都無法完成 indication 訂閱，
  原因尚未釐清。
- 目前共有 3 個圖磚，`MAP_CACHE_SLOTS` 的計算結果也為 3，因此所有圖磚都會常駐，
  LRU 淘汰機制不會執行。這個機制仍保留，以供日後擴充地圖使用。
