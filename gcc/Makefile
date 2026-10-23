#******************************************************************************
#
# Makefile - Rules for building the libraries, examples and docs.
#
# Copyright (c) 2026, Ambiq Micro, Inc.
# All rights reserved.
#
# Redistribution and use in source and binary forms, with or without
# modification, are permitted provided that the following conditions are met:
#
# 1. Redistributions of source code must retain the above copyright notice,
# this list of conditions and the following disclaimer.
#
# 2. Redistributions in binary form must reproduce the above copyright
# notice, this list of conditions and the following disclaimer in the
# documentation and/or other materials provided with the distribution.
#
# 3. Neither the name of the copyright holder nor the names of its
# contributors may be used to endorse or promote products derived from this
# software without specific prior written permission.
#
# Third party software included in this distribution is subject to the
# additional license terms as defined in the /docs/licenses directory.
#
# THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
# AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
# IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
# ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE
# LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
# CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
# SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
# INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
# CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
# ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
# POSSIBILITY OF SUCH DAMAGE.
#
# This is part of revision release_sdk5p2p0-66487dd10 of the AmbiqSuite Development Package.
#
#******************************************************************************
TARGET := nemagfx_watchface
COMPILERNAME := gcc
PROJECT := nemagfx_watchface_gcc
CONFIG := bin
SHELL:=/bin/bash

#******************************************************************************
#
# Location of the AmbiqSuite tree.
#
# This example normally sits at
#     <AmbiqSuite>/boards/<board>/examples/graphics/<example>/gcc
# which puts the SDK root six levels up. That is the first candidate below.
#
# The root is searched for instead of assumed, so the example also builds from
# one or two levels deeper -- a clone placed inside another directory, for
# instance. The test is a path that exists only at a real SDK root.
#
# Override either on the command line when the tree lives elsewhere:
#     make SDK_ROOT=/c/AmbiqSuite_5.2.0
#
#******************************************************************************
SDK_ROOT_CANDIDATES := ../../../../../.. ../../../../../../.. ../../../../../../../..

SDK_ROOT ?= $(firstword $(foreach d,$(SDK_ROOT_CANDIDATES),\
                $(if $(wildcard $(d)/mcu/apollo510/hal),$(d))))

ifeq ($(strip $(SDK_ROOT)),)
    $(error Cannot locate the AmbiqSuite root. Pass it explicitly, e.g. make SDK_ROOT=/c/AmbiqSuite_5.2.0)
endif

# The root that was found is made absolute before anything uses it.
#
# Windows rejects any path longer than 260 characters, and GCC does not collapse
# the ".." segments before opening a file -- it hands Windows the current
# directory and the whole relative path joined together. Once the tree sits a
# couple of directories deeper than usual, the longer header names tip over the
# limit and GCC reports
#
#     fatal error: nema_interpolators.h: No such file or directory
#
# for a file that is plainly there. An absolute root keeps every path far below
# the limit however deep the example sits. (MSYS2 rewrites /c/... into C:/...
# for GCC, and make reads the C:/... paths GCC writes into the .d files.)
#
# make cannot cope with a space in a path, so a tree under such a directory keeps
# the relative form. A root given on the command line is used as it is.
ifeq ($(origin SDK_ROOT),file)
    ifeq ($(words $(CURDIR)),1)
        SDK_ROOT := $(abspath $(SDK_ROOT))
    endif
endif

BSP_ROOT ?= $(SDK_ROOT)/boards/apollo510b_evb/bsp

ifeq ($(wildcard $(BSP_ROOT)/am_bsp.h),)
    $(error Cannot locate the board BSP at $(BSP_ROOT). Pass BSP_ROOT=<path to boards/apollo510b_evb/bsp>)
endif


#******************************************************************************
#
# Temporary directory for the compiler.
#
# GCC writes its intermediate files into $TMP. On Windows this is the single
# most common build failure: MSYS2 make often passes no TMP through at all,
# GCC falls back to C:\Windows\, and the error reads
#
#     Cannot create temporary file in C:\Windows\: Permission denied
#
# which looks like a compile error rather than an environment problem.
#
# Whatever the environment already provides is preferred; otherwise this
# directory is used, which is writable by definition. Note that GCC is a native
# Windows executable, so an MSYS-style path such as /tmp does not work here --
# it has to be something Windows can resolve.
#
#******************************************************************************
ifeq ($(strip $(TMP)),)
    ifneq ($(strip $(TEMP)),)
        TMP := $(TEMP)
    else
        TMP := $(CURDIR)
    endif
endif
export TMP


# Enable printing explicit commands with 'make VERBOSE=1'
ifneq ($(VERBOSE),1)
Q:=@
endif

#### Setup ####

TOOLCHAIN ?= arm-none-eabi
PART = apollo510
CPU = cortex-m55
FPU = auto
# Default to FPU hardware calling convention.  However, some customers and/or
# applications may need the software calling convention.
#FABI = softfp
FABI = hard

LINKER_FILE := ./linker_script.ld
STARTUP_FILE := ./startup_$(COMPILERNAME).c

