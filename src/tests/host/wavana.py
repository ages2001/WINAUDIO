import numpy as np, sys, wave
w=wave.open(sys.argv[1]); sr=w.getframerate(); n=w.getnframes(); ch=w.getnchannels()
x=np.frombuffer(w.readframes(n),'<i2').reshape(-1,ch).astype(float)
print("rate",sr,"ch",ch,"len %.2fs"%(n/sr))
# segments of activity (50ms windows)
win=sr//20; e=np.sqrt((x[:len(x)//win*win,0].reshape(-1,win)**2).mean(1))
er=np.sqrt((x[:len(x)//win*win,1].reshape(-1,win)**2).mean(1))
act=e>30
s=None
for i,a in enumerate(act):
    if a and s is None: s=i
    if (not a or i==len(act)-1) and s is not None:
        seg=x[s*win:i*win,0]
        if len(seg)>2048:
            X=np.abs(np.fft.rfft(seg*np.hanning(len(seg)))); f=np.fft.rfftfreq(len(seg),1/sr)
            pk=f[np.argmax(X)]
        else: pk=0
        print("active %.2f-%.2fs  rmsL %.0f rmsR %.0f peak %.0f  main %.1f Hz"%(s*win/sr,i*win/sr,e[s:i].mean(),er[s:i].mean(),np.abs(x[s*win:i*win]).max(),pk))
        s=None
# glitch detection: large sample-to-sample jumps
d=np.abs(np.diff(x[:,0])); print("max jump",d.max(), "jumps>8000:", (d>8000).sum())
