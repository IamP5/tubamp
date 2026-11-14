import { RATE, renderSignal, type Stage } from "./signal";

export type Ink = { signal: string; ghost: string; base: string };

/** `span` is how much signal time fits across the canvas. */
export const samples = (order: readonly Stage[], span: number) =>
  renderSignal(order, { start: 0, length: Math.round(span * RATE) });

export function blend(from: Float32Array, to: Float32Array, mix: number): Float32Array {
  return to.map((v, i) => from[i] + (v - from[i]) * mix);
}

/** `from` easing into `to`. The shape being left stays as a ghost until the new one has arrived. */
export function drawWave(canvas: HTMLCanvasElement, from: Float32Array, to: Float32Array, mix: number, ink: Ink) {
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

  const length = to.length;
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
  if (mix < 1) {
    ctx.globalAlpha = (1 - mix) * 0.55;
    ctx.strokeStyle = ink.ghost;
    ctx.beginPath();
    for (let i = 0; i < length; i++) ctx.lineTo(x(i), y(from[i]));
    ctx.stroke();
    ctx.globalAlpha = 1;
  }

  ctx.strokeStyle = ink.signal;
  ctx.lineWidth = 1.5;
  ctx.beginPath();
  for (let i = 0; i < length; i++) ctx.lineTo(x(i), y(from[i] + (to[i] - from[i]) * mix));
  ctx.stroke();
}