#### Required Executables ####
CC = $(TOOLCHAIN)-gcc
GCC = $(TOOLCHAIN)-gcc
CPP = $(TOOLCHAIN)-cpp
LD = $(TOOLCHAIN)-ld
CP = $(TOOLCHAIN)-objcopy
OD = $(TOOLCHAIN)-objdump
RD = $(TOOLCHAIN)-readelf
AR = $(TOOLCHAIN)-ar
SIZE = $(TOOLCHAIN)-size
RM = $(shell which rm 2>/dev/null)

EXECUTABLES = CC LD CP OD AR RD SIZE GCC
K := $(foreach exec,$(EXECUTABLES),\
		$(if $(shell which $($(exec)) 2>/dev/null),,\
		$(info $(exec) not found on PATH ($($(exec))).)$(exec)))
$(if $(strip $(value K)),$(info Required Program(s) $(strip $(value K)) not found))

ifneq ($(strip $(value K)),)
all clean:
	$(info Tools $(TOOLCHAIN)-$(COMPILERNAME) not installed.)
	$(RM) -rf bin
else

DEFINES = -DPART_$(PART)
DEFINES+= -DAM_FREERTOS
DEFINES+= -DAM_PACKAGE_BGA
DEFINES+= -DAM_PART_APOLLO510
DEFINES+= -DLD_LIBRARY_PATH=$(SDK_ROOT)/third_party/ThinkSi/NemaGFX_SDK/lib
DEFINES+= -DNEMAGFX_SDK_PATH=$(SDK_ROOT)/third_party/ThinkSi/NemaGFX_SDK
DEFINES+= -DNEMA_PLATFORM=apollo510_nemagfx
# VMEM_SIZE is the NemaGFX graphics pool, allocated in SHARED_SRAM.
# The linker script gives SHARED_SRAM the full 3 MB the part has, matching every
# other graphics example on this board.  2 MB of that goes to the graphics pool;
# the rest covers other AM_SHARED_RW data (the touch driver DMA TCB buffer) and
# leaves headroom for further UI pages.
#
# Three watch faces plus the page-overview thumbnail share that pool.  Nominal
# pixel data (w*h*bpp -- see the warning below, this is NOT the real figure):
#
#   frame buffer   468x468 RGB565    428 KB
#   overview thumb 264x264 RGB565    136 KB   (BTN0 page overview)
#   home image     454x454 RGB565    403 KB
#   face 0 chrono  454x454 + hands   426 KB
#   face 1 ambiq   454x454 + hands   458 KB   (+ a 46x46 centre cap)
#   face 2 numbers 454x454 + hands   450 KB
#
# 2.5 MB was not quite enough.  With the Ambiq face in place, every texture
# loaded EXCEPT the very last entry in render_assets_load()'s table -- face3's
# second hand, 15,336 B -- so the shortfall was under 15 KB.  The symptom was
# subtle in a way worth remembering: the blue "numbers" face simply had no
# second hand, and nothing else looked wrong.
#
# The pool is therefore 2.625 MB (0x2A0000), which clears that 15 KB with about
# 113 KB to spare so the next small asset does not immediately break it again.
#
# SHARED_SRAM is 3 MB.  Everything in it other than the pool is small
# AM_SHARED_RW buffers (the touch driver's DMA TCB, the FatFs/SDIO structures
# in asset_store.c) totalling under 5 KB, so roughly 388 KB remains spare after
# this increase -- but see below before assuming a fourth face would fit.
#
# THE AMBIQ FACE REPLACED aurora (a 390x390 dial, 338 KB nominal); it was not
# added as a fourth face.  A fourth face does not fit, and that attempt is
# worth writing down because none of it shows up at build time:
#
#   1. Do not budget with w*h*bpp.  nema_texture_size() is what actually gets
#      allocated and it is larger; measuring showed the textures consuming at
#      least 250 KB more than the nominal column above.  The pool is a NOLOAD
#      array, so a too-small pool links cleanly and only fails at run time.
#   2. texture_load_from_store() used to allocate before reading and not free
#      the buffer when the read failed.  A face whose .BIN files were not on
#      the eMMC yet still consumed its full ~460 KB.  Fixed -- it now frees on
#      the failure path -- but the lesson stands: allocation happens whether or
#      not the asset exists.
#   3. A failed frame buffer allocation used to be silent and then fatal: the
#      first draw bound the GPU's destination to a null address and the fault
#      took the whole system down, so the symptom was a dead board (no display
#      AND no BLE LED) rather than a missing UI.  Fixed -- RenderTask now says
#      why and exits, leaving BLE and the sensors running.
#
# The runtime log is the source of truth for how full the pool is:
#
#   [render] textures: N loaded (X B), M failed
#   [render] framebuffer alloc FAILED (X B) -- graphics pool exhausted
DEFINES+= -DVMEM_SIZE=0x2A0000
DEFINES+= -Dapollo510b_evb
DEFINES+= -Dgcc
#
# Factory provisioning of the graphics assets.
#
#   1 = the home image, the dials, the hands and the three map tiles are built into
#       the firmware too, and any file missing from eMMC is written out at boot.
#       Use this setting for the first flash of a blank board.
#   0 = the firmware carries no assets at all and MRAM drops by 2.16 MB; the assets
#       must already be on eMMC (written by an earlier provisioning run, or copied
#       there by the user).
#
# The drawing path is identical either way -- both read from eMMC. The only
# difference is whether that seed data is present in the firmware.
#
DEFINES+= -DWATCHFACE_ASSET_PROVISION=0
#
# Set to 1 only when swapping artwork: unconditionally overwrite assets already on eMMC.
#
# Normal provisioning only fills in files that are "missing or the wrong length", and
# new and old images usually have the same dimensions (map tiles are all 468x468
# RGB565), so a matching length skips them -- without this a new image is never written.
#
# It only has meaning with WATCHFACE_ASSET_PROVISION=1, and must be set back to 0
# afterwards: left at 1, every boot rewrites 2 MB, slowing the boot and burning eMMC
# write cycles for nothing.
#
DEFINES+= -DWATCHFACE_ASSET_FORCE_WRITE=0
#
# Write assets into eMMC from the PC over RTT (see src/asset_upload.h and tools/asset_upload.py).
#
# Production firmware can turn it off: it lets anyone who can attach SWD rewrite the contents of eMMC.
#
DEFINES+= -DWATCHFACE_ASSET_UPLOAD=1
#
# RTT down buffer (host -> target).
#
# The default is only 16 bytes, meant for someone typing in a terminal. Transferring
# 2 MB of assets that way, the host has to wait for the target to drain every 16 bytes
# and the throughput is unusable. 4 KB lets the host write almost continuously.
DEFINES+= -DBUFFER_SIZE_DOWN=4096
#
# RTT up buffer (target -> host). A separate block of memory from the down buffer.
#
# The default is 1 KB. Upload flow control relies on the target printing one
# `[upload] ack` line per 1 KB received (see src/asset_upload.c), and RTT is in
# NO_BLOCK_SKIP mode -- a full buffer drops that line. A 412 KB file needs over four
# hundred ack lines competing with the render and BLE logs inside 1 KB, so losing one
# is close to certain, and one lost line stalls the transfer (the target has a resend
# mechanism as a backstop, but it should not have to step in every time).
#
DEFINES+= -DBUFFER_SIZE_UP=4096
#
# Route am_util_stdio_printf()/am_util_debug_printf() to SEGGER RTT channel 0.
# Comment out AM_USE_RTT_LOG to fall back to ITM/SWO output instead.
#
DEFINES+= -DAM_DEBUG_PRINTF
DEFINES+= -DAM_USE_RTT_LOG
#
# --- BLE (Cordio / EM9305) ---
# CORDIO_ENABLED    enables the Cordio BLE host stack.
# SEC_ECC_CFG       ECC is computed by the controller (SEC_ECC_CFG_HCI).
# WSF_TRACE_ENABLED WSF diagnostic messages. They share RTT channel 0 with the render
#                   log; comment this line out if the output volume affects the FPS.
#
DEFINES+= -DCORDIO_ENABLED=1
DEFINES+= -DRPMSG_BUFFER_SIZE=288
DEFINES+= -DSEC_ECC_CFG=SEC_ECC_CFG_HCI
#
# WSF_TRACE_ENABLED must stay OFF.
#
# Measured: with it on, DM reset completion moves from t=1062 ms to t=11067 ms -- a
# full 10 seconds slower. Cordio produces a great deal of trace during DM reset, and
# am_util_stdio_printf() vsprintf-formats every line before writing it to RTT;
# RadioTask has the highest priority and does not yield during printf, which serializes
# and stretches the whole reset. The knock-on effect is advertising starting late and
# the status LED starting to blink 10 seconds late (the appearance time of
# TMPS_LED_TIMER_IND = evt 162 confirms it).
#
# On top of that, am_util_stdio_printf() shares a single static buffer (g_prfbuf) with
# no lock, so with trace on RadioTask overwrites what other tasks are formatting --
# measurement showed even this file's own timestamp output truncated, making the log
# less trustworthy rather than more.
#
# For BLE diagnosis, use the [ble] / [ccc] messages hts_main.c prints directly with
# am_util_stdio_printf() -- they are unaffected by this switch.
#
#DEFINES+= -DWSF_TRACE_ENABLED

