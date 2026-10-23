# 漏掉一次 GPIO117 上升邊緣，HCI 初始化就卡 10 秒

**平台** apollo510b_evb（AP510BFA-CBR，EM9305 封裝內）· AmbiqSuite 5.2.0
· Cordio BLE host · FreeRTOS
**狀態** 卡住的原因已定案並在應用層補上恢復機制；邊緣為何漏掉仍未定論。
**範圍** 全部改動只在本範例的 `src/` 與 `gcc/Makefile`。
**`third_party/` 與 `devices/` 一個字都沒改。**

English version: [BLE_HCI_MISSED_INTERRUPT.md](BLE_HCI_MISSED_INTERRUPT.md)

---

## 1. 摘要

這片板子從開機到開始廣播的時間不固定：有時約 1.3 秒，有時 11.3 秒，有時
21.3 秒——是階梯狀的，每階正好 10 秒。

每一階就是一次 `HCI_CMD_TIMEOUT`（10 秒，`hci_cmd.c`）。逾時的原因是 EM9305
已經把 Command Complete 準備好、並把 `SPI_RDY`（GPIO117）拉高，而主機漏掉了那個
**上升邊緣**。GPIO117 設定為 `AM_HAL_GPIO_PIN_INTDIR_LO2HI`，而 Apollo5 的 GPIO
**沒有準位觸發模式**，所以那個轉態一旦漏掉，硬體不會再給第二次機會。driver 本身
也沒有任何會自己去讀腳位的背景輪詢，於是整條 HCI 命令序列停住，直到 Cordio 的
逾時把控制器整個關掉重開、重跑一次初始化。

我們的做法是從算繪迴圈偵測這個卡住的狀態，補送一次那個漏掉的中斷原本會送出的
WSF event。最壞恢復時間從 10 秒降到 20~40 ms，而且不必重啟控制器——卡住的那個
封包會被正常讀走，序列從斷點繼續。

**希望 Ambiq 考慮的一點：** `hci_drv_em9305.c` 已有「讀滿 4 包後補送事件」的局部
補救，註解也提到有封包等待卻沒有中斷的情況（見 §3.4）。但它沒有獨立於 handler
的持續檢查：若漏掉邊緣時 handler 沒有被排程，也沒有待處理的 transfer 事件，
仍可能只能等 10 秒逾時，並丟掉所有 HCI/DM 狀態。偵測的成本很低（一次 GPIO
讀取加兩次比較），放在 driver 裡會比讓每個應用自己補更合適。

---

## 2. 定案的那段 log

這是關鍵證據，透過 RTT 擷取（診斷機制見 §6）：

```
[irqdiag] em9305 irq=13 pin=1 nvic=1
[irqdiag] em9305 irq=13 pin=1 nvic=1
[irqdiag] em9305 irq=13 pin=1 nvic=1
[bledbg] t=11048 ms  *** HCI CMD TIMEOUT op=0x0C01 irq=13 pin=1 ***
```

`[irqdiag]` 由 SensorTask 每秒印一行（`src/sensor_task.c:324`）。連續三秒，
每個欄位都不動：

| 欄位 | 值 | 意義 |
|---|---|---|
| `irq` | `13`，完全不動 | `hci_drv_em9305.c` 的 `g_ui32InterruptsSeen`。它是**在 `HciDrvIntService()` 內部**遞增的，所以凍結代表 ISR 根本沒被進入，而不是進去了卻沒做事。 |
| `pin` | `1` | `am_hal_gpio_input_read(AM_BSP_GPIO_EM9305_INT)`。高電位表示 EM9305 有資料等主機讀，而且還在等。 |
| `nvic` | `1` | `GPIO0_607F_IRQn` 在 NVIC 仍是啟用的。中斷沒有被遮蔽掉。 |

三個欄位合起來讀：**控制器正在說「我有資料」、NVIC 是開的、而 ISR 沒有在跑。**
這把問題範圍縮到「腳位轉態 → `HciDrvIntService()`」這一段，同時排除了 WSF 事件
分派與它上面的 host stack。

