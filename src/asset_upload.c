//*****************************************************************************
//
//! @file asset_upload.c
//!
//! @brief Write asset files from the PC into eMMC over RTT.
//!
//! The protocol and its rationale are in asset_upload.h.
//
//*****************************************************************************

#include <string.h>

#include "am_mcu_apollo.h"
#include "am_util.h"

#include "FreeRTOS.h"
#include "task.h"

#include "asset_store.h"
#include "asset_upload.h"

#include "SEGGER_RTT.h"

#if WATCHFACE_ASSET_UPLOAD

//*****************************************************************************
//
// Task handle.
//
//*****************************************************************************
TaskHandle_t AssetUploadTaskHandle;

//*****************************************************************************
//
//! @brief Receive buffer size.
//!
//! @details Data pulled from RTT lands here first and is written to the file
//! once the buffer fills. 1 KB balances write count against RAM: an eMMC sector
//! is 512 bytes, 1 KB is exactly two of them, and FatFs never has to do an extra
//! read-modify-write to round up.
//!
//! It needs no alignment and no AM_SHARED_RW -- f_write() copies the data into
//! the sector buffer inside the FATFS object, and DMA reads that, not this.
//
//*****************************************************************************
#define UPLOAD_CHUNK        (1024)

//*****************************************************************************
//
//! @brief How long without data before this transfer is abandoned.
//!
//! @details When the host disconnects mid-transfer (closing the GUI, unplugging
//! J-Link), the target is left "a few bytes short". Without a timeout that
//! half-open file holds the filesystem lock forever and map tiles can never be
//! swapped in again -- the symptom is the display slowly degrading for no visible reason.
//!
//! 3 s is far longer than the normal inter-packet gap (the host sends
//! continuously) and short enough that the user does not assume a hang.
//
//*****************************************************************************
#define UPLOAD_TIMEOUT_MS   (10000)

//*****************************************************************************
//
//! @brief How long a stall before the progress report is resent.
//!
//! @details The ack goes out over the log, and the RTT up buffer is in
//! NO_BLOCK_SKIP mode -- **when it is full the line is simply dropped**. A
//! 412 KB file needs over four hundred ack lines, and the odds of one being
//! squeezed out by another log (render, BLE) are high.
//!
//! And one lost ack is a deadlock: the host waits for confirmation and does not
//! send, the target receives nothing and does not report again, and both wait
//! until the timeout. The symptom is large files breaking off at random points
//! (2048 / 18432 / 21504 / 39936 / 45056 were measured), while small files have
//! only a few acks and almost never hit it.
//!
//! So after a stall longer than this, the current progress is printed again. The
//! ack is cumulative ("this many bytes received") rather than incremental, so
//! resending is entirely harmless and any single line lets the host continue.
//
//*****************************************************************************
#define UPLOAD_REACK_MS     (250)

//*****************************************************************************
//
//! @brief How many bytes between progress reports (flow control).
//!
//! @details **This decides whether large files get through at all.**
//!
//! The RTT down buffer is 4 KB (BUFFER_SIZE_DOWN in the Makefile), and once it
//! is full J-Link **simply discards the excess** instead of stalling the host.
//! With no feedback the host sends at its own pace -- this task has the lowest
//! priority and cannot keep up, so everything past the first 4 KB vanishes and
//! the target sits "a few bytes short" until UPLOAD_TIMEOUT_MS expires.
//! The symptom is `abort <name>: timeout` on every file larger than 4 KB.
//!
//! So every time this many bytes have arrived, one `[upload] ack <bytes
//! received>` line is printed and the host only continues after reading it (see
//! WINDOW in tools/asset_upload.py). The host's unacknowledged volume therefore
//! always stays below the down buffer and nothing is ever discarded.
//!
//! 1 KB, the same as UPLOAD_CHUNK: exactly the boundary of "one eMMC write
//! finished", so the reported progress equals the bytes actually on disk.
//
//*****************************************************************************
#define UPLOAD_ACK_BYTES    (1024)

//*****************************************************************************
//
// Receive state machine.
//
//*****************************************************************************
typedef enum
{
    UP_STATE_MAGIC = 0,     // Looking for "ASU1"
    UP_STATE_HEADER,        // namelen + name + size
    UP_STATE_DATA,          // File contents
    UP_STATE_SUM,           // Trailing checksum
    UP_STATE_DEL            // namelen + name of a delete command
} upload_state_e;

