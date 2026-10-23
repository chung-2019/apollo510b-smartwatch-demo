//*****************************************************************************
//
//! @file rtt_log.h
//! @brief SEGGER RTT transport for Ambiq am_util_stdio_printf().
//
//*****************************************************************************

#ifndef RTT_LOG_H
#define RTT_LOG_H

#ifdef __cplusplus
extern "C"
{
#endif

//*****************************************************************************
//
//! @brief Initialize RTT channel 0 and route Ambiq stdio messages to it.
//!
//! The RTT control block is placed in the default .bss section, which the
//! linker script maps to MCU_TCM.  TCM is not covered by the D-cache, so the
//! host debugger always observes coherent data without any cache maintenance.
//
//*****************************************************************************
void rtt_log_init(void);

#ifdef __cplusplus
}
#endif

#endif // RTT_LOG_H
