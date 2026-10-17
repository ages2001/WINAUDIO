import numpy as np, sys
def ana(f, ch, freq, sr=48000):
    x=np.fromfile(f,'<i2').reshape(-1,2)[:,ch].astype(float)
    nz=np.nonzero(np.abs(x)>50)[0]
    x=x[nz[0]+4800:nz[-1]-4800]
    n=len(x); w=np.blackman(n); X=np.abs(np.fft.rfft(x*w)); fr=np.fft.rfftfreq(n,1/sr)
    k=np.argmax(X); pk=fr[k]
    sig=X[max(0,k-20):k+20]**2; tot=(X**2).sum()
    print(f, "ch",ch,"peak %.1f Hz amp %.0f"%(pk, np.max(np.abs(x))), "SNR %.1f dB"%(10*np.log10(sig.sum()/(tot-sig.sum()))), "len %.3f s"%((nz[-1]-nz[0])/sr))
ana('out-s44.raw',0,1000); ana('out-s44.raw',1,440); ana('out-m22.raw',0,1000); ana('out-f96.raw',0,3000)
