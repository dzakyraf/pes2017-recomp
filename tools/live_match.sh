#!/bin/bash
# tools/live_match.sh <label> [PES17_X=v ...]: launch; macro3 to the match, stopped at Match/MatchIdle; intro untouched;
# matchplay looped from MatchIdle + ${PLAY_AT:-24} s; window ${W0:-45}..+45 s after MatchIdle; tele fps + screenshots.
# Needs ANDROID_SERIAL, files/macros/.remote_control, macro3 (menus to kick-off) and matchplay
# (python3 tools/gen_matchplay.py > matchplay.macro; adb push it to files/macros/). State-driven:
# the menu macro is stopped when Match/MatchIdle appears, so faster or slower loads do not shift it.
LABEL=$1; shift
OUT=${OUT:-/tmp/pes17_ab}; mkdir -p $OUT
PKG=com.pes2017.recomp; F=/sdcard/Android/data/$PKG/files
M() { adb shell am broadcast -a com.pes2017.recomp.MACRO -p $PKG --es cmd "$1" --es name "${2:-x}" ${3:+--ei loops $3} >/dev/null; }
adb shell "rm -f $F/env_override"
[ $# -gt 0 ] && for kv in "$@"; do echo "$kv"; done | adb shell "cat > $F/env_override"
adb shell am force-stop $PKG; adb shell "rm -f $F/game.log"
adb shell am start -n $PKG/.MainActivity >/dev/null
for i in $(seq 1 60); do sleep 2; adb shell "grep -q 'procedure: PreStartup' $F/game.log" 2>/dev/null && break; done
sleep 3; M play macro3; T0=$(date +%s)
until adb shell "grep -q 'Match/MatchIdle' $F/game.log" 2>/dev/null; do sleep 1; [ $(( $(date +%s)-T0 )) -gt 240 ] && { echo "$LABEL: NO MATCHIDLE"; M stopplay; exit 1; }; done
M stopplay; TI=$(date +%s)
at() { while [ $(( $(date +%s) - TI )) -lt "$1" ]; do sleep 1; done; }
at ${PLAY_AT:-24}; M play matchplay 0
at ${W0:-45}; MARK=$(adb shell "wc -l < $F/game.log" | tr -d '\r ')
at $(( ${W0:-45} + 15 )); [ -n "$SHOT" ] && adb exec-out screencap -p > $OUT/$LABEL.a.png
at $(( ${W0:-45} + 32 )); [ -n "$SHOT" ] && adb exec-out screencap -p > $OUT/$LABEL.b.png
at $(( ${W0:-45} + 45 )); M stopplay
adb shell cat $F/game.log > $OUT/$LABEL.log
CT=$(adb shell dumpsys thermalservice | awk '/Current temperatures from HAL/{f=1} f&&/mName=CPU/{print; exit}' | sed -E 's/.*mValue=([0-9.]+).*/\1/')
tail -n +$MARK $OUT/$LABEL.log | python3 -c "
import re,sys,statistics as s
v=[int(m) for m in re.findall(r'tele: fps (\d+)',sys.stdin.read())]
print('$LABEL: n=%d mean=%.1f median=%s min=%s cpuC=$CT' % (len(v), s.mean(v) if v else 0, s.median(v) if v else None, min(v) if v else None))"
echo "DONE $LABEL"
