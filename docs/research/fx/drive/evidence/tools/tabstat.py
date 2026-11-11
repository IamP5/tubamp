#!/usr/bin/env python3
"""tabstat.py <binary> 0xADDR N [name]: shape statistics of a static double[N] waveshaping table (no table dump)."""
import os, struct, sys
import numpy as np
from pathlib import Path
sys.path.insert(0, os.environ["RE"]); import disasm
thin = disasm.thin_slice(Path(sys.argv[1])); a = int(sys.argv[2], 16); n = int(sys.argv[3])
t = np.array(struct.unpack(f"<{n}d", disasm.read_vm(thin, a, 8 * n)))
c = n // 2; d = np.diff(t)
zc = np.where(np.sign(t[:-1]) != np.sign(t[1:]))[0]
print(f"{sys.argv[4] if len(sys.argv) > 4 else ''} n={n} min={t.min():.4f} max={t.max():.4f} t[0]={t[0]:.4f} t[c]={t[c]:.4f} t[-1]={t[-1]:.4f} zero-cross idx={list(zc[:4])}")
k = int(np.argmax(np.abs(d))); print(f"  max |slope| {abs(d[k]):.5f}/step at idx {k}; slope at ends {d[0]:.5f}, {d[-1]:.5f}; ratio centre/end slope {abs(d[k])/max(abs(d[0]),1e-12):.1f}, {abs(d[k])/max(abs(d[-1]),1e-12):.1f}")
z = zc[0] if len(zc) else c
m = min(z, n - 1 - z); odd = t[z + 1: z + m] + t[z - 1: z - m: -1] if m > 2 else np.array([0.0])
print(f"  odd-symmetry error about its zero crossing: max |f(x)+f(-x)| = {np.max(np.abs(odd)):.4f} (peak {np.max(np.abs(t)):.4f}); +side peak {t[z:].max():.4f} / -side peak {t[:z+1].min():.4f}")
mono = np.all(d >= 0) or np.all(d <= 0); print(f"  monotonic: {mono}; sign of slope {'+' if d[k] > 0 else '-'}")
# knee: input distance (in steps from zero crossing) where local slope falls to half and to a tenth of the max
s = np.abs(d) / abs(d[k])
for lab, side in (("+", range(k, n - 1)), ("-", range(k, -1, -1))):
    h = next((abs(i - k) for i in side if s[i] < 0.5), None); q = next((abs(i - k) for i in side if s[i] < 0.1), None)
    print(f"  {lab} side: slope < 50% after {h} steps, < 10% after {q} steps")
