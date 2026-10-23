//*****************************************************************************
//
//! @file ble_diag.c
//!
//! @brief BLE startup latency diagnostics: HCI transport event ring buffer and --wrap hooks.
//!
//! The rationale and the list of hooks are in ble_diag.h.
//
//*****************************************************************************

#include <stdarg.h>

#include "ble_freertos_tmp119_hts.h"

#include "SEGGER_RTT.h"

#include "wsf_types.h"
#include "wsf_os.h"

#include "hci_drv_apollo.h"
#include "hci_drv_em9305.h"

#include "ble_diag.h"

#if WATCHFACE_BLE_DIAG

//*****************************************************************************
//
// Ring buffer.
//
// 256 entries x 12 bytes = 3 KB (.bss). SensorTask drains it once a second and
// idle traffic produces only single-digit entries per second, so the capacity is
// ample. If a burst after connecting overflows it, the excess is counted in
// g_ui32Dropped rather than overwriting existing entries -- the order of the
// startup phase matters more than completeness after the fact.
//
//*****************************************************************************
#define BLE_DIAG_ENTRIES        256

//
// Cap on how much one drain emits. SensorTask ranks below RadioTask, but RTT
// output still costs time; the cap keeps one call from holding on too long, and
// whatever is left waits for the next second.
//
#define BLE_DIAG_MAX_PER_DRAIN  48

typedef struct
{
    uint32_t ui32Tick;
    uint32_t ui32B;
    uint16_t ui16A;
    uint8_t  ui8Tag;
} ble_diag_entry_t;

static ble_diag_entry_t  g_sEntries[BLE_DIAG_ENTRIES];
static volatile uint32_t g_ui32Head;       // Write position
static volatile uint32_t g_ui32Tail;       // Read position
static volatile uint32_t g_ui32Dropped;    // Entries dropped because the buffer was full

//
// The last HCI command opcode actually written to the transport.
//
// hciCmdCb in hci_cmd.c is non-static, but its type exists only inside that .c
// file, so reaching cmdOpcode would mean copying the struct definition here --
// and a future SDK reordering those fields would break it silently. Instead the
// opcode is cached in __wrap_hciTrSendCmd(): it is the first 2 bytes of the
// command packet, and hciCmdCb.cmdOpcode is taken from the same place one line
// before hciTrSendCmd() is called, so the two are equivalent.
//
static volatile uint16_t g_ui16LastTxOpcode;

//
// Defined in hci_drv_em9305.c (non-static). The number of HciDrvIntService()
// entries; note that HciDrvRadioBoot() zeroes it, so it does not accumulate
// across restarts.
//
extern volatile uint32_t g_ui32InterruptsSeen;

//*****************************************************************************
//
// Entry points to the original functions produced by --wrap.
//
//*****************************************************************************
extern bool_t   __real_hciTrSendCmd(uint8_t *pData);
extern uint16_t __real_hciTrSerialRxIncoming(uint8_t *pBuf, uint16_t len);
extern void     __real_hciCmdRecvCmpl(uint8_t numCmdPkts);
extern void     __real_hciCmdTimeout(wsfMsgHdr_t *pMsg);
extern uint32_t __real_HciDrvRadioBoot(bool bColdBoot);

//*****************************************************************************
//
// Emit one line, bypassing the shared buffer of am_util_stdio_printf().
//
// am_util_stdio_printf() formats into a single unlocked static buffer
// (g_prfbuf), so when two tasks printf at once the later overwrites the earlier.
// The first round of on-target measurement lost the whole record between
// t=1041 and 11048 ms this way -- exactly the stretch that mattered most.
//
// Here it formats into its own buffer instead (am_util_stdio_vsprintf() writes
// to an address the caller supplies and never touches g_prfbuf), then hands it
// to SEGGER_RTT_WriteString(); RTT has its own lock and is safe across tasks.
//
// g_cLine is static rather than a local: ble_diag_drain() is called only by
// SensorTask and needs no reentrancy, and SensorTask's stack is only 512 words.
//
//*****************************************************************************
static char g_cLine[192];