第四行來自我們掛在 `hciCmdTimeout()` 上的 `--wrap`：

| 欄位 | 值 | 意義 |
|---|---|---|
| `t` | `11048 ms` | 開機後經過的時間。那個命令在 t≈1048 ms 送出，所以這裡正好是 10 秒後。 |
| `op` | `0x0C01` | `HCI_Set_Event_Mask`——它的 Command Complete 正卡在 EM9305 裡。 |
| `irq` / `pin` | `13` / `1` | 逾時當下仍然沒變，證明這個狀態整整維持了 10 秒。 |

這和實際觀察到的開機時間直接對應：漏 0 次 → 1.3 秒，漏 1 次 → 11.3 秒，
漏 2 次 → 21.3 秒。

> **關於那 1.3 秒的底線。** 它不是缺陷，判讀時不要和這個問題混在一起。其中約
> 1.04 秒是 EM9305 收到 `HCI_Reset` 之後自己的復位時間（`tx cmd op=0x0C03`
> 在 t=2 ms，`cmd cmpl` 在 t=1040 ms）。

---

## 3. 為什麼漏一次就回不來

### 3.1 這支腳是邊緣觸發，而 Apollo5 沒有準位模式

GPIO117 由 `am_devices_em9305_config_pins()`（`am_devices_em9305.c:280`）用通用的
`am_hal_gpio_pincfg_input` 設定：

| 欄位 | 值 |
|---|---|
| `eGPInput` | `INPUT_ENABLE` |
| `eGPOutCfg` | `OUTCFG_DISABLE` |
| **`eIntDir`** | **`AM_HAL_GPIO_PIN_INTDIR_LO2HI`** ← 只認上升邊緣 |
| `ePullup` | `PULLUP_NONE` |

`AM_HAL_GPIO_PIN_INTDIR_*` 只有 `NONE` / `HI2LO` / `LO2HI` / `BOTH`
（`am_hal_gpio.h:320-323`），沒有準位觸發的選項，所以「這支腳現在是高的」本身
不會產生中斷。改成 `BOTH` 也無解——那只是多收下降邊緣，對一條已經是高電位、
而且會繼續維持高電位的線完全沒有幫助。

> 另外一件值得單獨提出來的事：BSP 確實有定義 `g_AM_BSP_GPIO_EM9305_INT`
> （`am_bsp_pins.c:4292`），而它寫的是 `AM_HAL_GPIO_PIN_INTDIR_HI2LO`，
> 極性**剛好相反**。那個結構並沒有被 `am_devices_em9305_config_pins()` 使用。
> 它是指向錯誤方向的殘留設定，我們在確認真實極性時被它耽誤過。

### 3.2 driver 自己的 GPIO 輪詢沒有機會執行

`HciDrvHandler()` 確實會讀腳位：

```c
/* hci_drv_em9305.c:852 */
if ( am_hal_gpio_input_read(GPIO_INT) )
{
    am_devices_em9305_block_read(...);
    /* line 884：讀完後等它回到低電位，避免重複讀到同一個封包 */
}
```

但這不是背景輪詢——它只在 handler 被排進去執行時才會跑。在
`USE_NONBLOCKING_HCI = 0`（`hciDrvReadCallback()` 那條路徑被編譯掉）的情況下，
從 handler 外部觸發 transfer 處理的主要來源有兩個：

| 來源 | 位置 |
|---|---|
| `HciDrvIntService()` → `WsfSetEvent()` | `hci_drv_em9305.c:570` |
| `hciDrvWrite()` → `WsfSetEvent()` | `hci_drv_em9305.c:519` |

handler 內部也有補送 transfer 事件的路徑，包括資料未被 stack 全部接收、傳送重試
達門檻，以及讀滿 4 包（見 §3.4）。但這些路徑都必須先進入 handler 才能執行，
不是獨立的背景檢查。

本文的卡住情境是**兩個外部來源都沒有觸發，且沒有待處理的 transfer 事件**：
中斷沒進來，而 Cordio 正在等上一個命令完成，不會送下一個命令。因此 handler
裡雖然有讀取 GPIO 的程式碼，仍沒有機會執行。

