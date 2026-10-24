# gen_matchplay.py out.macro: 60 s of live-match input on the touch slot (4), deterministic.
# Dribble in a direction (sometimes sprinting), then pass (A), through ball (Y), or shoot (B);
# lob/cross (X) now and then. Directions vary so play moves around the pitch.
import math, random, sys
A, B, X, Y = 0x1000, 0x2000, 0x4000, 0x8000
rng = random.Random(17)
ev, t = [], 500
def put(ms, buttons=0, rt=0, lx=0.0, ly=0.0):
    ev.append(f"{ms} 4 {buttons} 0 {rt} {lx:.3f} {ly:.3f} 0.0 0.0")
while t < 59000:
    ang = rng.uniform(-math.pi, math.pi)
    # lean toward the two goal directions (left/right on the broadcast camera)
    if rng.random() < 0.6: ang = rng.choice([0.0, math.pi]) + rng.uniform(-0.7, 0.7)
    lx, ly = math.cos(ang), math.sin(ang)
    sprint = 255 if rng.random() < 0.4 else 0
    run = rng.randint(700, 2200)
    put(t, 0, sprint, lx, ly); t += run
    r = rng.random()
    button = A if r < 0.55 else Y if r < 0.75 else B if r < 0.88 else X
    hold = 450 if button == B else rng.randint(90, 160)      # a shot needs some power
    put(t, button, sprint, lx, ly); t += hold
    put(t, 0, 0, lx * 0.5, ly * 0.5); t += rng.randint(250, 700)
put(t, 0, 0, 0.0, 0.0)
print(f"# pes17-macro v1, {len(ev)} events, {t + 500} ms")
print("\n".join(ev)); print(f"# end {t + 500}")