static void
ble_diag_emit(const char *pcFmt, ...)
{
    va_list args;

    va_start(args, pcFmt);
    am_util_stdio_vsprintf(g_cLine, pcFmt, args);
    va_end(args);

    SEGGER_RTT_WriteString(0, g_cLine);
}

//*****************************************************************************
//
// Interrupt path sample: pack the IRQ entry count and the GPIO117 level into one field.
//
// On GPIO117, "high = the EM9305 has data waiting for the host". The basis is
// HciDrvHandler() in hci_drv_em9305.c: it calls am_devices_em9305_block_read()
// only while am_hal_gpio_input_read(GPIO_INT) is true, then waits for it to
// return to 0 so the same packet is not read twice.
//
//*****************************************************************************
static uint32_t
ble_diag_irq_sample(void)
{
    uint32_t ui32Pin = am_hal_gpio_input_read(AM_BSP_GPIO_EM9305_INT) ? 1U : 0U;

    return (g_ui32InterruptsSeen << 1) | ui32Pin;
}

//*****************************************************************************
//
// Push one event.
//
//*****************************************************************************
void
ble_diag_log(uint8_t ui8Tag, uint16_t ui16A, uint32_t ui32B)
{
    uint32_t ui32Tick = xTaskGetTickCount();
    uint32_t ui32Next;

    AM_CRITICAL_BEGIN;

    ui32Next = g_ui32Head + 1;

    if ( ui32Next >= BLE_DIAG_ENTRIES )
    {
        ui32Next = 0;
    }

    if ( ui32Next == g_ui32Tail )
    {
        //
        // Full. Drop this entry and only count it.
        //
        g_ui32Dropped++;
    }
    else
    {
        g_sEntries[g_ui32Head].ui32Tick = ui32Tick;
        g_sEntries[g_ui32Head].ui32B    = ui32B;
        g_sEntries[g_ui32Head].ui16A    = ui16A;
        g_sEntries[g_ui32Head].ui8Tag   = ui8Tag;

        g_ui32Head = ui32Next;
    }

    AM_CRITICAL_END;
}

