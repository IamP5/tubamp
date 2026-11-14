/**
 * The wire that runs from In, through every slot, to Out, drawn the way the
 * plugin's board draws it: rows left to right, and out of the end of one row,
 * down, back under it and into the start of the next.
 */

export type Rect = { x: number; y: number; w: number; h: number };
type Point = { x: number; y: number };

export type Wire = {
  d: string;
  /** Length along the wire of each stop: the In jack, then the open slot after k blocks, then Out. */
  stops: number[];
};

/** Gap left between a slot's right edge and the wire's end while that slot waits. */
const LEAD = 6;
/** How far the finished wire runs past the last block to Out. */
const TAIL = 28;
/** Corner radius of the wrap; no longer than the short run out of a row's last slot. */
const r = 8;

export function buildWire(slots: readonly Rect[], start: number): Wire {
  const mid = (s: Rect) => s.y + s.h / 2;
  const pts: Point[] = [{ x: start, y: mid(slots[0]) }];
  const stopAt: number[] = [0];

  slots.forEach((slot, i) => {
    const prev = slots[i - 1];
    if (prev && slot.y > prev.y + 1) {
      const right = prev.x + prev.w + LEAD + 10;
      const between = (prev.y + prev.h + slot.y) / 2;
      const left = slot.x - 14;
      pts.push({ x: right, y: mid(prev) }, { x: right, y: between }, { x: left, y: between }, { x: left, y: mid(slot) });
    }
    pts.push({ x: slot.x + slot.w + LEAD, y: mid(slot) });
    stopAt.push(pts.length - 1);
  });
  const last = slots[slots.length - 1];
  pts.push({ x: last.x + last.w + TAIL, y: mid(last) });
  stopAt.push(pts.length - 1);

  return { d: roundedPath(pts), stops: stopLengths(pts, stopAt) };
}

const isCorner = (a: Point, v: Point, b: Point) =>
  Math.abs((v.x - a.x) * (b.y - v.y) - (v.y - a.y) * (b.x - v.x)) > 1e-6;

function unit(a: Point, b: Point): Point {
  const l = Math.hypot(b.x - a.x, b.y - a.y) || 1;
  return { x: (b.x - a.x) / l, y: (b.y - a.y) / l };
}

function roundedPath(pts: Point[]): string {
  let d = `M${pts[0].x},${pts[0].y}`;
  for (let i = 1; i < pts.length; i++) {
    const v = pts[i];
    const next = pts[i + 1];
    if (next && isCorner(pts[i - 1], v, next)) {
      const a = unit(pts[i - 1], v);
      const b = unit(v, next);
      const sweep = a.x * b.y - a.y * b.x > 0 ? 1 : 0;
      d += ` L${v.x - a.x * r},${v.y - a.y * r} A${r},${r} 0 0 ${sweep} ${v.x + b.x * r},${v.y + b.y * r}`;
    } else {
      d += ` L${v.x},${v.y}`;
    }
  }
  return d;
}

/** Exact lengths: a rounded corner trades 2r of straight line for a quarter circle. */
function stopLengths(pts: Point[], stopAt: number[]): number[] {
  const cut = (2 - Math.PI / 2) * r;
  const at: number[] = [0];
  for (let i = 1; i < pts.length; i++) {
    const corner = i > 1 && isCorner(pts[i - 2], pts[i - 1], pts[i]);
    at.push(at[i - 1] + Math.hypot(pts[i].x - pts[i - 1].x, pts[i].y - pts[i - 1].y) - (corner ? cut : 0));
  }
  return stopAt.map((i) => at[i]);
}