INCLUDES = -I$(SDK_ROOT)
INCLUDES+= -I$(SDK_ROOT)/CMSIS/ARM/Include
INCLUDES+= -I$(SDK_ROOT)/CMSIS/AmbiqMicro/Include
INCLUDES+= -I$(SDK_ROOT)/devices
INCLUDES+= -I$(SDK_ROOT)/mcu/apollo510
INCLUDES+= -I$(SDK_ROOT)/mcu/apollo510/hal
INCLUDES+= -I$(SDK_ROOT)/mcu/apollo510/hal/mcu
INCLUDES+= -I$(SDK_ROOT)/third_party/FreeRTOSv10.5.1/Source/include
INCLUDES+= -I$(SDK_ROOT)/third_party/FreeRTOSv10.5.1/Source/portable/GCC/AMapollo5
INCLUDES+= -I$(SDK_ROOT)/third_party/ThinkSi/NemaGFX_SDK/NemaDC
INCLUDES+= -I$(SDK_ROOT)/third_party/ThinkSi/NemaGFX_SDK/NemaGFX
INCLUDES+= -I$(SDK_ROOT)/third_party/ThinkSi/NemaGFX_SDK/NemaGFX/Nema
INCLUDES+= -I$(SDK_ROOT)/third_party/ThinkSi/NemaGFX_SDK/common/mem
INCLUDES+= -I$(SDK_ROOT)/third_party/ThinkSi/NemaGFX_SDK/include/tsi/NemaDC
INCLUDES+= -I$(SDK_ROOT)/third_party/ThinkSi/NemaGFX_SDK/include/tsi/NemaGFX
INCLUDES+= -I$(SDK_ROOT)/third_party/ThinkSi/NemaGFX_SDK/include/tsi/NemaVG
INCLUDES+= -I$(SDK_ROOT)/third_party/ThinkSi/NemaGFX_SDK/include/tsi/common
INCLUDES+= -I$(SDK_ROOT)/third_party/ThinkSi/config/apollo510_nemagfx
INCLUDES+= -I$(SDK_ROOT)/utils
INCLUDES+= -I$(BSP_ROOT)
INCLUDES+= -I../src
INCLUDES+= -I../src/generated
INCLUDES+= -I../src/font
#
# FatFs and its eMMC driver: the graphics assets live on a FAT filesystem on eMMC
# (see src/asset_store.h). mmc_apollo5 is FatFs's diskio implementation and contains
# the SDIO0 initialization and ISR as well, so none of that has to be written here.
#
INCLUDES+= -I$(SDK_ROOT)/third_party/FatFs/source
INCLUDES+= -I$(SDK_ROOT)/third_party/FatFs/apollo_driver/mmc_apollo5
INCLUDES+= -I$(SDK_ROOT)/third_party/SEGGER/RTT/Config
INCLUDES+= -I$(SDK_ROOT)/third_party/SEGGER/RTT/RTT
#
# --- BLE (Cordio / WSF / uECC) ---
#
INCLUDES+= -I$(SDK_ROOT)/third_party/cordio/ble-host/include
INCLUDES+= -I$(SDK_ROOT)/third_party/cordio/ble-host/sources/hci/ambiq
INCLUDES+= -I$(SDK_ROOT)/third_party/cordio/ble-host/sources/hci/ambiq/em9305
INCLUDES+= -I$(SDK_ROOT)/third_party/cordio/ble-host/sources/sec/common
INCLUDES+= -I$(SDK_ROOT)/third_party/cordio/ble-host/sources/stack/att
INCLUDES+= -I$(SDK_ROOT)/third_party/cordio/ble-host/sources/stack/cfg
INCLUDES+= -I$(SDK_ROOT)/third_party/cordio/ble-host/sources/stack/dm
INCLUDES+= -I$(SDK_ROOT)/third_party/cordio/ble-host/sources/stack/hci
INCLUDES+= -I$(SDK_ROOT)/third_party/cordio/ble-host/sources/stack/l2c
INCLUDES+= -I$(SDK_ROOT)/third_party/cordio/ble-host/sources/stack/smp
INCLUDES+= -I$(SDK_ROOT)/third_party/cordio/ble-profiles/include/app
INCLUDES+= -I$(SDK_ROOT)/third_party/cordio/ble-profiles/sources/apps
INCLUDES+= -I$(SDK_ROOT)/third_party/cordio/ble-profiles/sources/apps/app
INCLUDES+= -I$(SDK_ROOT)/third_party/cordio/ble-profiles/sources/apps/fit
INCLUDES+= -I$(SDK_ROOT)/third_party/cordio/ble-profiles/sources/profiles
INCLUDES+= -I$(SDK_ROOT)/third_party/cordio/ble-profiles/sources/profiles/bas
INCLUDES+= -I$(SDK_ROOT)/third_party/cordio/ble-profiles/sources/profiles/gap
INCLUDES+= -I$(SDK_ROOT)/third_party/cordio/ble-profiles/sources/profiles/gatt
INCLUDES+= -I$(SDK_ROOT)/third_party/cordio/ble-profiles/sources/profiles/hid
INCLUDES+= -I$(SDK_ROOT)/third_party/cordio/ble-profiles/sources/profiles/htps
INCLUDES+= -I$(SDK_ROOT)/third_party/cordio/ble-profiles/sources/profiles/include
INCLUDES+= -I$(SDK_ROOT)/third_party/cordio/ble-profiles/sources/profiles/uribeacon
INCLUDES+= -I$(SDK_ROOT)/third_party/cordio/ble-profiles/sources/services
INCLUDES+= -I$(SDK_ROOT)/third_party/cordio/wsf/include
INCLUDES+= -I$(SDK_ROOT)/third_party/cordio/wsf/sources
INCLUDES+= -I$(SDK_ROOT)/third_party/cordio/wsf/sources/port/freertos
INCLUDES+= -I$(SDK_ROOT)/third_party/cordio/wsf/sources/util
INCLUDES+= -I$(SDK_ROOT)/third_party/uecc