static upload_state_e g_eState;

//
// Header scratch. The maximum is 1(namelen) + 12(name) + 4(size) = 17.
//
#define UPLOAD_HDR_MAX      (1 + ASSET_UPLOAD_NAME_MAX + 4)
static uint8_t  g_pui8Hdr[UPLOAD_HDR_MAX];
static uint32_t g_ui32HdrGot;
static uint32_t g_ui32HdrNeed;

static char     g_cName[ASSET_UPLOAD_NAME_MAX + 1];
static uint32_t g_ui32Size;
static uint32_t g_ui32Got;
static uint32_t g_ui32Sum;

//
// g_ui32Got at the last progress report; see UPLOAD_ACK_BYTES.
//
static uint32_t g_ui32Acked;

//
// When the last "resend because stalled" happened; see UPLOAD_REACK_MS.
//
static uint32_t g_ui32ReackTick;

static uint8_t  g_pui8Buf[UPLOAD_CHUNK];
static uint32_t g_ui32BufUsed;

//
// How far each of the three magics has matched.
//
// "ASU1" (upload), "ASL1" (list) and "ASD1" (delete) share their first two
// characters, so all three must be tracked at once -- with a single counter,
// "ASL" would have its first three characters consumed by the upload matcher,
// the fourth would mismatch, the whole group would be discarded, and the list
// and delete commands would never be recognized.
//
static uint32_t g_ui32MagicGot;
static uint32_t g_ui32ListGot;
static uint32_t g_ui32DelGot;

static uint32_t g_ui32LastRxTick;
static bool     g_bActive;

//*****************************************************************************
//
//! @brief Abandon the current transfer.
//!
//! @param pcWhy The reason, which is printed.
//
//*****************************************************************************
static void
upload_abort(const char *pcWhy)
{
    if ( g_eState == UP_STATE_DATA || g_eState == UP_STATE_SUM )
    {
        //
        // The file is already open, so close it and release the lock. What stays
        // on eMMC is half a file of the wrong length, which the provisioning
        // check treats as missing on the next boot (see AssetStoreWriteAbort).
        //
        AssetStoreWriteAbort();
        am_util_stdio_printf("[upload] abort %s: %s\n", g_cName, pcWhy);
    }

    g_eState       = UP_STATE_MAGIC;
    g_ui32MagicGot = 0;
    g_ui32ListGot  = 0;
    g_ui32DelGot   = 0;
    g_ui32HdrGot   = 0;
    g_ui32BufUsed  = 0;
    g_bActive      = false;
}

//*****************************************************************************
//
//! @brief Write the data accumulated in the receive buffer to the file.
//!
//! @return true when the write succeeded.
//
//*****************************************************************************
static bool
upload_flush(void)
{
    bool bOk = true;

    if ( g_ui32BufUsed > 0 )
    {
        bOk = AssetStoreWriteChunk(g_pui8Buf, g_ui32BufUsed);
        g_ui32BufUsed = 0;
    }

    return bOk;
}

//*****************************************************************************
//
//! @brief AssetStoreList() callback: print one file.
//
//*****************************************************************************
static void
upload_list_entry(const char *pcName, uint32_t ui32Bytes)
{
    am_util_stdio_printf("[list] %s %u\n", pcName, (unsigned)ui32Bytes);
}

//*****************************************************************************
//
//! @brief Handle the list command.
//!
//! @details The listing is bracketed with begin/end so the host knows where to
//! start and stop reading -- the log is shared with other tasks and messages
//! from elsewhere in between are normal.
//
//*****************************************************************************
static void
upload_do_list(void)
{
    uint32_t ui32TotalKB = 0;
    uint32_t ui32FreeKB  = 0;
    int32_t  i32Count;

    am_util_stdio_printf("[list] begin\n");

    i32Count = AssetStoreList(upload_list_entry, &ui32TotalKB, &ui32FreeKB);

    if ( i32Count < 0 )
    {
        am_util_stdio_printf("[list] error\n");
        return;
    }

    am_util_stdio_printf("[list] end %d files, %u KB free of %u KB\n",
                         (int)i32Count, (unsigned)ui32FreeKB,
                         (unsigned)ui32TotalKB);
}