//*****************************************************************************
//
// Drain and emit.
//
//*****************************************************************************
uint32_t
ble_diag_drain(void)
{
    uint32_t ui32Printed = 0;

    while ( ui32Printed < BLE_DIAG_MAX_PER_DRAIN )
    {
        ble_diag_entry_t sEntry;
        bool             bHave;

        AM_CRITICAL_BEGIN;

        bHave = (g_ui32Tail != g_ui32Head);

        if ( bHave )
        {
            uint32_t ui32Next;

            sEntry   = g_sEntries[g_ui32Tail];

            ui32Next   = g_ui32Tail + 1;
            g_ui32Tail = (ui32Next >= BLE_DIAG_ENTRIES) ? 0 : ui32Next;
        }

        AM_CRITICAL_END;

        if ( !bHave )
        {
            break;
        }

        switch ( sEntry.ui8Tag )
        {
            case BLE_DIAG_TAG_TX_CMD:
                ble_diag_emit("[bledbg] t=%u ms  tx cmd op=0x%04X\r\n",
                              (unsigned)sEntry.ui32Tick,
                              (unsigned)sEntry.ui16A);
                break;

            case BLE_DIAG_TAG_RX:
                //
                // hdr holds the first 4 bytes handed to the host stack:
                //   [31:24] HCI packet type (0x04 = event)
                //   [23:16] event code (0x0E = Command Complete, 0x0F = Command Status)
                //   [15:8]  parameter total length
                //   [7:0]   numCmdPkts for Command Complete / Status
                //
                ble_diag_emit("[bledbg] t=%u ms  rx len=%u hdr=0x%08X\r\n",
                              (unsigned)sEntry.ui32Tick,
                              (unsigned)sEntry.ui16A,
                              (unsigned)sEntry.ui32B);
                break;

            case BLE_DIAG_TAG_CMD_CMPL:
                ble_diag_emit("[bledbg] t=%u ms  cmd cmpl op=0x%04X pkts=%u\r\n",
                              (unsigned)sEntry.ui32Tick,
                              (unsigned)sEntry.ui16A,
                              (unsigned)sEntry.ui32B);
                break;

            case BLE_DIAG_TAG_CMD_TIMEOUT:
                ble_diag_emit("[bledbg] t=%u ms  *** HCI CMD TIMEOUT op=0x%04X irq=%u pin=%u -> shutdown+boot+DmDevReset ***\r\n",
                              (unsigned)sEntry.ui32Tick,
                              (unsigned)sEntry.ui16A,
                              (unsigned)(sEntry.ui32B >> 1),
                              (unsigned)(sEntry.ui32B & 1U));
                break;

            case BLE_DIAG_TAG_RADIO_BOOT:
                ble_diag_emit("[bledbg] t=%u ms  HciDrvRadioBoot(cold=%u) enter\r\n",
                              (unsigned)sEntry.ui32Tick,
                              (unsigned)sEntry.ui16A);
                break;

            case BLE_DIAG_TAG_DRV_EVT:
                //
                // a[7:0] = wsf event mask, a[15:8] = pMsg->event (0xFF = no message)
                // b[31:1] = g_ui32InterruptsSeen, b[0] = GPIO117 level
                //
                ble_diag_emit("[bledbg] t=%u ms  drv evt=0x%02X msg=0x%02X irq=%u pin=%u\r\n",
                              (unsigned)sEntry.ui32Tick,
                              (unsigned)(sEntry.ui16A & 0xFFU),
                              (unsigned)((sEntry.ui16A >> 8) & 0xFFU),
                              (unsigned)(sEntry.ui32B >> 1),
                              (unsigned)(sEntry.ui32B & 1U));
                break;

            case BLE_DIAG_TAG_HEARTBEAT:
                ble_diag_emit("[bledbg] t=%u ms  heartbeat -> HciReadLocalVerInfoCmd, irq=%u pin=%u\r\n",
                              (unsigned)sEntry.ui32Tick,
                              (unsigned)(sEntry.ui32B >> 1),
                              (unsigned)(sEntry.ui32B & 1U));
                break;

            case BLE_DIAG_TAG_INT_KICK:
                //
                // latch = the pin 117 bit in the raw GPIO interrupt status latch.
                // See the comment in ble_diag_int_kick() for how to read it.
                //
                ble_diag_emit("[bledbg] t=%u ms  *** INT stuck high %u ms, irq frozen at %u, latch=%u -> kick WsfSetEvent ***\r\n",
                              (unsigned)sEntry.ui32Tick,
                              (unsigned)sEntry.ui16A,
                              (unsigned)(sEntry.ui32B >> 2),
                              (unsigned)((sEntry.ui32B >> 1) & 1U));
                break;

            default:
                ble_diag_emit("[bledbg] t=%u ms  tag=%u a=0x%04X b=0x%08X\r\n",
                              (unsigned)sEntry.ui32Tick,
                              (unsigned)sEntry.ui8Tag,
                              (unsigned)sEntry.ui16A,
                              (unsigned)sEntry.ui32B);
                break;
        }

        ui32Printed++;
    }

    //
    // If anything was dropped, report it once and reset, so it is not misread as
    // "nothing happened during this period".
    //
    if ( g_ui32Dropped != 0 )
    {
        uint32_t ui32Dropped;

        AM_CRITICAL_BEGIN;
        ui32Dropped   = g_ui32Dropped;
        g_ui32Dropped = 0;
        AM_CRITICAL_END;

        ble_diag_emit("[bledbg] *** dropped %u entries (ring full) ***\r\n",
                      (unsigned)ui32Dropped);
    }

    return ui32Printed;
}

