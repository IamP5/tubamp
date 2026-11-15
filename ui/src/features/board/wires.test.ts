/**
 * Self-checking tests for ./wires.ts, in the style of chain/structure.test.ts
 * (no runner). Run from ui/:
 *
 *     node_modules/.bin/esbuild src/features/board/wires.test.ts --bundle \
 *       --platform=node --log-level=warning | node
 *
 * Exits non-zero on any failure.
 */
import type { WidthStep, WidthView } from "../../bridge/types";
import {
  RAIL_OFFSET,
  bowPath,
  linkWidths,
  straightPath,
  turnText,
  wrapPath,
  type LinkEnds,
} from "./wires";

let failures = 0;
let passes = 0;

function check(name: string, ok: boolean, detail = ""): void {
  if (ok) passes += 1;
  else {
    failures += 1;
    console.error(`FAIL ${name}${detail ? `: ${detail}` : ""}`);
  }
}

function eq(name: string, got: unknown, want: unknown): void {
  const g = JSON.stringify(got);
  const w = JSON.stringify(want);
  check(name, g === w, `got ${g}, want ${w}`);
}

/* ─────────────────────────────── linkWidths ────────────────────────────── */

const link = (from: string, to: string, lane?: "a" | "b"): LinkEnds => ({
  from: { id: from },
  to: { id: to },
  ...(lane ? { lane } : {}),
});

function view(steps: WidthStep[], over: Partial<WidthView> = {}): WidthView {
  return {
    host: { in: 1, out: 2 },
    input: "mono",
    steps,
    output: steps[steps.length - 1]?.out ?? "mono",
    wouldBeStereo: false,
    notice: null,
    ...over,
  };
}

eq("no view draws as before", linkWidths(undefined, [link("in", "amp")]), null);

{
  const serial = view([
    { block: "amp", out: "mono", turn: "keeps" },
    { block: "delay", out: "stereo", turn: "widens", why: "pingPong" },
    { block: "reverb", out: "stereo", turn: "keeps" },
  ]);
  eq(
    "serial: each wire takes the width leaving its source; add passes it on",
    linkWidths(serial, [
      link("in", "amp"),
      link("amp", "delay"),
      link("delay", "reverb"),
      link("reverb", "add"),
      link("add", "out"),
    ]),
    ["mono", "mono", "stereo", "stereo", "stereo"],
  );
  eq(
    "the wire into Out takes output, whatever its source says",
    linkWidths({ ...serial, output: "mono" }, [link("reverb", "out")]),
    ["mono"],
  );
  eq(
    "the wire from In takes input",
    linkWidths({ ...serial, input: "stereo" }, [link("in", "amp")]),
    ["stereo"],
  );
}

{
  const lanes = view([
    { block: "split", out: "mono", outB: "stereo", turn: "keeps" },
    { block: "amp", out: "mono", turn: "keeps" },
    { block: "lane2", out: "stereo", turn: "keeps" },
    { block: "mix", out: "stereo", turn: "widens", why: "lanes" },
  ]);
  eq(
    "split: lane A takes out, lane B outB; a lane's add slot and an empty lane's bow pass it on",
    linkWidths(lanes, [
      link("split", "amp", "a"),
      link("amp", "lane-a", "a"),
      link("lane-a", "mix", "a"),
      link("split", "mix", "b"),
      link("mix", "out"),
    ]),
    ["mono", "mono", "mono", "stereo", "stereo"],
  );
  eq(
    "a block the view does not know yet (drag preview) falls back to mono",
    linkWidths(lanes, [link("eq", "out"), link("eq", "mix")]),
    ["stereo", "mono"],
  );
}

/* ──────────────────────────────── turnText ─────────────────────────────── */

eq(
  "widen text",
  turnText({ block: "delay", out: "stereo", turn: "widens", why: "pingPong" }),
  "Stereo from here · ping-pong repeats",
);
eq(
  "fold text",
  turnText({ block: "amp", out: "mono", turn: "folds", why: "amp" }),
  "Mono from here · the amp sums left and right",
);
eq(
  "plugin fold",
  turnText({ block: "fx1", out: "mono", turn: "folds", why: "pluginMono" }),
  "Mono from here · plugin is mono only",
);
eq("no why: the lead alone", turnText({ block: "mix", out: "stereo", turn: "widens" }), "Stereo from here");
eq("keeps: no tooltip", turnText({ block: "eq", out: "stereo", turn: "keeps" }), null);
eq(
  "monoHost: no tooltip",
  turnText({ block: "delay", out: "mono", turn: "monoHost", why: "pingPong" }),
  null,
);

/* ─────────────────────────────── rail paths ────────────────────────────── */

type Pt = [number, number];

