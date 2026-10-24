#!/bin/bash
# One launch + macro + a fixed measuring window, with the numbers that say where the time went:
#   tools/ab_window.sh <label> [PES17_NAME=value ...]
# Needs ANDROID_SERIAL (adb serial; check `adb shell getprop ro.product.model` first -- two
# phones are used here and a build once went onto the wrong one) and files/macros/.remote_control.
#
#   MACRO=<name>   macro to replay (default play)
#   W0=<s>         seconds after playback starts when the window opens (default: after a 114 s macro)
#   MEASURE=<s>    window length (default 80)
#   SHOT=1         also take a screenshot a third of the way into the window
#   OUT=<dir>      where <label>.log/.mark/... go (default /tmp/pes17_ab)
#
# Prints: median fps of the 600-swap windows after W0, thermal state, per-thread CPU (from
# /proc/<pid>/task/*/stat: utime+stime, so the run-queue wait that schedstat gives is not there),
# GPU pass time and draws a frame. A run that never reached Match/MatchIdle or logged no fps
# is invalid: matchIdle=0 or fps=[] -- repeat it.
#
# Why a window and not the whole run: macros are time-based, the scene at the end differs from
# run to run (300-600 draws a frame apart), and heat skews later runs. Interleave configs
# (A B A B), repeat, and judge differences by scene-matched windows only (tools/crowd_windows.py
# does that for PES17_CROWD_ALTERNATE).
LABEL=${1:?label}; shift
PKG=com.pes2017.recomp
F=/sdcard/Android/data/$PKG/files
OUT=${OUT:-/tmp/pes17_ab}; mkdir -p "$OUT"
MEASURE=${MEASURE:-80}
W0=${W0:-125}
: "${ANDROID_SERIAL:?set ANDROID_SERIAL}"

