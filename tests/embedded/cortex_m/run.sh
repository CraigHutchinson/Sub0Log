#!/bin/sh
# Builds firmware.cpp for one QEMU MPS2 board, runs it, and decodes the
# segment it wrote with the host sub0log-cat.
#
#   run.sh <board> <repo-root> <sub0log-cat> <work-dir> [opt]
#     board: an386 (Cortex-M4) | an505 (Cortex-M33) | an385 (Cortex-M3) | an500 (Cortex-M7)
#     opt:   optimisation flag, default -Os (firmware's usual choice)
#
# Exit status is non-zero if the build, the firmware's own checks, or the
# host-side decode fail.
set -eu
board=$1; root=$2; cat_tool=$3; work=$4; opt=${5:--Os}

case "$board" in
    an385) cpu=cortex-m3;  origin=0x00000000; define= ;;
    an386) cpu=cortex-m4;  origin=0x00000000; define= ;;
    an500) cpu=cortex-m7;  origin=0x00000000; define= ;;
    an505) cpu=cortex-m33; origin=0x10000000; define=-DSUB0LOG_BOARD_AN505 ;;
    *) echo "unknown board $board" >&2; exit 2 ;;
esac

mkdir -p "$work"
elf="$work/firmware-$board$opt.elf"
here=$(dirname "$0")

arm-none-eabi-g++ -std=c++23 -mcpu="$cpu" -mthumb -mfloat-abi=soft "$opt" \
    -fno-exceptions -fno-rtti -ffunction-sections -fdata-sections \
    -DSUB0LOG_PLATFORM_CUSTOM $define -I"$root/include" \
    --specs=rdimon.specs -nostartfiles -T "$here/link.ld" \
    -Wl,--defsym=RAM_ORIGIN="$origin" -Wl,--gc-sections \
    "$here/firmware.cpp" -o "$elf"
arm-none-eabi-size "$elf"

cd "$work"
rm -f cortex_m_segment.s0l
timeout 300 qemu-system-arm -M "mps2-$board" -nographic -monitor none -serial none \
    -semihosting-config enable=on,target=native -icount shift=0 -kernel "$elf"

"$cat_tool" --stats cortex_m_segment.s0l > decoded.txt
head -3 decoded.txt
grep -q 'boot 49374 -42 2.5' decoded.txt
grep -q 'rssi -71 on ch37' decoded.txt
grep -q 'frame abcdefghijklmnopqrstuvwxyz' decoded.txt
echo "cortex-m ($board $opt): host decode OK, $(grep -c 'tick ' decoded.txt) tick records"