VPATH = $(SDK_ROOT)/CMSIS/AmbiqMicro/Source
VPATH+=:$(SDK_ROOT)/devices
VPATH+=:$(SDK_ROOT)/third_party/FreeRTOSv10.5.1/Source
VPATH+=:$(SDK_ROOT)/third_party/FreeRTOSv10.5.1/Source/portable/GCC/AMapollo5
VPATH+=:$(SDK_ROOT)/third_party/FreeRTOSv10.5.1/Source/portable/MemMang
VPATH+=:$(SDK_ROOT)/third_party/ThinkSi/config/apollo510_nemagfx
VPATH+=:$(SDK_ROOT)/utils
VPATH+=:../src
VPATH+=:../src/font
VPATH+=:$(SDK_ROOT)/third_party/FatFs/source
VPATH+=:$(SDK_ROOT)/third_party/FatFs/apollo_driver/mmc_apollo5
VPATH+=:$(SDK_ROOT)/third_party/SEGGER/RTT/RTT
#
# --- BLE (Cordio / WSF / uECC) ---
#
VPATH+=:$(SDK_ROOT)/third_party/cordio/ble-host/sources/hci/ambiq
VPATH+=:$(SDK_ROOT)/third_party/cordio/ble-host/sources/hci/ambiq/em9305
VPATH+=:$(SDK_ROOT)/third_party/cordio/ble-host/sources/sec/common
VPATH+=:$(SDK_ROOT)/third_party/cordio/ble-host/sources/sec/uecc
VPATH+=:$(SDK_ROOT)/third_party/cordio/ble-host/sources/stack/att
VPATH+=:$(SDK_ROOT)/third_party/cordio/ble-host/sources/stack/cfg
VPATH+=:$(SDK_ROOT)/third_party/cordio/ble-host/sources/stack/dm
VPATH+=:$(SDK_ROOT)/third_party/cordio/ble-host/sources/stack/hci
VPATH+=:$(SDK_ROOT)/third_party/cordio/ble-host/sources/stack/l2c
VPATH+=:$(SDK_ROOT)/third_party/cordio/ble-host/sources/stack/smp
VPATH+=:$(SDK_ROOT)/third_party/cordio/ble-profiles/sources/apps/app
VPATH+=:$(SDK_ROOT)/third_party/cordio/ble-profiles/sources/apps/app/common
VPATH+=:$(SDK_ROOT)/third_party/cordio/ble-profiles/sources/apps/fit
VPATH+=:$(SDK_ROOT)/third_party/cordio/ble-profiles/sources/apps/hidapp
VPATH+=:$(SDK_ROOT)/third_party/cordio/ble-profiles/sources/profiles/bas
VPATH+=:$(SDK_ROOT)/third_party/cordio/ble-profiles/sources/profiles/gap
VPATH+=:$(SDK_ROOT)/third_party/cordio/ble-profiles/sources/profiles/gatt
VPATH+=:$(SDK_ROOT)/third_party/cordio/ble-profiles/sources/profiles/hid
VPATH+=:$(SDK_ROOT)/third_party/cordio/ble-profiles/sources/profiles/htps
VPATH+=:$(SDK_ROOT)/third_party/cordio/ble-profiles/sources/services
VPATH+=:$(SDK_ROOT)/third_party/cordio/wsf/sources/port/freertos
VPATH+=:$(SDK_ROOT)/third_party/cordio/wsf/sources/util
VPATH+=:$(SDK_ROOT)/third_party/uecc

