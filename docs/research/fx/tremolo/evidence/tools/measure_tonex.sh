#!/bin/zsh
# measure_tonex.sh <outdir-for-tsv>
# Regenerates the TONEX tremolo measurements, headless (TONEX exposes the tremolo as AU parameters).
# Needs RE_STATE and RE. Works in $RE_STATE/work/tremolo/bb.
# Parameter ids (auhost params aufx Txmm Ikmm): MODEL 73532169, MODULATION 1759670652, MOD MODEL 171070823
# (0.25 = TREMOLO), TREM RATE 29584774, TREM SHAPE 918242203, TREM SPREAD 556104857, TREM LEVEL 911708030.
set -e
T=${0:A:h}; OUT=${1:?outdir}; mkdir -p $OUT
B=$RE_STATE/work/tremolo/bb; mkdir -p $B; cd $B
gen() { [ -f $1 ] || python3 $RE/sig.py gen sine "$@" >/dev/null; }
for l in 36 18 6; do gen sine440_m${l}_12s.wav --freq 440 --db -$l --secs 12 --stereo; done
gen sine3k_m18_12s_st.wav --freq 3000 --db -18 --secs 12 --stereo
tx() { in=$1; out=$2; shift 2; $RE_STATE/bin/auhost render aufx Txmm Ikmm $in $out --set 73532169=0 "$@" 2>&1 | tail -1 | grep -v rendered || true; }
m() { label=$1; in=$2; ref=$3; shift 3; tx $in tx_$label.wav --set 1759670652=1 --set 171070823=0.25 "$@"; python3 $T/tremenv.py tx_$label.wav --freq 440 --ref $ref --skip 3 --label $label | tail -n +2; }
H="label\trate_hz\tgmax\tgmin\tdepth_db\tduty_hi\trise_ms\tfall_ms\tmean_gain_db"
{
echo "# TONEX 1.13.1 (aufx Txmm Ikmm) tremolo, headless. Regenerate: tools/measure_tonex.sh <outdir>"
echo "# one row = \$RE_STATE/bin/auhost render aufx Txmm Ikmm sine440_m18_12s.wav out.wav --set 73532169=0 --set 1759670652=1 --set 171070823=0.25 --set 29584774=<rate> --set 918242203=<shape> --set 911708030=<level>"
echo "#           python3 tools/tremenv.py out.wav --freq 440 --ref <same render with MODULATION off>.wav --skip 3"
echo "# probe: python3 \$RE/sig.py gen sine sine440_m18_12s.wav --freq 440 --db -18 --secs 12 --stereo. Amp MODEL is off (73532169=0)."
echo "# The first 1.5 s of every TONEX render sits 5 dB higher than the rest (plugin start-up), hence --skip 3."
for l in 36 18 6; do tx sine440_m${l}_12s.wav tx_ref_m$l.wav; done
echo "$H"
echo "# rate sweep (SHAPE 0, LEVEL default 0.6)"
for r in 0.25 0.5 0.646465 0.75 1; do m rate$r sine440_m18_12s.wav tx_ref_m18.wav --set 29584774=$r; done
echo "# shape sweep (RATE 0.5)"
for s in 0 0.25 0.5 0.75 1; do m shape$s sine440_m18_12s.wav tx_ref_m18.wav --set 29584774=0.5 --set 918242203=$s; done
echo "# level sweep (RATE 0.5, SHAPE 0)"
for v in 0 0.25 0.5 0.75 1; do m level$v sine440_m18_12s.wav tx_ref_m18.wav --set 29584774=0.5 --set 911708030=$v; done
echo "# input level (RATE 0.5, SHAPE 0, LEVEL 0.6): -36, -18, -6 dBFS"
for l in 36 18 6; do m in-$l sine440_m${l}_12s.wav tx_ref_m$l.wav --set 29584774=0.5; done
echo "# square edge with a 3 kHz carrier (RATE 0.5, SHAPE 1, LEVEL 1): rise/fall are 10-90 %"
tx sine3k_m18_12s_st.wav tx_ref3k.wav
tx sine3k_m18_12s_st.wav tx_edge3k.wav --set 1759670652=1 --set 171070823=0.25 --set 29584774=0.5 --set 918242203=1 --set 911708030=1
python3 $T/tremenv.py tx_edge3k.wav --freq 3000 --ref tx_ref3k.wav --skip 3 --label shape1_3kHz_carrier | tail -n +2
echo "# one cycle (24 points from the gain minimum), RATE 0.5 (5.05 Hz), LEVEL 1.0, SHAPE 0 / 0.5 / 0.75 / 1"
for s in 0 0.5 0.75 1; do tx sine440_m18_12s.wav tx_cyc$s.wav --set 1759670652=1 --set 171070823=0.25 --set 29584774=0.5 --set 911708030=1 --set 918242203=$s; python3 $T/tremenv.py tx_cyc$s.wav --freq 440 --ref tx_ref_m18.wav --skip 3 --table 24 | tail -24 | cut -f2 > cyc_$s.txt; done
echo "phase\tshape0\tshape0.5\tshape0.75\tshape1"; paste cyc_0.txt cyc_0.5.txt cyc_0.75.txt cyc_1.txt | awk '{printf "%.3f\t%s\n", (NR-1)/24, $0}'
echo "# SPREAD: phase of the right channel's gain envelope relative to the left (RATE 0.5, LEVEL 0.6, SHAPE 0)"
for sp in 0 0.25 0.5 0.75 1; do tx sine440_m18_12s.wav tx_spread$sp.wav --set 1759670652=1 --set 171070823=0.25 --set 29584774=0.5 --set 556104857=$sp
python3 - tx_spread$sp.wav $sp <<'PY'
import os, sys, numpy as np
sys.path.insert(0, os.environ['RE']); import sig
x, sr = sig.read_wav(sys.argv[1])
def env(v):
    n = np.arange(len(v)); z = v * np.exp(-2j * np.pi * 440 * n / sr); X = np.fft.fft(z); fr = np.fft.fftfreq(len(v), 1 / sr)
    X *= np.exp(-0.5 * (fr / (150 / 1.177)) ** 2); return 2 * np.abs(np.fft.ifft(X))
l, r = env(x[0])[3 * sr:-sr // 2], env(x[1])[3 * sr:-sr // 2]
t = np.arange(len(l)) / sr; c = np.exp(-2j * np.pi * 5.05 * t)
pl = np.angle(np.sum((l - l.mean()) * c)); pr = np.angle(np.sum((r - r.mean()) * c))
d = np.degrees((pr - pl + np.pi) % (2 * np.pi) - np.pi)
print('spread %s\tphase_R_minus_L_deg\t%.1f\tcorr\t%.3f' % (sys.argv[2], d, np.corrcoef(l, r)[0, 1]))
PY
done
} > $OUT/tonex_tremolo_measure.tsv 2>&1
echo done
