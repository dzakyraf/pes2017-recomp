#!/usr/bin/env bash
# Fresh launch to a COM vs COM match: League (New, English League, Arsenal), then
# "Forward Time > Proceed to Match in Coach Mode" -- the AI plays both sides, nobody
# holds a pad. Exhibition has no such mode; the League menu does.
#   tools/to_coach_match.sh        ends about 60 s into the match; screenshot /tmp/coach.png
# Timed with sleeps (the procedure log stops after 400 entries on long sessions).
cd "$(dirname "$0")/.."
PKG=com.pes2017.recomp; P=tools/pad.sh
$P clear
adb shell am force-stop $PKG; sleep 2
adb shell am start -n $PKG/com.pes2017.recomp.MainActivity >/dev/null
sleep 20
# title: START (twice, the first may only wake the demo), then wait for the top menu
for i in 1 2 3 4; do $P tap 0 START 0.15; sleep 3; $P tap 0 A 0.15; sleep 4; done
sleep 25
$P tap 0 RB; sleep 1.5; $P tap 0 RB; sleep 2.5          # COMPETITION tab
$P tap 0 DOWN; sleep 1.5; $P tap 0 A; sleep 5           # League -> New / Load popup
$P tap 0 A; sleep 12                                    # New -> "Select League." dialog
$P tap 0 A; sleep 2; $P tap 0 A; sleep 6                # OK, English League -> General Settings
$P tap 0 A; sleep 4                                     # OK -> "Select Team." dialog
$P tap 0 A; sleep 2; $P tap 0 A; sleep 5                # OK, Arsenal -> Coop settings
$P tap 0 A; sleep 12                                    # OK -> loading -> Matchday / Forward Time
$P tap 0 A; sleep 6                                     # (tips/next)
$P tap 0 DOWN; sleep 1; $P tap 0 A; sleep 15            # "Proceed to Match in Coach Mode"
$P tap 0 A; sleep 60                                    # Kick-off
adb exec-out screencap -p > /tmp/coach_full.png
python3 -c "
from PIL import Image
Image.open('/tmp/coach_full.png').resize((640, 288)).save('/tmp/coach.png')"