SRC = am_devices_chsc5816_ap5.c
#
# The vector font. DejaVuSans_ttf_kern.c is a TTF conversion for NemaVG and replaces
# the DejaVuSerif24pt4b.c bitmap font; with vectors the bitmap font no longer has to
# be moved into SSRAM, and the include in render_task.c went with it.
#
SRC += DejaVuSans_ttf_kern.c
SRC += am_devices_display_generic.c
SRC += am_resources.c
SRC += backlight.c
SRC += am_util_delay.c
SRC += am_util_stdio.c
SRC += display_task.c
SRC += event_groups.c
SRC += heap_4.c
SRC += list.c
SRC += nema_dc_hal.c
SRC += nema_event.c
SRC += nema_hal.c
SRC += nema_utils.c
SRC += nemagfx_watchface.c
SRC += port.c
SRC += port_stimer.c
SRC += port_systick_stimer.c
SRC += portasm.c
SRC += queue.c
SRC += render_task.c
SRC += rtos.c
SRC += sensor_task.c
SRC += map_task.c
#
# The asset storage layer: a FAT filesystem on eMMC. ff.c/ffunicode.c are FatFs
# itself, diskio.c its abstraction layer, and mmc_apollo5.c this board's eMMC
# implementation (including the SDIO0 initialization and ISR).
#
SRC += asset_store.c
SRC += asset_provision.c
SRC += asset_upload.c
SRC += ff.c
SRC += ffunicode.c
SRC += diskio.c
SRC += mmc_apollo5.c
#
# psram.c pulls am_devices_mspi_psram_aps25616ba_1p2v.c into its own translation unit
# with #include (the reason is in that file's comments), so that driver is not listed
# here -- listing it would define the same set of symbols twice.
#
SRC += psram.c
SRC += rtt_log.c
SRC += SEGGER_RTT.c
SRC += SEGGER_RTT_printf.c
SRC += tasks.c
SRC += touch.c
SRC += touch_task.c
SRC += tmp119.c
SRC += timers.c
SRC += startup_gcc.c
SRC += system_apollo510.c
#
# --- The NemaGFX UI Designer generated page (page 4) ---
#
# SRC / VPATH / INCLUDES are all maintained by the generated fragment, so adding
# images or swapping assets in the Designer needs no edit to this file -- just re-run
# Generate C. The fragment also declares the weak ui_event_*(), whose implementations
# stay in render_task.c.
#
# It has to come before CSRC is expanded below.
#
include ../src/generated/ui_designer/ui_designer.mk
#
# --- BLE application (Cordio HTS + TMP119) ---
#
SRC += app_hw_tmp119.c
SRC += ble_led.c
SRC += hts_main.c
SRC += radio_task.c
SRC += ble_diag.c
SRC += ble_pair.c
SRC += led_task.c
#
# --- BLE stack (Cordio profiles / host / WSF / uECC) ---
#
SRC += am_devices_button.c
SRC += am_devices_em9305.c
SRC += am_devices_led.c
SRC += am_util_debug.c
SRC += am_util_faultisr.c
SRC += am_util_id.c
SRC += app_db.c
SRC += app_disc.c
SRC += app_main.c
SRC += app_master.c
SRC += app_master_ae.c
SRC += app_master_leg.c
SRC += app_server.c
SRC += app_slave.c
SRC += app_slave_ae.c
SRC += app_slave_leg.c
SRC += app_terminal.c
SRC += app_ui.c
SRC += att_eatt.c
SRC += att_main.c
SRC += att_uuid.c
SRC += attc_disc.c
SRC += attc_eatt.c
SRC += attc_main.c
SRC += attc_proc.c
SRC += attc_read.c
SRC += attc_sign.c
SRC += attc_write.c
SRC += atts_ccc.c
SRC += atts_csf.c
SRC += atts_dyn.c
SRC += atts_eatt.c
SRC += atts_ind.c
SRC += atts_main.c
SRC += atts_proc.c
SRC += atts_read.c
SRC += atts_sign.c
SRC += atts_write.c
SRC += bas_main.c
SRC += bda.c
SRC += bstream.c
SRC += calc128.c
SRC += cfg_stack.c
SRC += crc32.c
SRC += dm_adv.c
SRC += dm_adv_ae.c
SRC += dm_adv_leg.c
SRC += dm_bis_master.c
SRC += dm_bis_slave.c
SRC += dm_cis.c
SRC += dm_cis_master.c
SRC += dm_cis_slave.c
SRC += dm_cis_sm.c
SRC += dm_conn.c
SRC += dm_conn_cte.c
SRC += dm_conn_master.c
SRC += dm_conn_master_ae.c
SRC += dm_conn_master_leg.c
SRC += dm_conn_slave.c
SRC += dm_conn_slave_ae.c
SRC += dm_conn_slave_leg.c
SRC += dm_conn_sm.c
SRC += dm_dev.c
SRC += dm_dev_priv.c
SRC += dm_ead.c
SRC += dm_iso.c
SRC += dm_main.c
SRC += dm_past.c
SRC += dm_phy.c
SRC += dm_priv.c
SRC += dm_scan.c
SRC += dm_scan_ae.c
SRC += dm_scan_leg.c
SRC += dm_sec.c
SRC += dm_sec_lesc.c
SRC += dm_sec_master.c
SRC += dm_sec_slave.c
SRC += dm_sync_ae.c
SRC += gap_main.c
SRC += gatt_main.c
SRC += hci_cmd.c
SRC += hci_cmd_ae.c
SRC += hci_cmd_bis.c
SRC += hci_cmd_cis.c
SRC += hci_cmd_cte.c
SRC += hci_cmd_iso.c
SRC += hci_cmd_past.c
SRC += hci_cmd_phy.c
SRC += hci_core.c
SRC += hci_core_ps.c
SRC += hci_drv_em9305.c
SRC += hci_evt.c
SRC += hci_main.c
SRC += hci_tr.c
SRC += hci_vs_ae.c
SRC += hci_vs_em9305.c
SRC += hid_main.c
SRC += hidapp_main.c
SRC += htps_main.c
SRC += l2c_coc.c
SRC += l2c_main.c
SRC += l2c_master.c
SRC += l2c_slave.c
SRC += print.c
SRC += sec_aes.c
SRC += sec_aes_rev.c
SRC += sec_ccm_hci.c
SRC += sec_cmac_hci.c
SRC += sec_ecc.c
SRC += sec_ecc_debug.c
SRC += sec_ecc_hci.c
SRC += sec_main.c
SRC += smp_act.c
SRC += smp_db.c
SRC += smp_main.c
SRC += smp_non.c
SRC += smp_sc_act.c
SRC += smp_sc_main.c
SRC += smpi_act.c
SRC += smpi_sc_act.c
SRC += smpi_sc_sm.c
SRC += smpi_sm.c
SRC += smpr_act.c
SRC += smpr_sc_act.c
SRC += smpr_sc_sm.c
SRC += smpr_sm.c
SRC += svc_alert.c
SRC += svc_batt.c
SRC += svc_bps.c
SRC += svc_core.c
SRC += svc_cps.c
SRC += svc_cscs.c
SRC += svc_cte.c
SRC += svc_dis.c
SRC += svc_gls.c
SRC += svc_gyro.c
SRC += svc_hid.c
SRC += svc_hts.c
SRC += svc_ipss.c
SRC += svc_plxs.c
SRC += svc_px.c
SRC += svc_scpss.c
SRC += svc_temp.c
SRC += svc_time.c
SRC += svc_uricfg.c
SRC += svc_wdxs.c
SRC += svc_wp.c
SRC += svc_wss.c
SRC += terminal.c
SRC += uECC.c
SRC += uECC_ll.c
SRC += ui_console.c
SRC += ui_lcd.c
SRC += ui_main.c
SRC += ui_platform.c
SRC += ui_timer.c
SRC += wsf_assert.c
SRC += wsf_buf.c
SRC += wsf_efs.c
SRC += wsf_math.c
SRC += wsf_msg.c
SRC += wsf_os.c
SRC += wsf_queue.c
SRC += wsf_timer.c
SRC += wsf_trace.c
SRC += wstr.c

