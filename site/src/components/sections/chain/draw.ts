import { RATE, renderSignal, type Stage } from "./signal";
import type { Scene } from "./scene";

export type Ink = { signal: string; ghost: string; base: string };

const cache = new Map<string, Float32Array>();

function signal(chain: readonly Stage[], start: number, length: number) {
  const key = `${chain.join()}|${start}|${length}`;
  let x = cache.get(key);
  if (!x) {
    if (cache.size > 6) cache.clear();
    x = renderSignal(chain, { start, length });
    cache.set(key, x);
  }
  return x;
}

/**
 * The signal after the blocks in so far, in amber, over the signal before the
 * last one arrived, in grey, so each block's change reads as a difference.
 * `span` is how much signal time fits across the canvas.
 */
export function drawWave(canvas: HTMLCanvasElement, scene: Scene, ink: Ink, span: number) {
  const ctx = canvas.getContext("2d");
  if (!ctx) return;
  const dpr = Math.min(window.devicePixelRatio || 1, 2);
  const w = canvas.clientWidth;
  const h = canvas.clientHeight;
  if (canvas.width !== Math.round(w * dpr) || canvas.height !== Math.round(h * dpr)) {
    canvas.width = Math.round(w * dpr);
    canvas.height = Math.round(h * dpr);
  }
  ctx.setTransform(dpr, 0, 0, dpr, 0, 0);
  ctx.clearRect(0, 0, w, h);
  ctx.globalAlpha = scene.chrome;

  const length = Math.round(span * RATE);
  const start = -Math.round(scene.time * RATE);
  const { from, to, mix } = scene.wave;
  const a = signal(from, start, length);
  const b = signal(to, start, length);
  const changed = from.join() !== to.join();
  const mid = h / 2;
  const gain = h * 0.5;
  const x = (i: number) => (i / (length - 1)) * w;
  const y = (v: number) => mid - Math.max(-0.98, Math.min(0.98, v)) * gain;

  ctx.strokeStyle = ink.base;
  ctx.lineWidth = 1;
  ctx.beginPath();
  ctx.moveTo(0, mid + 0.5);
  ctx.lineTo(w, mid + 0.5);
  ctx.stroke();

  ctx.lineJoin = "round";
  if (changed && mix > 0) {
    ctx.globalAlpha = scene.chrome * Math.min(1, mix * 1.5) * 0.55;
    ctx.strokeStyle = ink.ghost;
    ctx.beginPath();
    for (let i = 0; i < length; i++) ctx.lineTo(x(i), y(a[i]));
    ctx.stroke();
    ctx.globalAlpha = scene.chrome;
  }

  ctx.strokeStyle = ink.signal;
  ctx.lineWidth = 1.5;
  ctx.beginPath();
  for (let i = 0; i < length; i++) ctx.lineTo(x(i), y(a[i] + (b[i] - a[i]) * mix));
  ctx.stroke();
}