（10 秒的 heartbeat 也會叫起 handler，但那條分支送完
`HciReadLocalVerInfoCmd()` 就 return，走不到 transfer 迴圈。）

### 3.3 同一支腳同時服務兩種模型

這是整個設計問題的核心：

| 模型 | 誰在用 | 用途 |
|---|---|---|
| **準位輪詢** | `am_devices_em9305_block_read/write()`、reset 交握、韌體下載 | 同步等待「現在可以讀了嗎」，用 `WHILE_TIMEOUT_MS` 讀電位 |
| **上升邊緣中斷** | `HciDrvIntService()` | 非同步通知「有封包來了」，喚醒 RadioTask |

正常運作時兩者互補。但**若邊緣漏掉且沒有待處理的 transfer 事件，driver 沒有
獨立的背景機制去複查準位**。會讀準位的程式碼，必須先靠中斷、傳送要求，或先前
補排程的事件進入 handler 才能執行。缺口就在這裡。

### 3.4 原廠已有局部補救：讀滿 4 包後重新排程

`hci_drv_em9305.c` 的 blocking 路徑已有以下程式：

```c
/* line 105 */
#define HCI_DRV_MAX_READ_PACKET          4   // max read in a row at a time

/* lines 931–938, inside HciDrvHandler() */
if (read_hci_packet_count >= HCI_DRV_MAX_READ_PACKET)
{
    // It looks like there's time that we won't get interrupt even though
    // there's packet waiting for host to grab.
    WsfSetEvent(g_HciDrvHandleID, BLE_TRANSFER_NEEDED_EVENT);

    break;
}
```

這段原始註解明確記錄了「有封包等待主機讀取，卻沒有收到中斷」的觀察，表示原廠
已針對這類現象加入局部補救。但它不能證明與本文是同一個根因，也不能據此判定
問題在 EM9305、MCU GPIO 中斷，或兩者的處理時序。

`read_hci_packet_count` 在每次進入 handler 時歸零；成功讀取封包、且資料已被
stack 完整接收後才遞增（line 913）。本次累積到 4 包，就先設定下一次 transfer
事件，再用 `break` 離開本次收送迴圈。`WsfSetEvent()` 是設定軟體事件，讓 WSF
之後再次執行 handler，不是立即遞迴呼叫，也不是重發硬體中斷。下一次 handler
會重新檢查 GPIO117，即使沒有新的上升邊緣，仍有機會讀取待處理資料。

**這是在結束本批處理前預約下一次檢查，並非偵測所有漏中斷的機制。** 它的觸發
條件是本次已完整處理 4 包，不是 IRQ 計數停止，也不是已確認還有下一包。

| 情境 | 讀滿 4 包後補排程的涵蓋範圍 |
|---|---|
| 本次已處理 4 包，後續有資料但沒有新中斷 | 會預先補送事件；下次執行時若 INT 為高，就能繼續讀取 |
| 本次只處理 1～3 包便退出，且沒有其他待處理事件 | 不會觸發這個門檻分支，無法靠它保證再次檢查 |
| 第一個中斷就沒進來，handler 根本沒有執行 | 沒有機會執行這段補救 |
| 預先補送的事件已處理完，之後才漏掉新的邊緣 | 先前那次補排程不提供持續保護 |

本文的 `ble_diag_int_kick()`（§5）補上的是另一個觸發來源：從 handler 外部持續
檢查 INT 電位與 IRQ 計數，符合卡住條件才補送 transfer 事件。因此它與原廠的
4 包補排程機制互補；兩者都讓實際 SPI 收送留在 RadioTask 中執行。

---

## 4. 逾時到期後 Cordio 做了什麼

```c
/* hci_cmd.c */
void hciCmdTimeout(wsfMsgHdr_t *pMsg)
{
    HciDrvRadioShutdown();
    HciDrvRadioBoot(0);
    DmDevReset();
}
```