/** Dense points along an SVG path made of M, H, V, Q and C commands. */
function sample(d: string, steps = 64): Pt[] {
  const pts: Pt[] = [];
  let x = 0;
  let y = 0;
  for (const [, cmd, args] of d.matchAll(/([MHVQC])([^MHVQC]*)/g)) {
    const n = args!.trim().split(/[\s,]+/).map(Number);
    if (cmd === "M") {
      [x, y] = [n[0]!, n[1]!];
      pts.push([x, y]);
      continue;
    }
    const lerp = (to: Pt): void => {
      const count = Math.max(steps, Math.ceil(Math.hypot(to[0] - x, to[1] - y) * 4));
      for (let i = 1; i <= count; i += 1)
        pts.push([x + ((to[0] - x) * i) / count, y + ((to[1] - y) * i) / count]);
      [x, y] = to;
    };
    if (cmd === "H") lerp([n[0]!, y]);
    else if (cmd === "V") lerp([x, n[0]!]);
    else if (cmd === "Q") {
      const [cx, cy, ex, ey] = n as [number, number, number, number];
      for (let i = 1; i <= steps; i += 1) {
        const t = i / steps;
        const u = 1 - t;
        pts.push([u * u * x + 2 * u * t * cx + t * t * ex, u * u * y + 2 * u * t * cy + t * t * ey]);
      }
      [x, y] = [ex, ey];
    } else {
      const [ax, ay, bx, by, ex, ey] = n as [number, number, number, number, number, number];
      for (let i = 1; i <= steps; i += 1) {
        const t = i / steps;
        const u = 1 - t;
        pts.push([
          u * u * u * x + 3 * u * u * t * ax + 3 * u * t * t * bx + t * t * t * ex,
          u * u * u * y + 3 * u * u * t * ay + 3 * u * t * t * by + t * t * t * ey,
        ]);
      }
      [x, y] = [ex, ey];
    }
  }
  return pts;
}

function nearest(p: Pt, line: readonly Pt[]): number {
  let best = Infinity;
  for (const q of line) best = Math.min(best, Math.hypot(p[0] - q[0], p[1] - q[1]));
  return best;
}

/** Every point of each rail sits RAIL_OFFSET from the centre line (within
 *  `tol`), and the two rails never come closer than their spacing allows. */
function railsParallel(name: string, build: (o: number) => string, tol: number): void {
  const centre = sample(build(0), 200);
  const left = sample(build(RAIL_OFFSET));
  const right = sample(build(-RAIL_OFFSET));
  // The ends are cut square at the tiles, where the offset is purely vertical.
  const inner = (pts: Pt[]) => pts.slice(2, -2);
  let worst = 0;
  for (const p of [...inner(left), ...inner(right)])
    worst = Math.max(worst, Math.abs(nearest(p, centre) - RAIL_OFFSET));
  check(`${name}: rails ${RAIL_OFFSET}px off the centre line`, worst <= tol, `worst deviation ${worst.toFixed(2)}px`);
  const rightDense = sample(build(-RAIL_OFFSET), 200);
  let closest = Infinity;
  for (const p of inner(left)) closest = Math.min(closest, nearest(p, rightDense));
  check(
    `${name}: rails never meet`,
    closest >= 2 * RAIL_OFFSET - 2 * tol,
    `closest ${closest.toFixed(2)}px`,
  );
}

railsParallel("straight run", (o) => straightPath(180, 0, 212, 0, o), 0.01);
railsParallel("split fan-out (S, down)", (o) => straightPath(212, 0, 244, 34, o), 0.6);
railsParallel("mixer converge (S, up)", (o) => straightPath(600, -34, 632, 0, o), 0.6);
railsParallel("row wrap", (o) => wrapPath(900, 0, 64, 88, 44, o), 0.15);
railsParallel("empty lane bow", (o) => bowPath(212, 0, 424, 0, 34, o), 0.65);

{
  const d = straightPath(0, 0, 32, 0, RAIL_OFFSET);
  check("the left rail runs above the right one", d.includes(`M0 ${-RAIL_OFFSET}H32`), d);
  const w = sample(wrapPath(900, 0, 64, 88, 44, RAIL_OFFSET));
  const end = w[w.length - 1]!;
  check("the left rail is still the upper one after a wrap", end[1] === 88 - RAIL_OFFSET, `${end[1]}`);
}

eq(
  "a mono wire is exactly the old centre line",
  [straightPath(0, 0, 32, 0), wrapPath(900, 0, 64, 88, 44)],
  [
    "M0 0H32",
    "M900 0H910Q920 0 920 10V34Q920 44 910 44H54Q44 44 44 54V78Q44 88 54 88H64",
  ],
);

if (failures > 0) {
  console.error(`${failures} failed, ${passes} passed`);
  throw new Error("wires tests failed");
}
console.log(`wires tests: ${passes} passed`);
