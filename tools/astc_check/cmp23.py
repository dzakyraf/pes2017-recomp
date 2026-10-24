import numpy as np, collections
bc=np.fromfile('bc23/blocks.bc',dtype=np.uint8).reshape(-1,16)
kind=np.fromfile('bc23/kinds',dtype=np.uint8)
dec=np.fromfile('bc23/decoded.rgba',dtype=np.uint8).reshape(1024,1024,4)
def exp565(c):
    r=(c>>11)&31; g=(c>>5)&63; b=c&31
    return np.array([(r<<3)|(r>>2),(g<<2)|(g>>4),(b<<3)|(b>>2)])
names=['bc3-random','bc3-smooth','bc3-binary','bc2-random','bc2-smooth','bc2-binary']
st=collections.defaultdict(lambda:{'n':0,'rgb':[],'a':[]})
for b in range(len(bc)):
    p=bc[b]; k=kind[b]; bc3=k<3
    if bc3:
        a0,a1=int(p[0]),int(p[1]); pal=[a0,a1]
        if a0>a1: pal+= [((7-i)*a0+i*a1)//7 for i in range(1,7)]
        else: pal+=[(4*a0+a1)//5,(3*a0+2*a1)//5,(2*a0+3*a1)//5,(a0+4*a1)//5,0,255]
        bits=int.from_bytes(bytes(p[2:8]),'little'); al=[pal[(bits>>(3*i))&7] for i in range(16)]
    else:
        al=[((int(p[i>>1])>>((i&1)*4))&15)*17 for i in range(16)]
    c0=int(p[8])|int(p[9])<<8; c1=int(p[10])|int(p[11])<<8
    idx=int.from_bytes(bytes(p[12:16]),'little')
    e0,e1=exp565(c0),exp565(c1)
    pal4=[e0,e1,(2*e0+e1)//3,(e0+2*e1)//3]
    bx,by=b%256,b//256
    s=st[k]; s['n']+=1
    wr=0; wa=0
    for i in range(16):
        px=dec[by*4+i//4,bx*4+i%4].astype(int)
        wr=max(wr,int(np.abs(px[:3]-pal4[(idx>>(2*i))&3]).max()))
        wa=max(wa,abs(px[3]-al[i]))
    s['rgb'].append(wr); s['a'].append(wa)
for k in range(6):
    s=st[k]; r=np.array(s['rgb']); a=np.array(s['a'])
    print(f"{names[k]:11s} n={s['n']:6d}  rgb err: max {r.max():3d} p95 {int(np.percentile(r,95)):3d} | alpha err: max {a.max():3d} median {int(np.median(a)):3d} p95 {int(np.percentile(a,95)):3d} blocks>8: {(a>8).mean()*100:5.1f}%")
