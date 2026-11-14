/**
 * The delay's taps on a time axis (ui/DESIGN.md §Editors, Delay).
 *
 * Each repeat is a thin bar at its time, its height its level in dB: the first
 * repeat at full level, every next one `feedback` lower, until it falls under
 * −40 dB or past the axis. Left is drawn above the axis and right below, from
 * the routing in docs/STEREO.md §2: Stereo repeats on both sides at once,
 * Ping-Pong alternates (left at t, right at 2t, …), Dual runs the right side
 * at time × ratio. Width is applied as the engine does (M/S on the wet), so a
 * narrowed ping-pong shows its leak to the other side.
 *
 * Handles: the first repeat drags Time (locked while synced), the second
 * repeat's top drags Feedback.
 *
 * Live: each repeat lights with what was played one repeat time before it,
 * scaled by its own level — so a note walks out along the taps, one delay time
 * apart, alternating sides in Ping-Pong and on the right's own clock in Dual.
 */
import { useMemo, useRef } from "react";
import type { BlockId, ComboParamId, SliderParamId } from "../../bridge";
import { useComboParam, useSliderParam } from "../../hooks";
import { Plot, PlotHandle, clamp, plot as p, useEase, useLive } from "./display";
import { noteSeconds } from "./tempoSync";

const MODE_PING_PONG = 1;
const MODE_DUAL = 2;

/** Time axis 0 to 4 s (the longest first repeat: 2 s × 200 % in Dual),
 *  compressed so short times get room and long ones still fit. */
const T_MAX = 4;
const T_KNEE = 0.3;
const PAD = 12;
const timeX = (t: number, w: number): number =>
  PAD + (Math.log(1 + clamp(t, 0, T_MAX) / T_KNEE) / Math.log(1 + T_MAX / T_KNEE)) * (w - PAD);
const xTime = (x: number, w: number): number =>
  T_KNEE * (Math.pow(1 + T_MAX / T_KNEE, clamp((x - PAD) / (w - PAD), 0, 1)) - 1);

const TIME_TICKS: readonly (readonly [number, string])[] = [
  [0.1, "0.1 s"],
  [0.5, "0.5 s"],
  [1, "1 s"],
  [2, "2 s"],
];

/** The ticks that keep a label's width apart at this plot width, from the left. */
function ticksThatFit(w: number): (readonly [number, string])[] {
  const kept: (readonly [number, string])[] = [];
  let last = -Infinity;
  for (const tick of TIME_TICKS) {
    const x = timeX(tick[0], w);
    if (x - last < 40) continue;
    kept.push(tick);
    last = x;
  }
  return kept;
}

/** Repeats quieter than this are not drawn. */
const FLOOR_DB = -40;
const FLOOR_GAIN = Math.pow(10, FLOOR_DB / 20);

interface Tap {
  t: number;
  /** Linear levels on the left and right, after width. */
  l: number;
  r: number;
}

/** Every repeat that is audible and on the axis, in time order. */
function taps(time: number, feedback: number, mode: number, ratio: number, width: number): Tap[] {
  const raw: { t: number; l: number; r: number }[] = [];
  const rTime = mode === MODE_DUAL ? time * ratio : time;
  for (let k = 1; k <= 400; k++) {
    const level = Math.pow(feedback, k - 1);
    if (level < FLOOR_GAIN) break;
    const tl = k * time;
    const tr = k * rTime;
    if (tl > T_MAX && tr > T_MAX) break;
    if (mode === MODE_PING_PONG) {
      raw.push(k % 2 === 1 ? { t: tl, l: level, r: 0 } : { t: tl, l: 0, r: level });
    } else if (Math.abs(tl - tr) < 1e-9) {
      raw.push({ t: tl, l: level, r: level });
    } else {
      raw.push({ t: tl, l: level, r: 0 }, { t: tr, l: 0, r: level });
    }
    if (feedback <= 0) break;
  }
  return raw
    .filter((tap) => tap.t <= T_MAX)
    .sort((a, b) => a.t - b.t)
    .map(({ t, l, r }) => {
      const mid = (l + r) / 2;
      const side = ((l - r) / 2) * width;
      return { t, l: mid + side, r: mid - side };
    });
}

/** A lit repeat at full input and full level: clearly there, short of a flash. */
const ENERGY_OPACITY = 0.5;
/** Width of a lit repeat, px: a column behind the bar, the bar stays crisp. */
const LIT_WIDTH = 6;

/** One side of one repeat, for the live layer: when it sounds and how loud. */
interface Light {
  t: number;
  gain: number;
  /** The repeat's level against the dry note, in the signal's 0..1 units
   *  (useSignal: dB-linear over 48 dB), so input and feedback add as dB do. */
  offset: number;
  up: boolean;
}

/** The window `useSignal` levels span, dB. */
const SIGNAL_RANGE_DB = 48;