//*****************************************************************************
//
//! @brief Process one received byte.
//!
//! @param ui8Byte The byte.
//
//*****************************************************************************
static void
upload_feed(uint8_t ui8Byte)
{
    static const char cMagic[] = ASSET_UPLOAD_MAGIC;
    static const char cList[]  = ASSET_LIST_MAGIC;
    static const char cDel[]   = ASSET_DEL_MAGIC;

    switch ( g_eState )
    {
        case UP_STATE_MAGIC:
            //
            // Match two magics at once. A mismatch restarts from the beginning --
            // the RTT down buffer is shared and whatever the user types in a
            // terminal arrives here too, so unrecognized bytes are simply dropped.
            //
            // On a mismatch the byte may still be the start of a new magic, as in
            // "AASU1", so matching restarts rather than resetting unconditionally.
            //
            if ( ui8Byte == (uint8_t)cList[g_ui32ListGot] )
            {
                g_ui32ListGot++;
                if ( g_ui32ListGot == ASSET_UPLOAD_MAGIC_LEN )
                {
                    g_ui32ListGot  = 0;
                    g_ui32MagicGot = 0;
                    g_ui32DelGot   = 0;
                    upload_do_list();
                    break;
                }
            }
            else
            {
                g_ui32ListGot = (ui8Byte == (uint8_t)cList[0]) ? 1 : 0;
            }

            //
            // Delete command. Unlike list it is followed by a file name, so it is
            // not executed on the spot but moves to a state that collects those bytes.
            //
            if ( ui8Byte == (uint8_t)cDel[g_ui32DelGot] )
            {
                g_ui32DelGot++;
                if ( g_ui32DelGot == ASSET_UPLOAD_MAGIC_LEN )
                {
                    g_ui32DelGot   = 0;
                    g_ui32ListGot  = 0;
                    g_ui32MagicGot = 0;
                    g_eState       = UP_STATE_DEL;
                    g_ui32HdrGot   = 0;
                    g_ui32HdrNeed  = 1;      // namelen only for now
                    break;
                }
            }
            else
            {
                g_ui32DelGot = (ui8Byte == (uint8_t)cDel[0]) ? 1 : 0;
            }

            if ( ui8Byte == (uint8_t)cMagic[g_ui32MagicGot] )
            {
                g_ui32MagicGot++;
                if ( g_ui32MagicGot == ASSET_UPLOAD_MAGIC_LEN )
                {
                    g_ui32MagicGot = 0;
                    g_ui32ListGot  = 0;
                    g_ui32DelGot   = 0;
                    g_eState       = UP_STATE_HEADER;
                    g_ui32HdrGot   = 0;
                    g_ui32HdrNeed  = 1;      // namelen only for now
                }
            }
            else
            {
                g_ui32MagicGot = (ui8Byte == (uint8_t)cMagic[0]) ? 1 : 0;
            }
            break;

        case UP_STATE_HEADER:
            g_pui8Hdr[g_ui32HdrGot++] = ui8Byte;

            if ( g_ui32HdrGot == 1 )
            {
                //
                // namelen is known; work out how much more header is needed.
                //
                uint32_t ui32NameLen = g_pui8Hdr[0];

                if ( (ui32NameLen == 0) ||
                     (ui32NameLen > ASSET_UPLOAD_NAME_MAX) )
                {
                    am_util_stdio_printf("[upload] bad name length %u\n",
                                         (unsigned)ui32NameLen);
                    upload_abort("bad header");
                    break;
                }

                g_ui32HdrNeed = 1 + ui32NameLen + 4;
            }

            if ( g_ui32HdrGot >= g_ui32HdrNeed )
            {
                uint32_t ui32NameLen = g_pui8Hdr[0];

                memset(g_cName, 0, sizeof(g_cName));
                memcpy(g_cName, &g_pui8Hdr[1], ui32NameLen);

                g_ui32Size = ((uint32_t)g_pui8Hdr[1 + ui32NameLen])
                           | ((uint32_t)g_pui8Hdr[2 + ui32NameLen] << 8)
                           | ((uint32_t)g_pui8Hdr[3 + ui32NameLen] << 16)
                           | ((uint32_t)g_pui8Hdr[4 + ui32NameLen] << 24);

                if ( (g_ui32Size == 0) || (g_ui32Size > (8u * 1024u * 1024u)) )
                {
                    am_util_stdio_printf("[upload] bad size %u\n",
                                         (unsigned)g_ui32Size);
                    upload_abort("bad header");
                    break;
                }

                am_util_stdio_printf("[upload] recv %s %u bytes\n",
                                     g_cName, (unsigned)g_ui32Size);

                if ( !AssetStoreWriteBegin(g_cName) )
                {
                    //
                    // The file would not open, so drop this transfer. The state
                    // has not reached DATA yet, so upload_abort() will not try to
                    // close a file that was never opened.
                    //
                    upload_abort("open failed");
                    break;
                }

                g_ui32Got     = 0;
                g_ui32Sum     = 0;
                g_ui32Acked   = 0;
                g_ui32BufUsed = 0;
                g_bActive     = true;
                g_eState      = UP_STATE_DATA;
            }
            break;

        case UP_STATE_DEL:
            //
            // namelen + name of the delete command. It borrows the upload header
            // scratch -- the two can never run at once (the state machine is in
            // one state at a time) and the lengths used here are shorter.
            //
            g_pui8Hdr[g_ui32HdrGot++] = ui8Byte;

            if ( g_ui32HdrGot == 1 )
            {
                uint32_t ui32NameLen = g_pui8Hdr[0];

                if ( (ui32NameLen == 0) ||
                     (ui32NameLen > ASSET_UPLOAD_NAME_MAX) )
                {
                    am_util_stdio_printf("[delete] bad name length %u\n",
                                         (unsigned)ui32NameLen);
                    upload_abort("bad delete header");
                    break;
                }

                g_ui32HdrNeed = 1 + ui32NameLen;
            }

            if ( g_ui32HdrGot >= g_ui32HdrNeed )
            {
                uint32_t ui32NameLen = g_pui8Hdr[0];

                memset(g_cName, 0, sizeof(g_cName));
                memcpy(g_cName, &g_pui8Hdr[1], ui32NameLen);

                //
                // Success and failure share one prefix, which is how the host
                // recognizes the result. A file that was never there also counts
                // as a failure -- what the host wants to know is "is it there
                // now", and that question belongs to [list], not to a vague answer here.
                //
                if ( AssetStoreDelete(g_cName) )
                {
                    am_util_stdio_printf("[delete] ok %s\n", g_cName);
                }
                else
                {
                    am_util_stdio_printf("[delete] fail %s\n", g_cName);
                }

                g_eState     = UP_STATE_MAGIC;
                g_ui32HdrGot = 0;
            }
            break;

        case UP_STATE_DATA:
            g_pui8Buf[g_ui32BufUsed++] = ui8Byte;
            g_ui32Sum += ui8Byte;
            g_ui32Got++;

            if ( g_ui32BufUsed >= UPLOAD_CHUNK )
            {
                if ( !upload_flush() )
                {
                    upload_abort("write failed");
                    break;
                }
            }

            //
            // Report progress so the host knows it may send more. Placed after
            // the flush, so what is reported has genuinely reached eMMC. See
            // UPLOAD_ACK_BYTES.
            //
            if ( (g_ui32Got - g_ui32Acked) >= UPLOAD_ACK_BYTES )
            {
                g_ui32Acked = g_ui32Got;
                am_util_stdio_printf("[upload] ack %u\n", (unsigned)g_ui32Got);
            }

            if ( g_ui32Got >= g_ui32Size )
            {
                if ( !upload_flush() )
                {
                    upload_abort("write failed");
                    break;
                }

                g_ui32HdrGot = 0;
                g_eState     = UP_STATE_SUM;
            }
            break;

        case UP_STATE_SUM:
            g_pui8Hdr[g_ui32HdrGot++] = ui8Byte;

            if ( g_ui32HdrGot >= 4 )
            {
                uint32_t ui32Want = ((uint32_t)g_pui8Hdr[0])
                                  | ((uint32_t)g_pui8Hdr[1] << 8)
                                  | ((uint32_t)g_pui8Hdr[2] << 16)
                                  | ((uint32_t)g_pui8Hdr[3] << 24);

                bool bClosed = AssetStoreWriteEnd();

                if ( !bClosed )
                {
                    am_util_stdio_printf("[upload] err %s: close failed\n",
                                         g_cName);
                }
                else if ( ui32Want != g_ui32Sum )
                {
                    //
                    // The file is written and closed, but the contents do not
                    // match. It is deliberately not deleted -- leaving it where
                    // the user can see "right length, wrong contents" is easier
                    // to diagnose than a silent disappearance. Resending overwrites it.
                    //
                    am_util_stdio_printf("[upload] err %s: sum %08x != %08x\n",
                                         g_cName, (unsigned)g_ui32Sum,
                                         (unsigned)ui32Want);
                }
                else
                {
                    am_util_stdio_printf("[upload] ok %s\n", g_cName);
                }

                g_eState       = UP_STATE_MAGIC;
                g_ui32MagicGot = 0;
                g_bActive      = false;
            }
            break;

        default:
            g_eState = UP_STATE_MAGIC;
            break;
    }
}

