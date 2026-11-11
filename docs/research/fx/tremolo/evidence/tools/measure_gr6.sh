#!/bin/zsh
# measure_gr6.sh <outdir-for-tsv>
# Regenerates every Guitar Rig 6 tremolo measurement of this study, headless.
# Needs: RE_STATE (shared state dir with bin/auhost) and RE (repo tools/re). Works in $RE_STATE/work/tremolo/bb.
# Steps per row: gr6state.py builds an AU state holding one rack component (parameter block taken from the
# plugin's own '<Name> Factory.ngrc'), `auhost render aufx 'Ni$R' -NI- probe.wav out.wav --state st.aupreset`
# renders a steady sine through it, tremenv.py demodulates the gain envelope.
set -e
T=${0:A:h}; OUT=${1:?outdir}; mkdir -p $OUT
B=$RE_STATE/work/tremolo/bb; mkdir -p $B; cd $B
AU=$RE_STATE/bin/auhost
D="/Library/Application Support/Native Instruments/Guitar Rig 6/Component Presets"
[ -f gr6_default.aupreset ] || $AU state aufx 'Ni$R' '-NI-' gr6_default.aupreset
for c in "Tremolo" "AC Box" "Twang Reverb"; do n=${c// /}; python3 -I $T/ngrc_xml.py "$D/$c/$c Factory.ngrc" ngrc_$n >/dev/null; done
gen() { [ -f $1 ] || python3 $RE/sig.py gen sine "$@" >/dev/null; }
gen sine3k_m18.wav --freq 3000 --db -18 --secs 12; gen sine3k_m6.wav --freq 3000 --db -6 --secs 6; gen sine3k_m36.wav --freq 3000 --db -36 --secs 6
gen sine3k_m18_st.wav --freq 3000 --db -18 --secs 6 --stereo
for l in 42 30 18 6; do gen sine440_m$l.wav --freq 440 --db -$l --secs 6; done; gen sine440_m0.wav --freq 440 --db -0.1 --secs 6
H="label\trate_hz\tgmax\tgmin\tdepth_db\tduty_hi\trise_ms\tfall_ms\tmean_gain_db"
st() { tag=$1; probe=$2; doc=$3; name=$4; shift 4      # build state + render
  python3 $T/gr6state.py gr6_default.aupreset $doc "$name" st_$tag.aupreset "$@" Rack.G=0
  $AU render aufx 'Ni$R' '-NI-' $probe out_$tag.wav --state st_$tag.aupreset >/dev/null 2>&1 || echo "render failed $tag"; }
env() { python3 $T/tremenv.py "$@" | tail -n +2; }

# ---------------- Tremolo component (NI::DSP::RigTremolo)
TR=ngrc_Tremolo_doc0.xml
{
echo "# Guitar Rig 6 'Tremolo' rack component (NI::DSP::RigTremolo), headless. Regenerate: tools/measure_gr6.sh <outdir>"
echo "# one row = python3 tools/gr6state.py gr6_default.aupreset ngrc_Tremolo_doc0.xml Tremolo st.aupreset Rt=<r> Int=<i> Up=<u> Dn=<d> Wdt=<w> Rack.G=0"
echo "#           \$RE_STATE/bin/auhost render aufx 'Ni\$R' -NI- sine3k_m18.wav out.wav --state st.aupreset"
echo "#           python3 tools/tremenv.py out.wav --freq 3000 --refdb -18"
echo "# probe: python3 \$RE/sig.py gen sine sine3k_m18.wav --freq 3000 --db -18 --secs 12   (gain 1.0 = input level; rise/fall = 10-90 % of the swing)"
echo "# A rate curve: Rt sweep (Int=1 Up=0 Dn=0 Wdt=0.5)"; echo "$H"
for r in 0.125 0.25 0.375 0.5 0.625 0.75 0.875 1; do st rate_$r sine3k_m18.wav $TR Tremolo Rt=$r Int=1 Up=0 Dn=0; env out_rate_$r.wav --freq 3000 --refdb -18 --label Rt=$r; done
echo "# B depth: Int sweep (Rt=0.5 Up=0 Dn=0)"; echo "$H"
for d in 0 0.25 0.5 0.75 0.9 1; do st int_$d sine3k_m18.wav $TR Tremolo Rt=0.5 Int=$d Up=0 Dn=0; env out_int_$d.wav --freq 3000 --refdb -18 --label Int=$d; done
echo "# C edges: Up sweep (Rt=0.5 Int=1 Dn=0), then Dn sweep (Up=0)"; echo "$H"
for u in 0 0.25 0.5 0.75; do st up_$u sine3k_m18.wav $TR Tremolo Rt=0.5 Int=1 Up=$u Dn=0; env out_up_$u.wav --freq 3000 --refdb -18 --label Up=$u; done
for u in 0.25 0.5 0.75 1; do st dn_$u sine3k_m18.wav $TR Tremolo Rt=0.5 Int=1 Up=0 Dn=$u; env out_dn_$u.wav --freq 3000 --refdb -18 --label Dn=$u; done
echo "# D width: Wdt sweep (Rt=0.5 Int=1 Up=0 Dn=0)"; echo "$H"
for w in 0 0.25 0.5 0.75 1; do st wdt_$w sine3k_m18.wav $TR Tremolo Rt=0.5 Int=1 Up=0 Dn=0 Wdt=$w; env out_wdt_$w.wav --freq 3000 --refdb -18 --label Wdt=$w; done
echo "# E factory INIT preset (Rt=0.500057 Int=0.5 Up=0.5 Dn=0.5 Wdt=0.5), one cycle"; echo "$H"
st init sine3k_m18.wav $TR Tremolo; python3 $T/tremenv.py out_init.wav --freq 3000 --refdb -18 --label INIT --table 20 | tail -n +2
echo "# F input level: Rt=0.5 Int=0.75 Up=0.5 Dn=0.5 at -6, -18, -36 dBFS"; echo "$H"
for l in 6 18 36; do st lvl_m$l sine3k_m$l.wav $TR Tremolo Rt=0.5 Int=0.75; env out_lvl_m$l.wav --freq 3000 --refdb -$l --label in-${l}dBFS; done
echo "# G stereo: Pan=1 (Rt=0.5 Int=0.75 Up=0.5 Dn=0.5), stereo probe, channel 0 then channel 1"; echo "$H"
st pan sine3k_m18_st.wav $TR Tremolo Rt=0.5 Int=0.75 Pan=1
env out_pan.wav --freq 3000 --refdb -18 --ch 0 --label pan_L; env out_pan.wav --freq 3000 --refdb -18 --ch 1 --label pan_R
python3 - <<'PY'
import os, sys, numpy as np
sys.path.insert(0, os.environ['RE']); import sig
x, sr = sig.read_wav('out_pan.wav')
def env(v):
    n = np.arange(len(v)); z = v * np.exp(-2j * np.pi * 3000 * n / sr); X = np.fft.fft(z); fr = np.fft.fftfreq(len(v), 1 / sr)
    X *= np.exp(-0.5 * (fr / (1050 / 1.177)) ** 2); return 2 * np.abs(np.fft.ifft(X)) / 10 ** (-18 / 20)
l, r = env(x[0])[sr:-sr // 4], env(x[1])[sr:-sr // 4]
print('corr(gL,gR)\t%.4f' % np.corrcoef(l, r)[0, 1]); print('gL+gR min/max\t%.4f\t%.4f' % ((l + r).min(), (l + r).max()))
print('gL^2+gR^2 min/max\t%.4f\t%.4f' % ((l * l + r * r).min(), (l * l + r * r).max()))
PY
} > $OUT/gr6_tremolo_measure.tsv 2>&1

# ---------------- AC Box (NI::DSP::RigAC30)
A=ngrc_ACBox_doc1.xml; C=ngrc_ACBox_doc2.xml
{
echo "# Guitar Rig 6 'AC Box' amp (NI::DSP::RigAC30) tremolo, headless. Regenerate: tools/measure_gr6.sh <outdir>"
echo "# one row = python3 tools/gr6state.py gr6_default.aupreset ngrc_ACBox_doc1.xml 'AC Box' st.aupreset TDt=<depth> TSp=<speed> Rack.G=0"
echo "#           \$RE_STATE/bin/auhost render aufx 'Ni\$R' -NI- sine440_m<L>.wav out.wav --state st.aupreset"
echo "#           python3 tools/tremenv.py out.wav --freq 440 --ref <same amp, TDt=0, same level>.wav"
echo "# doc1 = factory preset Clean, doc2 = factory preset Crunch. Gain 1.0 = the amp without tremolo at that input level."
echo "$H"
for l in 42 30 18 6 0; do
  st ac_ref_m$l sine440_m$l.wav $A "AC Box" TDt=0 TSp=0.5
  python3 $RE/sig.py stats out_ac_ref_m$l.wav | sed -n 2p | awk -v l=$l '{print "# Clean, TDt=0, in -"l" dBFS: out peak "$2" dB, rms "$3" dB"}'
  for d in 0.25 0.5 0.75 1; do st ac_m${l}_d$d sine440_m$l.wav $A "AC Box" TDt=$d TSp=0.5; env out_ac_m${l}_d$d.wav --freq 440 --ref out_ac_ref_m$l.wav --label clean_in-${l}dB_TDt$d; done
done
echo "# speed sweep (Clean, TDt=1, -18 dBFS)"
for s in 0 0.25 0.5 0.75 1; do st ac_s$s sine440_m18.wav $A "AC Box" TDt=1 TSp=$s; env out_ac_s$s.wav --freq 440 --ref out_ac_ref_m18.wav --label clean_TSp$s; done
echo "# Crunch preset, TDt=1 TSp=0.5"
for l in 30 18 6 0; do
  st cr_ref_m$l sine440_m$l.wav $C "AC Box" TDt=0 TSp=0.5
  python3 $RE/sig.py stats out_cr_ref_m$l.wav | sed -n 2p | awk -v l=$l '{print "# Crunch, TDt=0, in -"l" dBFS: out peak "$2" dB, rms "$3" dB"}'
  st cr_m${l}_d1 sine440_m$l.wav $C "AC Box" TDt=1 TSp=0.5; env out_cr_m${l}_d1.wav --freq 440 --ref out_cr_ref_m$l.wav --label crunch_in-${l}dB_TDt1
done
echo "# spectrum lines (dBFS, Hann, 4 s): LFO feed-through at 4.5 Hz, carrier, harmonics, AM sidebands"
python3 - <<'PY'
import os, sys, numpy as np
sys.path.insert(0, os.environ['RE']); import sig
def spec(f):
    x, sr = sig.read_wav(f); v = x[0][sr:sr * 5]; w = np.hanning(len(v)); X = np.abs(np.fft.rfft(v * w)) / np.sum(w) * 2; fr = np.fft.rfftfreq(len(v), 1 / sr)
    pk = lambda f0, bw=1.5: 20 * np.log10(X[(fr > f0 - bw) & (fr < f0 + bw)].max() + 1e-12)
    return [pk(4.5), pk(440), pk(880), pk(1320), pk(440 - 4.5, 1.0), pk(440 + 4.5, 1.0), pk(440 - 9, 1.0)]
print('file\tlfo_4.5Hz\tH1\tH2\tH3\tsb-4.5\tsb+4.5\tsb-9')
for f in ['out_ac_ref_m18.wav', 'out_ac_m18_d1.wav', 'out_ac_ref_m6.wav', 'out_ac_m6_d1.wav', 'out_cr_ref_m6.wav', 'out_cr_m6_d1.wav']:
    print(f + '\t' + '\t'.join('%.1f' % v for v in spec(f)))
PY
} > $OUT/gr6_acbox_measure.tsv 2>&1

# ---------------- Twang Reverb (NI::DSP::RigFender)
F=ngrc_TwangReverb_doc4.xml
{
echo "# Guitar Rig 6 'Twang Reverb' amp (NI::DSP::RigFender) vibrato (= tremolo), headless. Regenerate: tools/measure_gr6.sh <outdir>"
echo "# one row = python3 tools/gr6state.py gr6_default.aupreset ngrc_TwangReverb_doc4.xml 'Twang Reverb' st.aupreset ViOn=1 RvOn=0 ViInt=<i> ViSpd=<s> Rack.G=0"
echo "#           \$RE_STATE/bin/auhost render aufx 'Ni\$R' -NI- sine440_m<L>.wav out.wav --state st.aupreset"
echo "#           python3 tools/tremenv.py out.wav --freq 440 --ref <same amp, ViOn=0, same level>.wav"
echo "# doc4 = factory preset 'Warm Tremolo'. Gain 1.0 = the amp with ViOn=0 at that input level."
echo "$H"
for l in 30 18 6; do
  st tw_ref_m$l sine440_m$l.wav $F "Twang Reverb" ViOn=0 RvOn=0
  python3 $RE/sig.py stats out_tw_ref_m$l.wav | sed -n 2p | awk -v l=$l '{print "# ViOn=0, in -"l" dBFS: out peak "$2" dB, rms "$3" dB"}'
  for d in 0.25 0.5 0.75 1; do st tw_m${l}_d$d sine440_m$l.wav $F "Twang Reverb" ViOn=1 RvOn=0 ViInt=$d ViSpd=0.5; env out_tw_m${l}_d$d.wav --freq 440 --ref out_tw_ref_m$l.wav --label in-${l}dB_ViInt$d; done
done
echo "# speed sweep (ViInt=1, -30 dBFS)"
for s in 0 0.25 0.5 0.75 1; do st tw_s$s sine440_m30.wav $F "Twang Reverb" ViOn=1 RvOn=0 ViInt=1 ViSpd=$s; env out_tw_s$s.wav --freq 440 --ref out_tw_ref_m30.wav --label ViSpd$s; done
echo "# one cycle, ViInt=1 ViSpd=0.5, -30 dBFS (20 points, starts at the gain minimum)"
python3 $T/tremenv.py out_tw_m30_d1.wav --freq 440 --ref out_tw_ref_m30.wav --table 20 | tail -21
} > $OUT/gr6_twang_measure.tsv 2>&1
echo done
