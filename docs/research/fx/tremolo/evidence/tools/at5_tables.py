#!/usr/bin/env python3
"""at5_tables.py: characterise the static LFO / gain-law tables that AmpliTube 5's tremolo modules copy at
construction. Prints shape statistics and a coarse 11-point sample of each (enough to check the claims,
not a copy of the table). Needs RE_STATE. Run: python3 at5_tables.py"""
import glob, os, struct, subprocess, re
import numpy as np
thin = glob.glob(os.path.join(os.environ['RE_STATE'], 'thin', '*-AmpliTube 5'))[0]
out = subprocess.run(['otool', '-l', thin], capture_output=True, text=True).stdout
secs = [(int(m.group(1), 16), int(m.group(2), 16), int(m.group(3))) for m in re.finditer(
    r'sectname \S+\n\s+segname \S+\n\s+addr (0x[0-9a-f]+)\n\s+size (0x[0-9a-f]+)\n\s+offset (\d+)', out)]
def f32(addr, n):
    for a, s, o in secs:
        if a <= addr < a + s and o:
            with open(thin, 'rb') as f:
                f.seek(o + addr - a); return np.array(struct.unpack('<%df' % n, f.read(4 * n)))
db = lambda v: 20 * np.log10(max(v, 1e-12))
def describe(name, addr, n, cyclic=True):
    t = f32(addr, n)
    print(f'\n## {name}: {n} float32 at {addr:#x}')
    print(f'min {t.min():.4f} ({db(t.min()):.1f} dB) at index {t.argmin()}, max {t.max():.4f} at index {t.argmax()}, first {t[0]:.4f}, last {t[-1]:.4f}')
    if cyclic:
        mid = 0.5 * (t.max() + t.min()); d = np.diff(t)
        print(f'fraction of the cycle above the mid level: {np.mean(t > mid):.3f}; steepest rise {d.max():+.4f}/step at {d.argmax()}, steepest fall {d.min():+.4f}/step at {d.argmin()}')
        F = np.abs(np.fft.rfft(t[:-1])); print('harmonics 2..5 re fundamental (dB): ' + ' '.join(f'{db(F[k] / F[1]):.1f}' for k in range(2, 6)))
    idx = np.linspace(0, n - 1, 11).round().astype(int)
    print('11-point sample: ' + ' '.join(f'{t[i]:.3f}' for i in idx))
    return t
describe("AT3::Tremolo (stomp 'Opto Tremolo') LFO table, phase 0..1", 0x305f904, 101)
describe('AT3::FenderTremolo LFO table, phase -1..1 (also used by the Fender amp models)', 0x26257c0, 256)
t0 = describe('SeymourDuncanShapeShifter wave table 0 (Wave knob = 0)', 0x2fe2308, 101)
x = np.arange(101) / 100; print(f'max |table0 - (0.5 - 0.5*cos(2*pi*x))| = {np.max(np.abs(t0 - (0.5 - 0.5 * np.cos(2 * np.pi * x)))):.2e}')
describe('SeymourDuncanShapeShifter wave table 1 (Wave knob = 0.5)', 0x2fe249c, 3, cyclic=False)
t2 = describe('SeymourDuncanShapeShifter wave table 2 (Wave knob = 1)', 0x2fe271c, 102, cyclic=False)
print('table 2 changes value after indices', np.nonzero(np.diff(t2))[0].tolist())
tc = describe('SeymourDuncanShapeShifter gain-law table (input 0..1 -> linear gain)', 0x2fe24a8, 157, cyclic=False)
u = np.arange(157) / 156; p = np.polyfit(u, 20 * np.log10(tc), 1)
print(f'20*log10(table) is a straight line in its input: slope {p[0]:.2f} dB, intercept {p[1]:.2f} dB, worst deviation {np.max(np.abs(20 * np.log10(tc) - np.polyval(p, u))):.2f} dB')
ts = describe('Silvertone1484 tremolo gain table T (gain = 2T-1), input 0.534..1.0', 0x301d2ac, 200, cyclic=False)
g = 0.534 + np.arange(200) / 426.97
print('gain = 2T-1 at input 0.534 / 0.6 / 0.7 / 0.77 / 0.85 / 1.0: ' + ' '.join(f'{2 * np.interp(v, g, ts) - 1:.3f}' for v in (0.534, 0.6, 0.7, 0.77, 0.85, 1.0)))
d = f32(0x312a9b4, 16); print('\n## tempo-sync division table (beats) at 0x312a9b4: ' + ' '.join(f'{v:.4g}' for v in d))