//*****************************************************************************
//
// The upload task itself.
//
//*****************************************************************************
void
AssetUploadTask(void *pvParameters)
{
    (void)pvParameters;

    g_eState         = UP_STATE_MAGIC;
    g_ui32LastRxTick = xTaskGetTickCount();

    am_util_debug_printf("[upload] task started (RTT ch0 down)\n");

    while (1)
    {
        //
        // Read a bit more at a time. This task has the lowest priority and its
        // run slots are fragmented anyway; the more each slot moves, the less
        // likely the down buffer is to fill -- and when it fills J-Link discards
        // the excess (see UPLOAD_ACK_BYTES). 256 is still easy on the stack.
        //
        uint8_t  pui8Rx[256];
        unsigned uRead;

        uRead = SEGGER_RTT_Read(0, pui8Rx, sizeof(pui8Rx));

        if ( uRead > 0 )
        {
            unsigned i;

            for ( i = 0; i < uRead; i++ )
            {
                upload_feed(pui8Rx[i]);
            }

            g_ui32LastRxTick = xTaskGetTickCount();
        }
        else
        {
            uint32_t ui32Idle = xTaskGetTickCount() - g_ui32LastRxTick;

            //
            // Stalled. Assume first that an ack was dropped by the RTT up buffer
            // and deadlocked things, so report progress again (see
            // UPLOAD_REACK_MS). The host continues as soon as it arrives.
            //
            if ( (g_eState == UP_STATE_DATA) &&
                 (ui32Idle > pdMS_TO_TICKS(UPLOAD_REACK_MS)) &&
                 ((xTaskGetTickCount() - g_ui32ReackTick) >
                  pdMS_TO_TICKS(UPLOAD_REACK_MS)) )
            {
                g_ui32ReackTick = xTaskGetTickCount();
                am_util_stdio_printf("[upload] ack %u\n", (unsigned)g_ui32Got);
            }

            //
            // Only give up when resending did not help, and release the lock --
            // otherwise the half-open file keeps blocking map tile loads (see
            // UPLOAD_TIMEOUT_MS).
            //
            if ( (g_eState != UP_STATE_MAGIC) &&
                 (ui32Idle > pdMS_TO_TICKS(UPLOAD_TIMEOUT_MS)) )
            {
                upload_abort("timeout");
            }

            //
            // Poll more slowly while idle. 10 ms is ample for an event like "the
            // user pressed upload"; once a transfer starts the branch below
            // shortens the period.
            //
            vTaskDelay(pdMS_TO_TICKS(10));
            continue;
        }

        //
        // Do not sleep while transferring: the RTT down buffer is small, and
        // sleeping means the host waits for us, dropping overall throughput by an
        // order of magnitude. taskYIELD() is enough to give up the CPU -- this
        // task has the lowest priority, so anyone who wants to run gets in.
        //
        taskYIELD();
    }
}

//*****************************************************************************
//
// Whether a transfer is in progress.
//
//*****************************************************************************
bool
AssetUploadIsActive(void)
{
    return g_bActive;
}

#endif // WATCHFACE_ASSET_UPLOAD
