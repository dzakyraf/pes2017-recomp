#!/usr/bin/env python3
"""Scene-matched windows of one run made with PES17_CROWD_ALTERNATE=600 (the crowd flips every
600 frames), from tools/ab_window.sh's <label>.log and <label>.mark:

    python3 tools/crowd_windows.py /tmp/pes17_ab A2_alt [more labels...]

Each 600-swap window is a row: fps, process CPU a frame, draws recorded, passes, GPU pass time.
Compare ON and OFF only between neighbouring rows with the same draws a frame -- two separate
runs differ by 300-600 draws a frame in the scene alone, which swamps the crowd (about 34 draws
and 1.6 passes a frame). The run needs PES17_GPU_TIMING=1 for the GPU column.
A window is OFF when the report line shows more than 3000 skipped crowd draws.
"""
import re
import sys


def windows(log, mark):
    lines = open(log, errors="replace").read().splitlines()[mark:]
    rows, cur = [], {}
    for l in lines:
        m = re.search(r"perf: ([0-9.]+) fps over the last 600", l)
        if m:
            cur = {"fps": float(m.group(1))}
        m = re.search(r"perf: cpu ([0-9.]+) ms a frame \((\d+)% of a core\)", l)
        if m and cur:
            cur.update(cpu=float(m.group(1)), pct=int(m.group(2)))
        m = re.search(r"swaps: (\d+) draws \((\d+) recorded\).*?skipped:(.*)", l)
        if m and cur:
            c = re.search(r"crowd off x(\d+)", m.group(3))
            cur.update(draws=int(m.group(1)) / 600, rec=int(m.group(2)) / 600, skipped=int(c.group(1)) if c else 0)
        m = re.search(r"gpu: 600 frames, ([0-9.]+) passes and ([0-9.]+) ms of pass time", l)
        if m and cur and "draws" in cur:
            cur.update(passes=float(m.group(1)), gpu=float(m.group(2)))
            rows.append(cur)
            cur = {}
    return [r for r in rows if "cpu" in r]


def main():
    out, labels = sys.argv[1], sys.argv[2:]
    if not labels:
        sys.exit(__doc__)
    for label in labels:
        mark = int(open(f"{out}/{label}.mark").read())
        print(f"\n== {label}")
        print(" crowd   fps  cpu_ms/frame  cpu%  draws/f  rec/f  skipped  passes  gpu_ms")
        for r in windows(f"{out}/{label}.log", mark):
            state = "OFF" if r["skipped"] > 3000 else "on "
            print(f"  {state}  {r['fps']:5.1f} {r['cpu']:9.1f} {r['pct']:8d} {r['draws']:8.0f} {r['rec']:6.0f}"
                  f" {r['skipped']:8d} {r['passes']:7.1f} {r['gpu']:6.2f}")


if __name__ == "__main__":
    main()
