# State-based A/B of env settings on a device: tools/device_ab.sh base PES17_X=1 base PES17_X=1
# DEVICE=<adb serial> MACRO=<macro name> (default play) SCRATCH=<dir for stream.txt>
S=${SCRATCH:-/tmp/pes17_ab}; mkdir -p $S
D=${DEVICE:?set DEVICE to the adb serial}; MACRO=${MACRO:-play}; B=com.pes2017.recomp; F=/sdcard/Android/data/$B/files
for cfg in "$@"; do
  for attempt in 1 2 3; do
    if [ "$cfg" = base ]; then adb -s $D shell "rm -f $F/env_override"; else adb -s $D shell "echo $cfg > $F/env_override"; fi
    adb -s $D shell am force-stop $B; sleep 2; adb -s $D shell am start -n $B/.MainActivity >/dev/null
    for i in $(seq 180); do adb -s $D exec-out "tail -n 300 $F/game.log" 2>/dev/null | grep -q PreStartup && break; sleep 1; done; sleep 2
    python3 - $S $D $F <<'PY' &
import subprocess,sys,time,re
S,D,F=sys.argv[1:4]
p=subprocess.Popen(['adb','-s',D,'exec-out',f'tail -n 0 -f {F}/game.log'],stdout=subprocess.PIPE,text=True,errors='replace',bufsize=1)
t0=time.time(); out=open(S+'/stream.txt','w')
for line in p.stdout:
    if re.search(r'tele:|procedure:',line): out.write(f'{time.time()-t0:7.1f} {line[:150]}'); out.flush()
    if time.time()-t0>190: break
p.kill()
PY
    sleep 1; adb -s $D shell am broadcast -a com.pes2017.recomp.MACRO -p $B --es cmd play --es name $MACRO >/dev/null
    ok=0; for i in $(seq 150); do awk '$1>5 && /Match\/MatchIdle/{f=1} END{exit !f}' $S/stream.txt 2>/dev/null && { ok=1; break; }; sleep 1; done
    if [ $ok = 1 ]; then sleep 68; fi
    pkill -f "tail -n 0 -f" ; wait
    [ $ok = 1 ] && break
  done
  python3 - $S/stream.txt "$cfg" $ok <<'PY'
import re,sys,statistics
idle=None;setup=None;rows=[]
for l in open(sys.argv[1],errors='replace'):
    t=float(l[:7])
    if 'Match/MatchSetup' in l and setup is None and t>5: setup=t
    if 'Match/MatchIdle' in l and idle is None and t>5: idle=t
    m=re.search(r'tele: fps (\d+) worst (\d+)',l)
    if m: rows.append((t,int(m.group(1)),int(m.group(2))))
if idle is None: print(sys.argv[2],'never reached the match'); sys.exit()
def stat(lo,hi):
    w=sorted(f for t,f,_ in rows if idle+lo<=t<=idle+hi)
    return f'mean {sum(w)/len(w):.1f} p10 {w[len(w)//10]} min {w[0]} sd {statistics.pstdev(w):.1f}' if w else 'n/a'
print(f'{sys.argv[2]:20s} setup@{setup:.0f}s idle@{idle:.0f}s | intro (0-25s after idle): {stat(0,25)} | match (25-65s): {stat(25,65)}')
PY
done
adb -s $D shell "rm -f $F/env_override"; adb -s $D shell am force-stop $B