對「漏掉一個封包」的回應是整個控制器重啟：所有 HCI/DM 狀態丟掉、
`g_ui32InterruptsSeen` 歸零、約 60 個 HCI 命令重跑一次。而卡住的那個封包是隨著
重啟一起消失，不是被讀走。

它也**不會發 `DM_HW_ERROR_IND`**。應用層完全看不到任何錯誤，只看到
`[ble] reset cmpl` 莫名其妙在 11 秒或 21 秒出現。沒有診斷工具的話，沒有任何線索
能把那個現象連到中斷問題——這正是這個問題花了這麼久才定案的原因。

把 `HCI_CMD_TIMEOUT` 改短沒有用：那只會更早走到
`shutdown + boot + DmDevReset`，更頻繁地重啟控制器，而且照樣不會去讀那個還在等的
封包。

---

## 5. 補救做法

`ble_diag_int_kick()`（`src/ble_diag.c`）由算繪迴圈每幀呼叫一次，約 18 ms
（`src/render_task.c:8907`）：

```c
while (1)
{
    xSemaphoreTake(g_semDisplayEnd, portMAX_DELAY);

    ble_diag_int_kick();
    ...
}
```

三個條件同時成立才動作：

1. `am_hal_gpio_input_read(AM_BSP_GPIO_EM9305_INT)` 為高電位；
2. `g_ui32InterruptsSeen` 從上一幀到現在完全沒變；
3. 以上狀態已經持續至少 `BLE_DIAG_KICK_STUCK_MS`（20 ms，`ble_diag.c:377`）。

動作只有一行：

```c
/* BLE_TRANSFER_NEEDED_EVENT = 0x01，定義在 hci_drv_em9305.c，未匯出到標頭檔 */
WsfSetEvent(g_HciDrvDiagHandlerId, 0x01);
```

兩次補踢之間至少間隔 `BLE_DIAG_KICK_HOLDOFF_MS`（100 ms，`ble_diag.c:384`），
避免控制器真的沒回應時把 log 淹掉。

**20 ms 這個門檻**：實機 log 顯示正常一次 HCI 往返只要 1~2 ms，20 ms 已經遠超過，
不會和進行中的傳輸搶動作。

**為什麼用 `WsfSetEvent()` 而不直接呼叫 `HciDrvHandler()`。** 這個檢查跑在
RenderTask。直接從那裡呼叫 HCI transport，等於把它搬出 RadioTask 的 context，
會和 RadioTask 自己掌管的事件序列交錯。補送事件則維持原本的架構不變——
RenderTask 只負責偵測，所有 transport 工作仍然由 RadioTask 做：

```
RenderTask          偵測到「INT 高電位、irq 凍結」
     │
     └─ WsfSetEvent(BLE_TRANSFER_NEEDED_EVENT)
            │
            └─ RadioTask → HciDrvHandler() → 讀到腳位是高的
                   → am_devices_em9305_block_read()
                   → hciTrSerialRxIncoming() → hciCmdRecvCmpl()
                   → WsfTimerStop(cmdTimer)      ← 即將到期的 10 秒逾時被取消
                   → 下一個 HCI 命令繼續
```

handler ID 在註冊時用明確的 setter 保存（`src/radio_task.c:270`），而不是去
`extern` 那個 `g_HciDrvHandleID`，避免依賴「0 代表尚未初始化」這種隱含約定。

### 5.1 結果

| 情況 | 修改前 | 修改後 |
|---|---|---|
| 沒漏中斷 | 1.3 秒 | 1.3 秒（完全相同） |
| 漏 1 次 | **11.3 秒** | 約 1.35 秒 |
| 漏 2 次 | **21.3 秒** | 約 1.4 秒 |
| 每次漏接的代價 | +10 秒，控制器重啟、序列重跑 | +20~40 ms，序列繼續 |
| 控制器狀態 | 全部丟掉重建 | 不動 |
| 卡住的封包 | 隨重啟消失 | 正常讀走 |

執行成本：每幀一次 GPIO 讀取加兩次比較。

