/**
 * Filter magnitudes: analog prototypes close enough to the engines' filters to
 * draw honestly, cheap enough to evaluate per pixel. All return dB at frequency
 * `f`. Pure (no React), so the mock bridge can apply the same laws to its fake
 * spectrum; `display.tsx` re-exports them for the editors.
 */

/** 12 dB/oct high-pass (a low cut) at `fc`. */
export function lowCutDb(f: number, fc: number): number {
  const r4 = Math.pow(f / fc, 4);
  return 10 * Math.log10(r4 / (1 + r4) + 1e-12);
}

/** 12 dB/oct low-pass (a high cut) at `fc`. */
export function highCutDb(f: number, fc: number): number {
  return -10 * Math.log10(1 + Math.pow(f / fc, 4));
}

/** Peaking bell at `f0`, `gainDb`, quality `q`. */
export function bellDb(f: number, f0: number, gainDb: number, q: number): number {
  const a = Math.pow(10, gainDb / 40);
  const w = f / f0;
  const re = 1 - w * w;
  const num = re * re + Math.pow((w * a) / q, 2);
  const den = re * re + Math.pow(w / (a * q), 2);
  return 10 * Math.log10(num / den);
}

/** Low shelf at `f0`, `gainDb` (Q 0.707). */
export function lowShelfDb(f: number, f0: number, gainDb: number): number {
  const a = Math.pow(10, gainDb / 40);
  const w = f / f0;
  const k = Math.sqrt(a) / Math.SQRT1_2;
  const num = Math.pow(a - w * w, 2) + Math.pow(k * w, 2);
  const den = Math.pow(1 - a * w * w, 2) + Math.pow(k * w, 2);
  return 10 * Math.log10((a * a * num) / den);
}

/** High shelf at `f0`, `gainDb` (Q 0.707). */
export function highShelfDb(f: number, f0: number, gainDb: number): number {
  const a = Math.pow(10, gainDb / 40);
  const w = f / f0;
  const k = Math.sqrt(a) / Math.SQRT1_2;
  const num = Math.pow(1 - a * w * w, 2) + Math.pow(k * w, 2);
  const den = Math.pow(a - w * w, 2) + Math.pow(k * w, 2);
  return 10 * Math.log10((a * a * num) / den);
}