CSRC = $(filter %.c,$(SRC))
ASRC = $(filter %.s,$(SRC))
CPPSRC = $(filter %.cpp,$(SRC))

OBJS = $(CSRC:%.c=$(CONFIG)/%.o)
OBJS+= $(ASRC:%.s=$(CONFIG)/%.o)
OBJS+= $(CPPSRC:%.cpp=$(CONFIG)/%.o)

DEPS = $(CSRC:%.c=$(CONFIG)/%.d)
DEPS+= $(ASRC:%.s=$(CONFIG)/%.d)
DEPS+= $(CPPSRC:%.cpp=$(CONFIG)/%.d)

LIBS = $(SDK_ROOT)/mcu/apollo510/hal/mcu/gcc/bin/libam_hal.a
LIBS += $(SDK_ROOT)/third_party/ThinkSi/config/apollo510_nemagfx/gcc/bin/lib_nema_apollo510_nemagfx.a
LIBS += $(BSP_ROOT)/gcc/bin/libam_bsp.a

# Compiler cflags
CFLAGS = -mthumb -mcpu=$(CPU) -mfpu=$(FPU) -mfloat-abi=$(FABI)
CFLAGS+= -ffunction-sections -fdata-sections -fomit-frame-pointer
CFLAGS+= -fno-common -fno-exceptions -fstack-protector
CFLAGS+= -MMD -MP -std=c99 -Wall -g
CFLAGS+= -Wimplicit-fallthrough -Wundef -Wpointer-arith
CFLAGS+= -Wshadow -Wredundant-decls -Wstrict-prototypes
CFLAGS+= -Wno-sign-compare -Wno-unknown-pragmas -Wno-psabi
CFLAGS+= -O0
CFLAGS+= $(DEFINES)
CFLAGS+= $(INCLUDES)
CFLAGS+= 