//*****************************************************************************
//
// HciDrvHandler() wrapper.
//
// This wrapper is one of the keys to the diagnosis: the GPIO_INT polling inside
// HciDrvHandler() only runs when the handler is invoked, and what invokes it is
// either HciDrvIntService() or the WsfSetEvent() in hciDrvWrite(). So if no
// interrupt arrives at all, the polling never happens by itself; the irq count
// and pin level recorded on entry tell the two cases apart directly.
//
//*****************************************************************************
void
ble_diag_hci_drv_handler(wsfEventMask_t event, wsfMsgHdr_t *pMsg)
{
    uint16_t ui16MsgEvent = (pMsg != NULL) ? (uint16_t)pMsg->event : 0xFFU;

    //
    // BLE_HEARTBEAT_EVENT is defined as 0x02 inside hci_drv_em9305.c, but that
    // #define is not exported to a header, hence the literal and this note.
    //
    if ( (pMsg != NULL) && (pMsg->event == 0x02) )
    {
        ble_diag_log(BLE_DIAG_TAG_HEARTBEAT, 0, ble_diag_irq_sample());
    }
    else
    {
        ble_diag_log(BLE_DIAG_TAG_DRV_EVT,
                     (uint16_t)((ui16MsgEvent << 8) | (uint16_t)event),
                     ble_diag_irq_sample());
    }

    HciDrvHandler(event, pMsg);
}

//*****************************************************************************
//
// Stuck-INT detection and re-kick.
//
// Why this is needed: GPIO117 is configured by am_hal_gpio_pincfg_input with
// eIntDir = AM_HAL_GPIO_PIN_INTDIR_LO2HI, i.e. **rising edge only**; Apollo5
// GPIO interrupts have no level-triggered mode. So once INT is already high and
// the host has missed that transition, the hardware produces no second
// interrupt and the only way out is the 10 s hciCmdTimeout() -- which is exactly
// the path measured on target: op=0x0C01, irq frozen, pin=1.
//
// The remedy is a periodic check: if INT stays high while g_ui32InterruptsSeen
// does not move at all for more than BLE_DIAG_KICK_STUCK_MS, post one extra
// BLE_TRANSFER_NEEDED_EVENT so HciDrvHandler() drains the backlog itself.
//
//*****************************************************************************

//
// Threshold for declaring it stuck. A normal HCI round trip takes 1 to 2 ms (see
// the on-target log), so 20 ms is far beyond normal and cannot race a read in progress.
//
#define BLE_DIAG_KICK_STUCK_MS      20

//
// Minimum interval between two kicks. If it is still stuck after a kick (the
// controller genuinely not responding, say), keep retrying periodically without
// flooding the log.
//
#define BLE_DIAG_KICK_HOLDOFF_MS    100

static wsfHandlerId_t g_HciDrvDiagHandlerId;
static bool           g_bHciHandlerIdValid;

void
ble_diag_set_hci_handler_id(wsfHandlerId_t handlerId)
{
    g_HciDrvDiagHandlerId = handlerId;
    g_bHciHandlerIdValid  = true;
}

#if WATCHFACE_BLE_INT_KICK

static uint32_t g_ui32KickPrevIrq;
static uint32_t g_ui32KickHighSince;   // Tick at which INT started staying high
static uint32_t g_ui32KickLastTick;    // Tick of the last kick
static uint32_t g_ui32KickCount;
static bool     g_bKickHighPrev;

