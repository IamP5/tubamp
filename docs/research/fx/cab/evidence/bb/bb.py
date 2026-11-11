"""Black-box helpers for the cab study. render(): auhost render; ir(): load output IR and analyse."""
import os, subprocess, sys
import numpy as np
RE = os.environ["RE"]; RS = os.environ["RE_STATE"]
sys.path.insert(0, RE)
import sig
AUHOST = RS + "/bin/auhost"
HERE = os.path.dirname(os.path.abspath(__file__))

def render(au, inp, out, sets=None, extra=None):
    cmd = [AUHOST, "render", *au, os.path.join(HERE, inp), os.path.join(HERE, out)]
    for k, v in (sets or {}).items():
        cmd += ["--set", f"{k}={v}"]
    cmd += extra or []
    r = subprocess.run(cmd, capture_output=True, text=True, timeout=240)
    if r.returncode != 0:
        print("RENDER FAIL", r.stderr[-300:])
    return " ".join(cmd[1:])

def load(out):
    d, sr = sig.read_wav(os.path.join(HERE, out))
    return d, sr

def analyse(x, sr, t0, amp):
    """x: mono output; impulse of amplitude amp at index t0. Returns dict."""
    h = x[t0:] / amp
    pk = np.max(np.abs(h)); ipk = int(np.argmax(np.abs(h)))
    thr = pk * 10 ** (-40 / 20)
    on = int(np.argmax(np.abs(h) > thr))
    e = np.cumsum(h[::-1] ** 2)[::-1]
    tot = e[0]
    def tdb(db):
        idx = np.nonzero(e < tot * 10 ** (db / 10))[0]
        return int(idx[0]) if len(idx) else len(h)
    return dict(peak=pk, ipk=ipk, onset=on, l2=float(np.sqrt(tot)), t30=tdb(-30), t60=tdb(-60), t80=tdb(-80))

def mag(h, sr, freqs, n=65536):
    H = np.fft.rfft(h[:n], n)
    f = np.fft.rfftfreq(n, 1 / sr)
    out = []
    for fc in freqs:  # 1/6-octave power average
        lo, hi = fc * 2 ** (-1 / 12), fc * 2 ** (1 / 12)
        m = (f >= lo) & (f <= hi)
        out.append(10 * np.log10(np.mean(np.abs(H[m]) ** 2) + 1e-30))
    return np.array(out)

FREQS = [40, 60, 80, 100, 125, 160, 200, 315, 500, 800, 1000, 1600, 2500, 3150, 4000, 5000, 6300, 8000, 10000, 12500, 16000]