# Compiler cxxflags
CXXFLAGS = -mthumb -mcpu=$(CPU) -mfpu=$(FPU) -mfloat-abi=$(FABI)
CXXFLAGS+= -ffunction-sections -fdata-sections -fomit-frame-pointer
CXXFLAGS+= -fno-common -fno-exceptions -fstack-protector -fno-use-cxa-atexit
CXXFLAGS+= -MMD -MP -std=c++11 -Wall -g
CXXFLAGS+= -Wimplicit-fallthrough -Wundef -Wpointer-arith
CXXFLAGS+= -Wshadow -Wredundant-decls
CXXFLAGS+= -Wno-sign-compare -Wno-unknown-pragmas -Wno-psabi
CXXFLAGS+= -O0
CXXFLAGS+= $(DEFINES)
CXXFLAGS+= $(INCLUDES)
CXXFLAGS+= 

LFLAGS = -mthumb -mcpu=$(CPU) -mfpu=$(FPU) -mfloat-abi=$(FABI)
LFLAGS+= -nostartfiles -static
LFLAGS+= -Wl,--gc-sections,--entry,Reset_Handler,-Map,$(CONFIG)/$(TARGET).map
LFLAGS+= -Wl,--start-group -lm -lc -lgcc         -lstdc++ $(LIBS) -Wl,--end-group
#
# --- Interception for the BLE diagnostics (src/ble_diag.c) ---
#
# GNU ld's --wrap is used to tap Cordio's HCI send and receive path. Not one line of
# source under third_party needs changing; these are the five symbols intercepted:
#
#   hciTrSendCmd          defined in hci_tr.c,  called from hci_cmd.c
#   hciTrSerialRxIncoming defined in hci_tr.c,  called from hci_drv_em9305
#   hciCmdRecvCmpl        defined in hci_cmd.c, called from hci_evt.c
#   hciCmdTimeout         defined in hci_cmd.c, called from hci_core_ps.c
#   HciDrvRadioBoot       defined in hci_drv_em9305.c, called from radio_task.c / hci_cmd.c
#
# Setting WATCHFACE_BLE_DIAG in ble_diag.h to 0 turns the diagnostics themselves off,
# but these lines have to stay -- removing them makes the linker complain that
# __wrap_* is an undefined reference.
#
LFLAGS+= -Wl,--wrap=hciTrSendCmd
LFLAGS+= -Wl,--wrap=hciTrSerialRxIncoming
LFLAGS+= -Wl,--wrap=hciCmdRecvCmpl
LFLAGS+= -Wl,--wrap=hciCmdTimeout
LFLAGS+= -Wl,--wrap=HciDrvRadioBoot
LFLAGS+= 

