#!/usr/bin/env python3
"""Prototype of the gate proposed for tubamp (docs/research/fx/gate/report.md, R1-R4) and a
re-implementation of the TONE3000 gate from its decompiled constants (evidence/tone3000_gate.md).
  proto.py <tag> <thr_dB> <sigpy_dir> [key=value ...]     writes out/<tag>_<signal>.wav for every test signal
keys (proposed design): model=new|t3k  hyst=6 hold=0.02 rel=0.08 att=0.001 range=-90 slope=3 hp=90 win=0.012 dec=0.003
"""
import sys, math, numpy as np
tag, thr = sys.argv[1], float(sys.argv[2]); sys.path.insert(0, sys.argv[3]); import sig
P = dict(model='new', hyst=6.0, hold=0.02, rel=0.08, att=0.0005, range=-90.0, slope=3.0, hp=90.0, win=0.012, dec=0.003)
for kv in sys.argv[4:]:
    k, v = kv.split('='); P[k] = v if k == 'model' else float(v)

def svf_hp(x, fc, sr, k=math.sqrt(2)):
    g = math.tan(math.pi*fc/sr); a1 = 1/(1+g*(g+k)); a2 = g*a1; a3 = g*a2
    ic1 = ic2 = 0.0; y = np.empty_like(x)
    for i, v0 in enumerate(x):
        v3 = v0-ic2; v1 = a1*ic1+a2*v3; v2 = ic2+a2*ic1+a3*v3
        ic1 = 2*v1-ic1; ic2 = 2*v2-ic2; y[i] = v0-k*v1-v2
    return y

def gate_new(x, sr):
    sc = svf_hp(x, P['hp'], sr) if P['hp'] > 0 else x
    a = np.abs(sc)
    openT = 10**(thr/20); closeT = openT*10**(-P['hyst']/20)
    nwin = int(P['win']*sr); kdec = math.exp(-1/(P['dec']*sr))
    nhold = int(P['hold']*sr)
    up = 60.0/(P['att']*sr); down = 60.0/(P['rel']*sr)       # dB per sample
    rng = P['range']; k = P['slope']-1.0
    pk = 0.0; cnt = 0; st = 0; hc = 0; gr = rng; g = np.empty_like(x)
    for i in range(len(x)):
        v = a[i]
        if v >= pk: pk = v; cnt = nwin
        elif cnt > 0: cnt -= 1
        else: pk = pk*kdec + v*(1-kdec)
        if pk >= openT: st = 1; hc = nhold
        elif st == 1:
            if pk >= closeT: hc = nhold
            elif hc > 0: hc -= 1
            else: st = 0
        if st == 1: tgt = 0.0
        elif k > 0 and pk > 1e-9: tgt = min(0.0, max(rng, k*20*math.log10(pk/closeT)))
        else: tgt = rng
        if tgt > gr: gr = min(tgt, gr+up)
        else: gr = max(tgt, gr-down)
        g[i] = 10**(gr/20)
    return x*g

def gate_t3k(x, sr):
    c = lambda t: 1-math.exp(-1/(sr*t))
    cea, cer, co, cc = c(0.0002), c(0.025), c(0.0002), c(0.1); nhold = int(sr*0.05)
    hp = svf_hp(x, 80.0, sr); clp = 1-math.exp(-2*math.pi*5000/sr)
    openT = 10**(thr/20); closeT = 10**((thr-5)/20)
    lp = env = 0.0; st = 0; n = 0; gg = 0.0; y = np.empty_like(x)
    for i in range(len(x)):
        lp += clp*(hp[i]-lp); a = abs(lp)
        env += (cea if a > env else cer)*(a-env)
        tgt = 1.0
        if st == 0:
            if env >= openT: st = 1
            else: tgt = max((env/openT)**3, 1e-4)
        elif st == 1:
            if env < closeT: st = 2; n = nhold
        else:
            if env >= closeT: st = 1
            else:
                n -= 1
                if n <= 0: st = 0; tgt = max((env/openT)**3, 1e-4)
        gg += (co if tgt > gg else cc)*(tgt-gg)
        y[i] = x[i]*gg
    return y

for s in ('decay', 'rise', 'burst', 'burstlf', 'hover', 'chug', 'sustain'):
    x, sr = sig.read_wav(f'sig/{s}.wav'); x = x[0]
    y = gate_t3k(x, sr) if P['model'] == 't3k' else gate_new(x, sr)
    sig.write_wav(f'out/{tag}_{s}.wav', y.astype(np.float32), sr)