adb shell "rm -f $F/env_override"
if [ $# -gt 0 ]; then
  for kv in "$@"; do echo "$kv"; done | adb shell "cat > $F/env_override"
fi
adb shell am force-stop $PKG
adb shell "rm -f $F/game.log"
adb logcat -c
adb shell am start -n $PKG/.MainActivity >/dev/null

for i in $(seq 1 60); do      # the guest reaching PreStartup, then a moment more
  sleep 2
  adb shell "grep -q 'procedure: PreStartup' $F/game.log" 2>/dev/null && break
done
sleep 3
adb shell am broadcast -a com.pes2017.recomp.MACRO -p $PKG --es cmd play --es name "${MACRO:-play}" >/dev/null
START=$(date +%s)
sleep_until() { while [ $(( $(date +%s) - START )) -lt "$1" ]; do sleep 1; done; }

sleep_until "$W0"
reached=$(adb shell "grep -c 'Match/MatchIdle' $F/game.log" | tr -d '\r')
echo "$(adb shell "wc -l < $F/game.log" | tr -d '\r ')" > "$OUT/$LABEL.mark"
GP=$(adb shell pidof $PKG:game | tr -d '\r ')       # the run lives in its own process
snap() { adb shell "for t in /proc/$GP/task/*; do echo \"\$(cat \$t/stat)\"; done"; }
snap > "$OUT/$LABEL.ss1"; SS1T=$(date +%s)
adb shell "for tid in \$(ls /proc/$GP/task); do n=\$(cat /proc/$GP/task/\$tid/comm); case \"\$n\" in pes17|'gpu ring consum'|'vulkan presente'|'guest thread 35') echo \"\$n \$(grep Cpus_allowed_list /proc/$GP/task/\$tid/status | cut -f2)\";; esac; done" > "$OUT/$LABEL.aff"
clk() { adb shell 'for c in 0 3 4 7; do printf "cpu$c=%s " $(cat /sys/devices/system/cpu/cpu$c/cpufreq/scaling_cur_freq); done'; }
sleep_until $((W0 + MEASURE / 3))
clk > "$OUT/$LABEL.clk1"
[ -n "$SHOT" ] && adb exec-out screencap -p > "$OUT/$LABEL.png"
sleep_until $((W0 + 2 * MEASURE / 3))
clk > "$OUT/$LABEL.clk2"
sleep_until $((W0 + MEASURE))
snap > "$OUT/$LABEL.ss2"; echo $(( $(date +%s) - SS1T )) > "$OUT/$LABEL.sst"
TEMP=$(adb shell dumpsys battery | awk '/temperature/{print $2/10}')
# The SoC's own sensor and the GPU's cooling level: Android's overall thermal status follows the
# skin and stays 0 while these say severe (X7 Pro: CPU/GPU 85-86 C, cooling level 27).
adb shell dumpsys thermalservice | awk '/Current temperatures from HAL/{f=1} f&&/mName=CPU/{print; exit}' | sed -E 's/.*mValue=([0-9.]+).*/\1/' > "$OUT/$LABEL.cputemp"
adb shell dumpsys thermalservice | grep -m1 "devfreq-13000000.mali" | sed -E 's/.*mValue=([0-9]+).*/\1/' > "$OUT/$LABEL.malicool"
adb shell cat $F/game.log > "$OUT/$LABEL.log"
adb logcat -d -b crash > "$OUT/$LABEL.crash"

python3 - "$LABEL" "$reached" "$TEMP" "$OUT" <<'EOF'
import re, sys, statistics as st
label, reached, temp, out = sys.argv[1:]
lines = open(f"{out}/{label}.log", errors="replace").read().splitlines()
idx = int(open(f"{out}/{label}.mark").read().strip())
fps, gpuwait = [], []
for l in lines[idx:]:
    m = re.search(r"perf: ([0-9.]+) fps .*?([0-9.]+) ms a frame waiting for the GPU", l)
    if m: fps.append(float(m.group(1))); gpuwait.append(float(m.group(2)))
crash = open(f"{out}/{label}.crash").read().strip()
rd = lambda ext: open(f"{out}/{label}.{ext}").read().strip()
print(f"{label}: matchIdle={reached} fps={fps} median={st.median(fps) if fps else None} "
      f"gpuwait={max(gpuwait) if gpuwait else None} temp={temp}C crash={'YES' if crash else 'no'}")
print(f"   clocks: {rd('clk1')} | {rd('clk2')}   cpu_die_C={rd('cputemp')} mali_cooling={rd('malicool')}")

def ss(p):                       # comm has spaces and parens: split at the last ')'
    d = {}
    for l in open(p, errors="replace"):
        l = l.rstrip()
        if ")" not in l: continue
        tid = l.split(" ", 1)[0]; comm = l[l.index("(") + 1:l.rindex(")")]
        f = l[l.rindex(")") + 2:].split()
        if len(f) > 12: d[tid] = (comm, (int(f[11]) + int(f[12])) * 10_000_000)   # 10 ms ticks, as ns
    return d
try:
    a, b = ss(f"{out}/{label}.ss1"), ss(f"{out}/{label}.ss2"); dt = float(rd("sst"))
    pct = lambda name: sum(b[t][1] - a[t][1] for t in b if t in a and b[t][0] == name) / 1e9 / dt * 100
    guest = sorted(((b[t][1] - a[t][1]) / 1e9 / dt * 100 for t in b if t in a and b[t][0].startswith("guest thread")), reverse=True)[:3]
    total = sum(b[t][1] - a[t][1] for t in b if t in a) / 1e9 / dt * 100
    print("   cpu%% of a core: main(pes17)=%.0f ring=%.0f recorder=%.0f presenter=%.0f guest_top3=%s process=%.0f"
          % (pct("pes17"), pct("gpu ring consum"), pct("recorder"), pct("vulkan presente"),
             "/".join(f"{g:.0f}" for g in guest), total))
except Exception as e:
    print("   cpu: n/a", e)
gp = [l for l in lines[idx:] if "gpu: " in l and "ms of pass time" in l]
print("   gpu passes:", gp[-1].split("gpu: ", 1)[1][:80] if gp else "none (add PES17_GPU_TIMING=1)")
dv = [int(m.group(1)) / 600 for l in lines[idx:] if (m := re.search(r"swaps: (\d+) draws", l))]
print("   draws/frame median: %.0f" % (st.median(dv) if dv else -1))
print("   affinity: " + "; ".join(rd("aff").split("\n")))
EOF
