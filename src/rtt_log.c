//*****************************************************************************
//
//! @file rtt_log.c
//! @brief SEGGER RTT transport for Ambiq am_util_stdio_printf().
//
//*****************************************************************************

#include "rtt_log.h"

#include <stdint.h>

#include "am_util_stdio.h"
#include "SEGGER_RTT.h"

//
// Ambiq stdio supplies complete, null-terminated string fragments here.
//
static void
rtt_stdio_print(char *pcString)
{
    if (pcString != NULL)
    {
        SEGGER_RTT_WriteString(0, pcString);
    }
}

void
rtt_log_init(void)
{
    SEGGER_RTT_Init();

    //
    // Never stall the real-time render task if the host is disconnected.
    // TRIM preserves as much of a long message as the current buffer permits.
    //
    SEGGER_RTT_SetFlagsUpBuffer(0, SEGGER_RTT_MODE_NO_BLOCK_TRIM);

    am_util_stdio_printf_init(rtt_stdio_print);
    SEGGER_RTT_WriteString(0, "\r\n[RTT] nemagfx_watchface log ready\r\n");
}
