"""Black-box of tubamp's own cab block through the installed AU (aufx Tamp Tuba).
State is authored by patching the JUCE state XML: chainOrder="cab", irPath=<wav>, cut filters as asked."""
import os, sys, struct, plistlib, subprocess, time, re
import numpy as np
RE = os.environ["RE"]; RS = os.environ["RE_STATE"]
sys.path.insert(0, RE); import sig
HERE = os.path.dirname(os.path.abspath(__file__))
AUHOST = RS + "/bin/auhost"; AU = ["aufx", "Tamp", "Tuba"]

def make_state(name, ir_path, lowcut=20.0, highcut=20000.0, order="cab", cab_on=1.0):
    p = plistlib.load(open(os.path.join(HERE, "base.aupreset"), "rb"))
    d = p["jucePluginState"]; magic, size = struct.unpack("<II", d[:8])
    xml = d[8:8 + size].decode("utf8")
    xml = xml.replace('irPath=""', 'irPath="%s"' % ir_path).replace('chainOrder="amp"', 'chainOrder="%s"' % order)
    xml = re.sub(r'(<PARAM id="cab_lowcut" value=")[^"]*', r'\g<1>%r' % lowcut, xml)
    xml = re.sub(r'(<PARAM id="cab_highcut" value=")[^"]*', r'\g<1>%r' % highcut, xml)
    xml = re.sub(r'(<PARAM id="cab_on" value=")[^"]*', r'\g<1>%r' % cab_on, xml)
    b = xml.encode("utf8")
    p["jucePluginState"] = struct.pack("<II", magic, len(b)) + b + b"\0"
    p.pop("data", None)
    out = os.path.join(HERE, f"st_{name}.aupreset")
    plistlib.dump(p, open(out, "wb"))
    return out

def render(state, inp, out, block=512, tail=0.5, warmup=0.5):
    cmd = [AUHOST, "render", *AU, inp, os.path.join(HERE, out), "--state", state, "--block", str(block), "--tail", str(tail), "--warmup", str(warmup)]
    t = time.time(); r = subprocess.run(cmd, capture_output=True, text=True, timeout=600); dt = time.time() - t
    if r.returncode: print("FAIL", r.stderr[-300:])
    return dt, " ".join(cmd[1:])

def wav(name, data, sr):
    path = os.path.join(HERE, name); sig.write_wav(path, np.asarray(data, dtype=np.float32), sr); return path

def load(name):
    return sig.read_wav(os.path.join(HERE, name))

def resample_ideal(h, src, dst, n_out=None):
    """Band-limited (FFT) resampling of a short IR; exact for content below min(src,dst)/2."""
    n = len(h); pad = 1 << int(np.ceil(np.log2(n * 4)))
    H = np.fft.rfft(h, pad)
    m = int(round(pad * dst / src)); Hm = np.zeros(m // 2 + 1, complex)
    k = min(len(H), len(Hm)); Hm[:k] = H[:k]
    y = np.fft.irfft(Hm, m) * (m / pad)
    return y[: n_out or int(round(n * dst / src))]
