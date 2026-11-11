#!/usr/bin/env python3
"""Gate analyses. an.py <mode> in.wav out.wav [args]
gain   : windowed gain (out RMS / in RMS) vs time and input level; prints TSV t, in_dB(peak-equiv), gain_dB  [--win N --hop N --every K]
static : for decay/rise ramps: gain_dB at each input level (5 dB grid)
thr    : first/last time gain crosses -3 dB and -20 dB, with the input level there
burst  : attack and release timing around level steps (HI/LO bursts): t(-1dB) after onset, t(-3/-6/-20/-40 dB) after offset
flips  : number of gain crossings of -6 dB (chatter count) and min/max gain
"""
import sys, numpy as np
sys.path.insert(0, sys.argv[-1])
import sig
mode, fin, fout = sys.argv[1:4]
x, sr = sig.read_wav(fin); y, _ = sig.read_wav(fout)
x = x[0]; y = y[0][:len(x)]
if len(y) < len(x): x = x[:len(y)]
import os
win = int(os.environ.get('WIN','48')); hop = max(1,win//4)
lat = int(os.environ.get('LAT','0'))
y = y[lat:]; x = x[:len(y)]
def env(v):
    c = np.concatenate([[0.0], np.cumsum(v*v)])
    idx = np.arange(0, len(v)-win, hop)
    return np.sqrt((c[idx+win]-c[idx])/win), (idx+win/2)/sr
ex, t = env(x); ey, _ = env(y)
indb = 20*np.log10(np.maximum(ex*np.sqrt(2), 1e-9))
g = 20*np.log10(np.maximum(ey, 1e-10)/np.maximum(ex, 1e-10))
valid = ex > 1e-7
if mode == 'gain':
    k = int(sys.argv[4]) if len(sys.argv) > 5 else 400
    for i in range(0, len(t), k):
        print(f'{t[i]:.4f}\t{indb[i]:.1f}\t{g[i]:.1f}')
elif mode == 'static':
    for lev in range(-10, -101, -5):
        m = valid & (np.abs(indb-lev) < 0.25)
        if m.any(): print(f'{lev}\t{np.median(g[m]):.1f}')
elif mode == 'thr':
    for th in (-1, -3, -6, -20, -40):
        m = valid & (g < th)
        if m.any():
            i0 = np.argmax(m); i1 = len(m)-1-np.argmax(m[::-1])
            print(f'gain<{th}dB first t={t[i0]:.3f}s in={indb[i0]:.1f}dB ; last t={t[i1]:.3f}s in={indb[i1]:.1f}dB')
        else: print(f'gain<{th}dB never')
elif mode == 'burst':
    def at(tt): return int(np.searchsorted(t, tt))
    res_on = []; res_off = []
    for ton in (1.0, 2.8, 4.6):
        a = at(ton); seg = g[a:at(ton+0.29)]
        r = [f'g_before={np.median(g[at(ton-0.2):at(ton-0.01)]):.1f}']
        for th in (-20, -6, -3, -1):
            j = np.argmax(seg > th); r.append(f'>{th}dB@{(t[a+j]-ton)*1000:.1f}ms' if seg[j] > th else f'>{th}dB never')
        res_on.append(' '.join(r))
    for toff in (1.3, 3.1, 4.9):
        b = at(toff); seg = g[b:at(toff+1.4)]
        r = []
        for th in (-1, -3, -6, -20, -40, -60):
            j = np.argmax(seg < th); r.append(f'<{th}dB@{(t[b+j]-toff)*1000:.1f}ms' if seg[j] < th else f'<{th}dB never')
        r.append(f'final={np.median(seg[-50:]):.1f}dB')
        res_off.append(' '.join(r))
    print('onset#1 ', res_on[0]); print('onset#3 ', res_on[2]); print('offset#1', res_off[0]); print('offset#3', res_off[2])
elif mode == 'flips':
    gg = g[valid & (t > 0.5) & (t < t[-1]-0.2)]; s = gg > -6
    print(f'crossings(-6dB)={int(np.sum(s[1:] != s[:-1]))} min={gg.min():.1f} max={gg.max():.1f} median={np.median(gg):.1f}')
elif mode == 'gaps':
    # chug.wav: noise-only gaps are the last 10 ms before each 150 ms chug onset, k=1..19; tail = 3.2..5 s
    lv=[]
    for k in range(1,20):
        a=int((k*0.150-0.010)*sr); b=int(k*0.150*sr)
        lv.append(10*np.log10(np.mean(y[a:b]**2)+1e-20)-10*np.log10(np.mean(x[a:b]**2)+1e-20))
    a=int(3.5*sr); b=int(4.9*sr)
    tail=10*np.log10(np.mean(y[a:b]**2)+1e-20)-10*np.log10(np.mean(x[a:b]**2)+1e-20)
    # attack loss: first 10 ms of chugs 5..15
    al=[]
    for k in range(5,15):
        a=int(k*0.150*sr); b=a+int(0.010*sr)
        al.append(10*np.log10(np.mean(y[a:b]**2)+1e-20)-10*np.log10(np.mean(x[a:b]**2)+1e-20))
    print(f'gap_gain_dB median={np.median(lv):.1f} min={min(lv):.1f} max={max(lv):.1f} ; onset10ms_gain_dB={np.median(al):.1f} ; tail_gain_dB={tail:.1f}')
elif mode == 'sus':
    # sustain.wav: note level -12 dBFS - 6 dB/s. Report gain at note levels and the time the gain passes -3/-20/-40
    for lev in (-20,-30,-36,-40,-44,-48,-52,-56,-60):
        tt=(-12-lev)/6.0; a=int((tt-0.05)*sr); b=int((tt+0.05)*sr)
        gg=10*np.log10(np.mean(y[a:b]**2)+1e-20)-10*np.log10(np.mean(x[a:b]**2)+1e-20)
        print(f'{lev}\t{gg:.1f}')
