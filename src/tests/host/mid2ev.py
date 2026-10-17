import sys, struct
d=open(sys.argv[1],'rb').read()
assert d[:4]==b'MThd'
fmt,ntr,div=struct.unpack('>HHH',d[8:14])
p=14; evs=[]
def vlq(d,p):
    v=0
    while True:
        b=d[p]; p+=1; v=(v<<7)|(b&127)
        if b<128: return v,p
for tr in range(ntr):
    assert d[p:p+4]==b'MTrk'; ln=struct.unpack('>I',d[p+4:p+8])[0]; q=p+8; end=q+ln; p=end
    t=0; rs=0
    while q<end:
        dt,q=vlq(d,q); t+=dt; b=d[q]
        if b==0xFF:
            ty=d[q+1]; l,q2=vlq(d,q+2)
            if ty==0x51: evs.append((t,0,'tempo',int.from_bytes(d[q2:q2+3],'big')))
            q=q2+l; continue
        if b in (0xF0,0xF7):
            l,q2=vlq(d,q+1); q=q2+l; continue
        if b&0x80: rs=b; q+=1
        n=2 if (rs&0xF0) in (0xC0,0xD0) else 3
        data=[rs]+list(d[q:q+n-1]); q+=n-1
        evs.append((t,1,'m',data))
evs.sort(key=lambda e:(e[0],e[1]))
tempo=500000; last=0; ms=0.0
for t,_,k,v in evs:
    ms+= (t-last)*tempo/div/1000; last=t
    if k=='tempo': tempo=v
    else:
        v=v+[0]*(3-len(v)); print(int(ms), "%x %x %x"%tuple(v))
