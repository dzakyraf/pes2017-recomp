import numpy as np, collections
bc=np.fromfile('blocks.bc1',dtype=np.uint8).reshape(-1,8)
dec=np.fromfile('decoded.rgba',dtype=np.uint8).reshape(1024,1024,4)
def exp565(c):
    r=(c>>11)&31; g=(c>>5)&63; b=c&31
    return np.array([(r<<3)|(r>>2),(g<<2)|(g>>4),(b<<3)|(b>>2)])
stats=collections.defaultdict(lambda:[0,0,0,0])  # blocks, worstRGB, alphaBad, over3 blocks
for b in range(len(bc)):
    p=bc[b]; c0=int(p[0])|int(p[1])<<8; c1=int(p[2])|int(p[3])<<8
    idx=int.from_bytes(bytes(p[4:8]),'little')
    e0,e1=exp565(c0),exp565(c1)
    if c0>c1: pal=[(e0,255),(e1,255),((2*e0+e1)//3,255),((e0+2*e1)//3,255)]; cat='4col'
    else: pal=[(e0,255),(e1,255),((e0+e1)//2,255),(np.zeros(3,int),0)]; cat='3col'
    used3=any(((idx>>(2*i))&3)==3 for i in range(16))
    if c0<=c1: cat+= '-transp' if used3 else '-opaque'
    bx,by=b%256,b//256
    s=stats[cat]; s[0]+=1
    worst=0; abad=0
    for i in range(16):
        col,a=pal[(idx>>(2*i))&3]
        px=dec[by*4+i//4,bx*4+i%4]
        if a==0:
            abad+= px[3]>8
        else:
            abad+= px[3]<247
            worst=max(worst,int(np.abs(px[:3].astype(int)-col).max()))
    s[1]=max(s[1],worst); s[2]+=abad; s[3]+= worst>3
for k,v in sorted(stats.items()): print(f'{k:14s} blocks {v[0]:6d} worst RGB err {v[1]:3d}  alpha mismatches {v[2]:6d}  blocks >3 levels {v[3]}')

print("--- 3-colour blocks by whether the midpoint line fits")
def fits(e0,e1):
    d=e1-e0; sg=np.where(d>=0,1,-1)
    far=e0+(3*d+sg)//2 if False else e0+np.trunc((3*d+sg)/2).astype(int)
    near=e0-np.trunc((d+sg)/2).astype(int)
    return bool(((far>=0)&(far<=255)).all()), bool(((near>=0)&(near<=255)).all())
res=collections.defaultdict(list)
for b in range(len(bc)):
    p=bc[b]; c0=int(p[0])|int(p[1])<<8; c1=int(p[2])|int(p[3])<<8
    if c0>c1 or c0==c1: continue
    idx=int.from_bytes(bytes(p[4:8]),'little')
    e0,e1=exp565(c0),exp565(c1)
    fa,fb=fits(e0,e1)
    pal=[e0,e1,(e0+e1)//2,None]
    worst=0
    bx,by=b%256,b//256
    for i in range(16):
        k=(idx>>(2*i))&3
        if k==3: continue
        px=dec[by*4+i//4,bx*4+i%4]
        worst=max(worst,int(np.abs(px[:3].astype(int)-pal[k]).max()))
    res[(fa or fb)].append(worst)
for k,v in res.items():
    v=np.array(v); print('fits' if k else 'no-fit', len(v), 'median',np.median(v),'p95',np.percentile(v,95),'max',v.max())