# Additional user specified CFLAGS
CFLAGS+=$(EXTRA_CFLAGS)

CPFLAGS = -Obinary

ODFLAGS = -S

#### Rules ####
all: directories $(CONFIG)/$(TARGET).bin

ifeq ($(OS),Windows_NT)
    JLINK ?= JLink.exe
else
    UNAME_S := $(shell uname -s)
    ifeq ($(UNAME_S),Linux)
        JLINK ?= JLinkExe
    else ifeq ($(UNAME_S),Darwin)
        JLINK ?= JLinkExe
    else ifneq ($(filter MINGW% MSYS% CYGWIN%,$(UNAME_S)),)
        # MSYS2 / MinGW / Cygwin shell on Windows
        JLINK ?= JLink.exe
    else
        $(error OS not support)
    endif
endif

# JLink.exe is not on PATH by default on Windows; fall back to the newest
# SEGGER install found under Program Files if a bare 'JLink.exe' won't resolve.
ifeq ($(shell which "$(JLINK)" 2>/dev/null),)
JLINK_FALLBACK := $(shell ls -1 "/c/Program Files/SEGGER"/JLink*/JLink.exe \
                                "/c/Program Files (x86)/SEGGER"/JLink*/JLink.exe \
                                2>/dev/null | sort -V | tail -1)
ifneq ($(JLINK_FALLBACK),)
JLINK := $(JLINK_FALLBACK)
endif
endif

.PHONY: flash
flash: all
	@echo " Flashing with $(JLINK)"
	$(Q) "$(JLINK)" -nogui 1 -commanderscript flash.jlink

# Recover the target after a debugger session wedged the GPU power domain
# (firmware hard-faults in nema_sys_init and plain reset does not help).
# See poi.jlink for the full explanation.
.PHONY: recover
recover:
	@echo " Sending software POI with $(JLINK)"
	$(Q) "$(JLINK)" -nogui 1 -commanderscript poi.jlink

# Intel HEX output (some flashing tools prefer .hex over raw .bin)
.PHONY: hex
hex: $(CONFIG)/$(TARGET).hex

$(CONFIG)/$(TARGET).hex: $(CONFIG)/$(TARGET).axf
	@echo " Generating $(COMPILERNAME) $@..."
	$(Q) $(CP) -O ihex $< $@

.PHONY: ozone
ozone: all
	ozone oz.jdebug &

directories: $(CONFIG)

$(CONFIG):
	@mkdir -p $@

# Every object also depends on this makefile.
#
# The generated .d files track header dependencies only, so a change to DEFINES
# here rebuilt nothing and the link silently reused stale objects.  That is not
# a theoretical problem: raising VMEM_SIZE left nema_hal.o holding the old
# `tsi_buffer[VMEM_SIZE/8]`, so the graphics pool stayed at its previous size
# and the newest watch face failed to allocate at run time with no build-time
# complaint at all.  The linker cannot catch this either -- a too-small pool
# links fine.
#
# The cost is that editing this file rebuilds everything, which is the correct
# behaviour for a file that carries the compiler flags.
MAKEFILE_DEPS := $(MAKEFILE_LIST)

$(CONFIG)/%.o: %.c $(CONFIG)/%.d $(MAKEFILE_DEPS)
	@echo " Compiling $(COMPILERNAME) $<"
	$(Q) $(CC) -c $(CFLAGS) $< -o $@

$(CONFIG)/%.o: %.cpp $(CONFIG)/%.d $(MAKEFILE_DEPS)
	@echo " Compiling $(COMPILERNAME) $<"
	$(Q) $(CC) -c $(CXXFLAGS) $< -o $@

$(CONFIG)/%.o: %.s $(CONFIG)/%.d $(MAKEFILE_DEPS)
	@echo " Assembling $(COMPILERNAME) $<"
	$(Q) $(CC) -c $(CFLAGS) $< -o $@

$(CONFIG)/$(TARGET).axf: $(OBJS) $(LIBS)
	@echo " Linking $(COMPILERNAME) $@"
	$(Q) $(CC) -Wl,-T,$(LINKER_FILE) -o $@ $(OBJS) $(LFLAGS)

$(CONFIG)/$(TARGET).bin: $(CONFIG)/$(TARGET).axf
	@echo " Copying $(COMPILERNAME) $@..."
	$(Q) $(CP) $(CPFLAGS) $< $@
	$(Q) $(OD) $(ODFLAGS) $< > $(CONFIG)/$(TARGET).lst
	$(Q) $(SIZE) $(OBJS) $(LIBS) $(CONFIG)/$(TARGET).axf >$(CONFIG)/$(TARGET).size

clean:
	@echo "Cleaning..."
	$(Q) $(RM) -rf $(CONFIG)
$(CONFIG)/%.d: ;

$(SDK_ROOT)/mcu/apollo510/hal/mcu/gcc/bin/libam_hal.a:
	$(MAKE) -C $(SDK_ROOT)/mcu/apollo510/hal/mcu

$(BSP_ROOT)/gcc/bin/libam_bsp.a:
	$(MAKE) -C $(BSP_ROOT)

# Automatically include any generated dependencies
-include $(DEPS)
endif
.PHONY: all clean directories