export function TapPlot({ block }: { block: BlockId }) {
  const time = useSliderParam(`${block}_time` as SliderParamId);
  const feedback = useSliderParam(`${block}_feedback` as SliderParamId);
  const ratio = useSliderParam(`${block}_ratio` as SliderParamId);
  const width = useSliderParam(`${block}_width` as SliderParamId);
  const mode = useComboParam(`${block}_mode` as ComboParamId);
  const sync = useComboParam(`${block}_sync` as ComboParamId);

  const synced = noteSeconds(sync.text);
  /* Synced: the note length at the drawing tempo, clamped as the engine does.
     What is drawn travels to a value that changed without a hand on it (a
     type's starting points, a preset, a sync pick); see useEase. */
  const seconds = useEase(synced === null ? time.scaled / 1000 : clamp(synced, 0.02, 2));
  const fb = useEase(feedback.scaled);
  const w = useEase(width.scaled);
  const ratioN = useEase(ratio.scaled) / 100;
  const list = useMemo(() => taps(seconds, fb, mode.index, ratioN, w), [seconds, fb, mode.index, ratioN, w]);

  /* The live layer, one entry per drawn bar side, in the order they are drawn. */
  const lights = useMemo(() => {
    const out: Light[] = [];
    for (const tap of list) {
      const offset = (gain: number): number => (20 * Math.log10(Math.min(1, gain))) / SIGNAL_RANGE_DB;
      if (tap.l > FLOOR_GAIN) out.push({ t: tap.t, gain: tap.l, offset: offset(tap.l), up: true });
      if (tap.r > FLOOR_GAIN) out.push({ t: tap.t, gain: tap.r, offset: offset(tap.r), up: false });
    }
    return out;
  }, [list]);

  /* Repeat k sounds what was played k delay times ago, feedback^(k-1) down
     (its drawn level): the note's level plus the repeat's, in dB. */
  const lit = useRef<SVGGElement>(null);
  useLive((signal) => {
    const nodes = lit.current?.children;
    if (!nodes) return;
    const n = Math.min(nodes.length, lights.length);
    for (let i = 0; i < n; i++) {
      const light = lights[i]!;
      const o = Math.max(0, signal.at(light.t) + light.offset) * ENERGY_OPACITY;
      const next = o < 0.004 ? "0" : o.toFixed(3);
      const style = (nodes[i] as SVGElement).style;
      if (style.opacity !== next) style.opacity = next;
    }
  });

  /* Where the second repeat lands and how much of its level reaches the side
     it is drawn on: left at 2t, except in Ping-Pong (right at 2t). */
  const secondOnRight = mode.index === MODE_PING_PONG;
  const secondShare = (() => {
    if (mode.index === MODE_PING_PONG) return (1 + w) / 2;
    if (mode.index === MODE_DUAL && Math.abs(ratioN - 1) > 1e-6) return (1 + w) / 2;
    return 1;
  })();
  const firstShare = secondShare;

  return (
    <Plot flex={1} aria-label="Delay repeats">
      {({ w: pw, h }) => {
        const top = 14;
        const bottom = h - 22;
        const mid = (top + bottom) / 2;
        const half = (bottom - top) / 2;
        /* Bar length for a linear level: −40 dB at the axis, 0 dB at full. */
        const reach = (gain: number): number =>
          gain <= FLOOR_GAIN ? 0 : clamp((20 * Math.log10(gain) - FLOOR_DB) / -FLOOR_DB, 0, 1) * half;
        const gainAt = (dy: number): number =>
          dy <= 0.5 ? 0 : Math.pow(10, (FLOOR_DB * (1 - clamp(dy / half, 0, 1))) / 20);

        let bars = "";
        for (const tap of list) {
          const x = timeX(tap.t, pw).toFixed(1);
          const up = reach(tap.l);
          const down = reach(tap.r);
          if (up > 0) bars += `M${x} ${mid.toFixed(1)}V${(mid - up).toFixed(1)}`;
          if (down > 0) bars += `M${x} ${mid.toFixed(1)}V${(mid + down).toFixed(1)}`;
        }

        const secondReach = reach(fb * secondShare);
        const secondY = secondOnRight ? mid + secondReach : mid - secondReach;

        return (
          <>
            {ticksThatFit(pw).map(([t, text]) => {
              const x = Math.round(timeX(t, pw)) + 0.5;
              return (
                <g key={t}>
                  <line className={p.grid} x1={x} x2={x} y1={0} y2={h} />
                  <text className={p.tick} x={x + 4} y={h - 6}>
                    {text}
                  </text>
                </g>
              );
            })}
            <line className={p.axis} x1={0} x2={pw} y1={Math.round(mid) + 0.5} y2={Math.round(mid) + 0.5} />
            <text className={p.caption} x={4} y={top + 8}>
              L
            </text>
            <text className={p.caption} x={4} y={bottom}>
              R
            </text>
            <g ref={lit}>
              {lights.map((light, i) => {
                const len = reach(light.gain);
                return (
                  <rect
                    key={i}
                    className={p.energy}
                    x={timeX(light.t, pw) - LIT_WIDTH / 2}
                    y={light.up ? mid - len : mid}
                    width={LIT_WIDTH}
                    height={len}
                  />
                );
              })}
            </g>
            <path className={p.live} d={bars} />
            <PlotHandle
              x={timeX(2 * seconds, pw)}
              y={secondY}
              label="Feedback"
              sliders={[feedback]}
              onMove={(_x, y) => {
                const dy = secondOnRight ? y - mid : mid - y;
                const gain = gainAt(dy) / secondShare;
                feedback.setScaled(clamp(gain, feedback.properties.start, feedback.properties.end));
              }}
            />
            <PlotHandle
              x={timeX(seconds, pw)}
              y={mid - reach(firstShare)}
              label="Time"
              sliders={[time]}
              disabled={synced !== null}
              onMove={(x) =>
                time.setScaled(clamp(xTime(x, pw) * 1000, time.properties.start, time.properties.end))
              }
            />
          </>
        );
      }}
    </Plot>
  );
}
