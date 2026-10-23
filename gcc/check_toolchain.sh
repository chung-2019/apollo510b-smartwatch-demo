#!/usr/bin/env bash
#
# check_toolchain.sh -- Apollo510B EVB toolchain self-check
#
# Checks that make / arm-none-eabi-gcc / J-Link are ready, and in particular
# verifies that the installed J-Link supports the Apollo510 (AP510NFA-CBR).
#
# Usage (run from the gcc/ directory):  bash check_toolchain.sh
#

RC=0
ok()   { echo "  [ OK ] $*"; }
fail() { echo "  [FAIL] $*"; RC=1; }
warn() { echo "  [WARN] $*"; }

echo "=============================================="
echo " Apollo510B EVB toolchain check"
echo "=============================================="

# ---------------------------------------------------------------- make
echo
echo "-- GNU Make --"
if command -v make >/dev/null 2>&1; then
    ok "$(make --version | head -1)  ($(command -v make))"
else
    fail "make not found. Install MSYS2 and run: pacman -S make"
fi

# ------------------------------------------------------- ARM toolchain
echo
echo "-- Arm GNU Toolchain --"
if command -v arm-none-eabi-gcc >/dev/null 2>&1; then
    ok "$(arm-none-eabi-gcc --version | head -1)"
    if arm-none-eabi-gcc -mthumb -mcpu=cortex-m55 -mfpu=auto -mfloat-abi=hard \
                         -E -x c /dev/null -o /dev/null >/dev/null 2>&1; then
        ok "supports -mcpu=cortex-m55 (required by Apollo510)"
    else
        fail "this GCC does not support cortex-m55; upgrade the Arm GNU Toolchain"
    fi
else
    fail "arm-none-eabi-gcc not found; check that it is on PATH"
fi

# ------------------------------------------------- temp directory writable
echo
echo "-- temp directory --"
if [ -z "$TMP" ] && [ -z "$TEMP" ] && [ -z "$TMPDIR" ]; then
    warn "none of TMP/TEMP/TMPDIR is set; GCC may fall back to the unwritable C:\\Windows\\"
    warn "fix: export TMP=/tmp"
else
    ok "TMP=${TMP:-$TEMP}"
fi

# -------------------------------------------------------- dependent libraries
echo
echo "-- dependent libraries --"
for lib in \
    ../../../../../../mcu/apollo510/hal/mcu/gcc/bin/libam_hal.a \
    ../../../../../../third_party/ThinkSi/config/apollo510_nemagfx/gcc/bin/lib_nema_apollo510_nemagfx.a \
    ../../../../bsp/gcc/bin/libam_bsp.a
do
    if [ -f "$lib" ]; then
        ok "$(basename "$lib")"
    else
        warn "$(basename "$lib") not built yet (the first make builds it automatically)"
    fi
done

# ------------------------------------------------------------- J-Link
echo
echo "-- SEGGER J-Link --"
JLINK="$(command -v JLink.exe 2>/dev/null)"
if [ -z "$JLINK" ]; then
    JLINK="$(ls -1 "/c/Program Files/SEGGER"/JLink*/JLink.exe \
                   "/c/Program Files (x86)/SEGGER"/JLink*/JLink.exe \
                   2>/dev/null | sort -V | tail -1)"
fi

if [ -z "$JLINK" ]; then
    fail "JLink.exe not found; install SEGGER J-Link V8.40 or newer"
else
    ok "$JLINK"
    JLINK_DIR="$(dirname "$JLINK")"

    # The flash loader for the AP510NFA-CBR is built into JLinkARM.dll. Older versions
    # (V7.94, say) carry only the Apollo4 family, and connect fails with
    # "No valid device has been selected."
       grep -rq "AP510NFA-CBR" "$JLINK_DIR/JLinkDevices.xml" \
                               "$JLINK_DIR/Devices" 2>/dev/null; then
        ok "supports AP510NFA-CBR (Apollo510)"
    else
        fail "this J-Link does not support AP510NFA-CBR -- cannot flash an Apollo510"
        echo "         The installed version carries Apollo4 family devices only."
        echo "         Go to https://www.segger.com/downloads/jlink/"
        echo "         and install J-Link Software V8.40 or newer."
    fi
fi

echo
echo "=============================================="
if [ $RC -eq 0 ]; then
    echo " Result: toolchain ready; make all / make flash can be run"
else
    echo " Result: something did not pass (see [FAIL] above)"
fi
echo "=============================================="
exit $RC
