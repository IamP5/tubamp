#!/usr/bin/env python3
"""summ.py <name> [label-prefix]: one-line summaries of the evidence files in $W/ev/<name>."""
import os, sys, glob
W=os.environ["W"]; name=sys.argv[1]; pre=sys.argv[2] if len(sys.argv)>2 else ""
def rows(p):
    out=[]
    for l in open(p):
        if l.startswith("#") or not l.strip(): continue
        out.append(l.rstrip("\n").split("\t"))
    return out
def kv(p):
    d={}
    for r in rows(p):
        if len(r)>=2:
            try: d[r[0]]=float(r[1])
            except ValueError: pass
    return d
labels=sorted({os.path.basename(f).split(".")[0] for f in glob.glob(f"{W}/ev/{name}/{pre}*.tsv")})
for lab in labels:
    base=f"{W}/ev/{name}/{lab}"
    line=[lab]
    p=base+".curve.tsv"
    if os.path.exists(p):
        r=[[float(x) for x in row] for row in rows(p)[1:]]
        xin=[a[0] for a in r]; g=[a[2] for a in r]; out=[a[1] for a in r]
        g0=sorted(g[:5])[2]
        thr=None
        for i in range(1,len(r)):
            if g[i] < g0-1.0:
                # interpolate
                f=(g0-1.0-g[i-1])/(g[i]-g[i-1]) if g[i]!=g[i-1] else 0
                thr=xin[i-1]+f*(xin[i]-xin[i-1]); break
        slope_top=(out[-1]-out[-3])/(xin[-1]-xin[-3])
        slope_mid=(out[-4]-out[-6])/(xin[-4]-xin[-6])
        line.append(f"CURVE g0={g0:.1f} thr-1dB={thr if thr is None else round(thr,1)} GR@0={g[-1]-g0:.1f} GR@-12={g[-5]-g0:.1f} ratio_top={1/slope_top if abs(slope_top)>1e-3 else 999:.1f} ratio_mid={1/slope_mid if abs(slope_mid)>1e-3 else 999:.1f}")
    for t in ("tbA","tbB","tbL","tbF","tbFB"):
        p=f"{base}.{t}.tsv"
        if os.path.exists(p):
            d=kv(p)
            if "g_on_db" not in d: line.append(f"{t} FAILED"); continue
            line.append(f"{t} GR={d['g_on_db']-d['g_pre_db']:.1f} over={d['g_min_db']-d['g_on_db']:.1f} att63={d['att_t63_ms']:.2f} att90={d['att_t90_ms']:.2f} rel50={d['rel_t50_ms']:.0f} rel63={d['rel_t63_ms']:.0f} rel90={d['rel_t90_ms']:.0f}")
    for t in ("thd","thdlo","thd100","thd100lo"):
        p=f"{base}.{t}.tsv"
        if os.path.exists(p):
            h={}; d={}
            for r in rows(p):
                if r[0] in ("2","3","4","5"): h[r[0]]=float(r[3])
                elif len(r)==2:
                    try: d[r[0]]=float(r[1])
                    except ValueError: pass
                elif len(r)==3 and r[0] not in ("in_db",):
                    try: d["gain"]=float(r[2])
                    except ValueError: pass
            line.append(f"{t} thd={d.get('thd_percent',-1):.3f}% h2={h.get('2',0):.0f} h3={h.get('3',0):.0f} gain={d.get('gain',0):.1f} ripple={d.get('ripple_p95_p05_db',0):.2f}")
    print(" | ".join(line))
