/**
 * One requestAnimationFrame loop for the whole app.
 *
 * Guardrail (motion-design.md §4.7): meters, peak-hold decay and any other
 * per-frame reader share this driver so the per-frame cost stays O(1 loop)
 * regardless of how many indicators are on screen. Never start your own rAF
 * loop for a recurring reader — subscribe here.
 */
export type FrameCallback = (dtSeconds: number, nowMs: number) => void;

const subscribers = new Set<FrameCallback>();
let rafId = 0;
let lastTime = 0;

function frame(now: number): void {
  rafId = requestAnimationFrame(frame);
  const dt = lastTime === 0 ? 1 / 60 : Math.min((now - lastTime) / 1000, 0.1);
  lastTime = now;
  for (const fn of subscribers) fn(dt, now);
}

/** Subscribe to the shared frame loop; returns an unsubscribe. */
export function onFrame(fn: FrameCallback): () => void {
  subscribers.add(fn);
  if (rafId === 0 && typeof requestAnimationFrame === "function") {
    lastTime = 0;
    rafId = requestAnimationFrame(frame);
  }
  return () => {
    subscribers.delete(fn);
    if (subscribers.size === 0 && rafId !== 0) {
      cancelAnimationFrame(rafId);
      rafId = 0;
    }
  };
}
