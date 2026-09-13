#!/bin/zsh
# usage: flash_hold.sh <label> [KBD_REST_IDLE_TICKS]   (omit ticks = the Makefile default)
# Bench environment (env-overridable; defaults are the 2026-09-13 bench):
#   OPENKEYBOARD_QMK, OPENDONGLE_TOOL, MINICHLINK, MRS_TOOLCHAIN, OPENBOOT_TOOLCHAIN, KBD_PROBE,
#   BENCH_BIN (a directory of PATH shims: GNU make + `stat -c%s`, see the memory notes)
S=${BENCH_DIR:-$(cd "$(dirname "$0")" && pwd)}
REPO=$(cd "$(dirname "$0")/../../.." && pwd)
export PATH=/opt/homebrew/opt/make/libexec/gnubin:${BENCH_BIN:-$S/bin}:$PATH
Q=${OPENKEYBOARD_QMK:-$HOME/Development/openkeyboard/qmk_firmware}; E=$Q/.build/handwired_opencontroller_bench_oracle.elf
MRS=${MRS_TOOLCHAIN:-"/Applications/MounRiver Studio 2.app/Contents/Resources/app/resources/darwin/components/WCH/Toolchain/RISC-V Embedded GCC15/bin"}
OB=${OPENBOOT_TOOLCHAIN:-"/Applications/MounRiver Studio 2.app/Contents/Resources/app/resources/darwin/components/WCH/Toolchain/RISC-V Embedded GCC12/bin"}
MC=${MINICHLINK:-$HOME/Development/WCH/ch32fun/minichlink/minichlink}
DT=${OPENDONGLE_TOOL:-$HOME/Development/openkeyboard/OpenDongle/tools/target/release/opendongle}
KBD_PROBE=${KBD_PROBE:-CF148F065446}
cd "$REPO/firmware" || exit 1
X=""; [ -n "$2" ] && X="-DKBD_REST_IDLE_TICKS=${2}u"
echo "== build $1 EXTRA_CFLAGS='$X'"
make factory MRS_TOOLCHAIN="$MRS" OPENBOOT_TOOLCHAIN="$OB" MINICHLINK="$MC" EXTRA_CFLAGS="$X" 2>&1 | grep -E "rf_task|error|Error|factory|\.bin" | tail -5 || exit 1
python3 $Q/keyboards/handwired/opencontroller_bench/bench.py --elf "$E" tap 0 60 >/dev/null 2>&1   # input -> driver unlocks the module for 20 s
sleep 0.5; python3 $Q/keyboards/handwired/opencontroller_bench/bench.py --elf "$E" raw A6 56 >/dev/null 2>&1   # belt and braces: autosleep off
( for i in 1 2 3 4 5 6 7 8 9 10 11 12; do python3 $Q/keyboards/handwired/opencontroller_bench/bench.py --elf "$E" tap 0 60 >/dev/null 2>&1; sleep 0.6; done ) &   # hold the link CONNECTED (every key restarts the rest timer) while make starts up and attaches
HOLDPID=$!
make flash-factory KBD_PROBE=$KBD_PROBE ALLOW_BONDED_FLASH=1 MRS_TOOLCHAIN="$MRS" OPENBOOT_TOOLCHAIN="$OB" MINICHLINK="$MC" EXTRA_CFLAGS="$X" 2>&1 | grep -iE "flash|bond|error|fail|verif|ok" | tail -6
wait $HOLDPID 2>/dev/null; sleep 5
python3 $Q/keyboards/handwired/opencontroller_bench/bench.py --elf "$E" tap 0 60 >/dev/null 2>&1; sleep 1.5
echo "== post-flash: dongle: $($DT --status 2>/dev/null | tail -1 | grep -oE 'connection=[a-z ]*')  host: $(python3 $Q/keyboards/handwired/opencontroller_bench/bench.py --elf "$E" status 2>/dev/null | grep -E '^host')"
BIN=$REPO/firmware/build/opencontroller-ch592-slotA/opencontroller-ch592-factory.bin
echo "FLASH $1 ticks=${2:-default(KBD_REST_IDLE_MS)} image_sha256=$(shasum -a 256 $BIN | cut -c1-16) $(date '+%H:%M:%S')" | tee -a $S/hold_sweep.log