void
ble_diag_int_kick(void)
{
    uint32_t ui32Now = xTaskGetTickCount();
    uint32_t ui32Irq = g_ui32InterruptsSeen;
    bool     bHigh   = (am_hal_gpio_input_read(AM_BSP_GPIO_EM9305_INT) != 0);

    if ( !bHigh || (ui32Irq != g_ui32KickPrevIrq) )
    {
        //
        // INT went back low, or a new interrupt arrived in the meantime -- either
        // way the path is alive. Restart the timer.
        //
        g_bKickHighPrev     = bHigh;
        g_ui32KickPrevIrq   = ui32Irq;
        g_ui32KickHighSince = ui32Now;
        return;
    }

    if ( !g_bKickHighPrev )
    {
        //
        // First time this round that it is seen high; start counting from now.
        //
        g_bKickHighPrev     = true;
        g_ui32KickHighSince = ui32Now;
        return;
    }

    if ( (ui32Now - g_ui32KickHighSince) < BLE_DIAG_KICK_STUCK_MS )
    {
        return;
    }

    if ( !g_bHciHandlerIdValid )
    {
        //
        // exactle_stack_init() has not yet reached the HCI driver handler registration.
        //
        return;
    }

    if ( (g_ui32KickCount != 0) &&
         ((ui32Now - g_ui32KickLastTick) < BLE_DIAG_KICK_HOLDOFF_MS) )
    {
        return;
    }

    g_ui32KickCount++;
    g_ui32KickLastTick = ui32Now;

    //
    // Also sample the "raw" GPIO interrupt status latch (bEnabledOnly = false).
    //
    // GPIO117 connects to the EM9305 die inside the AP510BFA-CBR package and has
    // no ball to probe (the package brings out only 96 GPIOs and 117 is not in
    // the ball map), so a logic analyser is of no use. This bit is the only clue
    // available on the MCU side that separates the two possibilities:
    //
    //   latch=1 with irq frozen -> the edge was latched and lost in the ISR or dispatch layer (software)
    //   latch=0 with pin=1      -> the edge was never latched, or has been cleared (edge detect / hardware)
    //
    {
        uint32_t ui32Stat  = 0;
        uint32_t ui32Latch;

        am_hal_gpio_interrupt_irq_status_get(AM_BSP_EM9305_RADIO_INT_IRQ,
                                             false, &ui32Stat);

        ui32Latch = (ui32Stat >> (AM_BSP_GPIO_EM9305_INT & 0x1FU)) & 1U;

        ble_diag_log(BLE_DIAG_TAG_INT_KICK,
                     (uint16_t)(ui32Now - g_ui32KickHighSince),
                     (ui32Irq << 2) | (ui32Latch << 1) | 1U);
    }

    //
    // BLE_TRANSFER_NEEDED_EVENT = 0x01, defined in hci_drv_em9305.c, not exported to a header.
    //
    WsfSetEvent(g_HciDrvDiagHandlerId, 0x01);
}

#else

void
ble_diag_int_kick(void)
{
}

#endif // WATCHFACE_BLE_INT_KICK

//*****************************************************************************
//
// --wrap hooks.
//
//*****************************************************************************
bool_t
__wrap_hciTrSendCmd(uint8_t *pData)
{
    uint16_t ui16Opcode = (uint16_t)pData[0] | ((uint16_t)pData[1] << 8);

    g_ui16LastTxOpcode = ui16Opcode;

    ble_diag_log(BLE_DIAG_TAG_TX_CMD, ui16Opcode, 0);

    return __real_hciTrSendCmd(pData);
}

uint16_t
__wrap_hciTrSerialRxIncoming(uint8_t *pBuf, uint16_t len)
{
    uint32_t ui32Hdr = 0;

    for ( uint16_t i = 0; (i < 4U) && (i < len); i++ )
    {
        ui32Hdr |= ((uint32_t)pBuf[i]) << (24 - (8 * i));
    }

    ble_diag_log(BLE_DIAG_TAG_RX, len, ui32Hdr);

    return __real_hciTrSerialRxIncoming(pBuf, len);
}

void
__wrap_hciCmdRecvCmpl(uint8_t numCmdPkts)
{
    ble_diag_log(BLE_DIAG_TAG_CMD_CMPL, g_ui16LastTxOpcode, numCmdPkts);

    __real_hciCmdRecvCmpl(numCmdPkts);
}

void
__wrap_hciCmdTimeout(wsfMsgHdr_t *pMsg)
{
    //
    // This entry is what the whole diagnosis is after: confirming whether those
    // 10 seconds are an HCI command timeout, and which opcode is stuck.
    //
    // hciCmdTimeout() goes straight to HciDrvRadioShutdown() + HciDrvRadioBoot(0)
    // + DmDevReset() without raising DM_HW_ERROR_IND first -- which explains the
    // "reset cmpl appears again with no HW error log".
    //
    ble_diag_log(BLE_DIAG_TAG_CMD_TIMEOUT, g_ui16LastTxOpcode, ble_diag_irq_sample());

    __real_hciCmdTimeout(pMsg);
}

uint32_t
__wrap_HciDrvRadioBoot(bool bColdBoot)
{
    ble_diag_log(BLE_DIAG_TAG_RADIO_BOOT, (uint16_t)(bColdBoot ? 1U : 0U), 0);

    return __real_HciDrvRadioBoot(bColdBoot);
}

#endif // WATCHFACE_BLE_DIAG