---

## 6. 診斷機制：完全不改 SDK

需要掛勾的點全部在 `third_party/cordio` 裡。我們沒有去改那些檔案，而是用 GNU ld
的 `--wrap`——它只改寫**跨編譯單元**的呼叫（`gcc/Makefile:713-717`）：

| 掛勾符號 | 定義處 | 呼叫處 | 記錄什麼 |
|---|---|---|---|
| `hciTrSendCmd` | `hci_tr.c` | `hci_cmd.c` | 命令實際寫到 transport 的時間與 opcode |
| `hciTrSerialRxIncoming` | `hci_tr.c` | `hci_drv_em9305.c` | 交給 host stack 的時間 |
| `hciCmdRecvCmpl` | `hci_cmd.c` | `hci_evt.c` | Command Complete / Status |
| `hciCmdTimeout` | `hci_cmd.c` | `hci_core_ps.c` | **定案的關鍵**：逾時時間與卡住的 opcode |
| `HciDrvRadioBoot` | `hci_drv_em9305.c` | `radio_task.c`、`hci_cmd.c` | 控制器重新 boot，cold / warm |

五個符號的定義與呼叫都在不同的 `.c` 檔，符合 `--wrap` 的前提。攔截是否真的生效
可以在 image 裡驗證，不必只憑符號存在：

```
$ arm-none-eabi-objdump -d bin/nemagfx_watchface.axf | grep -A1 __wrap_hciCmdTimeout
4288bc: bl 41a954 <__wrap_hciCmdTimeout>     ← hci_core_ps.c 的呼叫已改指向 wrapper
41a972: bl 426de8 <hciCmdTimeout>            ← wrapper 內確實轉呼叫原函式
```

紀錄只寫 RAM（256 筆 × 12 bytes = 3 KB `.bss`），由優先權最低的 SensorTask
每秒排空一次。

> **一個值得重提的坑。** 第一輪量測整批遺失了 t=1041 ms 到 t=11048 ms 之間的
> 紀錄，log 裡只留下被截斷的半行。原因是 `am_util_stdio_printf()` 把格式化結果
> 寫進單一**無鎖**的 static buffer（`g_prfbuf`），兩個任務同時 printf 時後者會
> 蓋掉前者。現在診斷輸出改成用 `am_util_stdio_vsprintf()` 格式化到自己的緩衝區，
> 再交給有自己鎖的 `SEGGER_RTT_WriteString()`。任何會同時從 RadioTask 與低優先權
> 任務輸出 log 的程式都會踩到這個問題。

---

## 7. 還沒查清楚的部分

**那個上升邊緣為什麼沒有變成 ISR，仍然沒有定論。** 目前的做法是恢復機制，不是
根因的消除。

**而且量不到。** GPIO117 沒有封裝球位。`AP510BFA-CBR` 是 153 pin 的 BGA，從
die 的 224 個 pad 裡只接出 96 個 GPIO，而 117 不在其中——EM9305 是封裝在裡面的
（板子 `am_bsp.h:91` 的 `AM_BSP_IS_SIP`），所以 IOM6、CS、SPI_RDY、EN 與時脈
全部是 die-to-die 連線，只有 RF 與電源需要出到球位。這條訊號根本接不上邏輯分析儀。
不過這同時也讓「訊號完整性」這個解釋變得不太可能——沒有 PCB 走線、沒有接頭、
也沒有板級雜訊。

替代辦法是讀 MCU 自己的 GPIO 中斷**原始狀態閂鎖**，每次補踢都會取樣並輸出：

```c
am_hal_gpio_interrupt_irq_status_get(AM_BSP_EM9305_RADIO_INT_IRQ, false, &ui32Stat);
ui32Latch = (ui32Stat >> (AM_BSP_GPIO_EM9305_INT & 0x1FU)) & 1U;   /* pin 117 -> bit 21 */
```

