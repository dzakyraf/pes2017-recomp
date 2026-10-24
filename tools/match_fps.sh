#!/bin/bash
# tools/match_fps.sh <label> [PES17_X=v ...]: launch, replay MACRO (default macro2), wait for
# Match/MatchIdle in game.log (state, not clock), then +25 s, then 45 s of tele fps.
# Prints n/mean/median/min of the per-second fps and the SoC CPU temperature. Needs ANDROID_SERIAL.
# Run configs interleaved and repeated: one config varies about +/-7 fps median run to run.
LABEL=$1; shift
OUT=${OUT:-/tmp/pes17_ab}; mkdir -p $OUT
PKG=com.pes2017.recomp; F=/sdcard/Android/data/$PKG/files
adb shell "rm -f $F/env_override"
[ $# -gt 0 ] && for kv in "$@"; do echo "$kv"; done | adb shell "cat > $F/env_override"
adb shell am force-stop $PKG; adb shell "rm -f $F/game.log"
adb shell am start -n $PKG/.MainActivity >/dev/null
for i in $(seq 1 60); do sleep 2; adb shell "grep -q 'procedure: PreStartup' $F/game.log" 2>/dev/null && break; done
sleep 3
adb shell am broadcast -a com.pes2017.recomp.MACRO -p $PKG --es cmd play --es name "${MACRO:-macro2}" >/dev/null
T0=$(date +%s)
until adb shell "grep -q 'Match/MatchIdle' $F/game.log" 2>/dev/null; do sleep 2; [ $(( $(date +%s)-T0 )) -gt 240 ] && { echo "$LABEL: NO MATCHIDLE"; echo "DONE $LABEL"; exit 1; }; done
sleep 25
MARK=$(adb shell "wc -l < $F/game.log" | tr -d '\r ')
sleep 45
adb shell cat $F/game.log > $OUT/$LABEL.log
CT=$(adb shell dumpsys thermalservice | awk '/Current temperatures from HAL/{f=1} f&&/mName=CPU/{print; exit}' | sed -E 's/.*mValue=([0-9.]+).*/\1/')
tail -n +$MARK $OUT/$LABEL.log | python3 -c "
import re,sys,statistics as s
v=[int(m) for m in re.findall(r'tele: fps (\\d+)',sys.stdin.read())]
print('$LABEL: n=%d mean=%.1f median=%s min=%s cpuC=$CT' % (len(v), s.mean(v) if v else 0, s.median(v) if v else None, min(v) if v else None))"
echo "DONE $LABEL"