| `latch` | 判讀 | 往哪裡查 |
|---|---|---|
| `1` 且 `irq` 凍結 | 邊緣**有**被閂鎖，卻沒有變成 `HciDrvIntService()` | 軟體側：共用的 `am_gpio0_607f_isr`，以及它的 read → clear → service 順序 |
| `0` 且 `pin=1` | 邊緣從未被閂鎖，或已被清掉 | 邊緣偵測／硬體側 |

有一個假設我們查過，可以回報它不太成立：
`am_hal_gpio_interrupt_irq_clear(irq, mask)` 是把 mask 寫進 `MCUN0INTxCLR`
（write-1-to-clear，只清傳進去的位元），所以在 `status_get` **之後**才到的邊緣
不會被那次 clear 抹掉——它的位元會留著，IRQ 會再次觸發。這降低了「共用 ISR 的
read-clear 競態」的可能性，讓 `latch=0` 成為比較可能的觀察結果。

**一個可能對 Ambiq 有意義的平台細節。** 這片板子上 GPIO98（觸控面板 INT）與
GPIO117（EM9305）都對應到 `GPIO0_607F_IRQn`。兩個不同的 SDK 元件會展開成同一個
ISR 符號，所以同時用到兩者的應用必須兩個都不要自己定義，改讓
`am_hal_gpio_interrupt_service()` 依腳位分派。本範例在 `src/radio_task.c` 裡有
記錄這件事；它從 SDK 本身看不出來，而且第一次撞到時只會得到一個 multiple
definition 連結錯誤。

---

## 8. 重現方式

可以只關掉補救、保留診斷，這是看到原始行為最乾淨的方式：

| 開關 | 檔案 | 預設 | 作用 |
|---|---|---|---|
| `WATCHFACE_BLE_INT_KICK` | `src/ble_diag.h:85` | `1` | 設 `0` 就只觀察不介入——10 秒延遲會回來 |
| `WATCHFACE_BLE_DIAG` | `src/ble_diag.h:62` | `1` | 總開關。設 `0` 時 `gcc/Makefile` 的五行 `--wrap` 必須一併註解掉，否則連結器會報 `__wrap_*` undefined |
| `WATCHFACE_BLE_IRQ_DIAG` | `src/sensor_task.c` | `1` | 每秒一行的 `[irqdiag]` 快照 |

延遲是機率性的，**要反覆開機**。單次開機正常不代表什麼。要看的東西：

| 觀察到 | 意義 |
|---|---|
| `*** INT stuck high N ms, irq frozen at M, latch=L -> kick ***` | 補救攔到一次；這次開機原本會多花 10 秒 |
| 不再出現 `*** HCI CMD TIMEOUT ***` | 命令逾時消失了 |
| `[ble] reset cmpl` 約 1.1 秒、`start advertising` 約 1.3 秒 | 正常值 |
| 仍出現 `HCI CMD TIMEOUT` | 補踢沒救回來——往 `am_devices_em9305_block_read()` 的回傳值與 SPI 方向查 |
| `INT stuck high` 每秒都有 | 不是偶發漏接，值得回頭查共用的 `am_gpio0_607f_isr` |

---

## 9. 相關檔案

| 檔案 | 角色 |
|---|---|
| `src/ble_diag.h` | 開關、事件標籤、對外介面 |
| `src/ble_diag.c` | 事件環形緩衝區、`--wrap` 掛勾、INT 卡住偵測與補踢 |
| `src/render_task.c:8907` | 每幀呼叫一次 `ble_diag_int_kick()` |
| `src/radio_task.c:260,270` | 註冊診斷用的 handler 包裝並記下它的 ID |
| `src/sensor_task.c:324` | 排空環形緩衝區並印出 `[irqdiag]` |
| `gcc/Makefile:713-717` | 五行 `--wrap` 連結旗標 |

相關但未修改的 SDK 原始碼：`hci_drv_em9305.c`（`HciDrvIntService`、
`HciDrvHandler`）、`hci_cmd.c`（`HCI_CMD_TIMEOUT`、`hciCmdTimeout`、
`hciCmdRecvCmpl`）、`am_devices_em9305.c`（`config_pins`、`block_read`）。
