/**
 * Reverb editor (docs/REVERB-DELAY.md §1.2). Instance-aware: each reverb block
 * edits its own `<token>_type` and `<token>_*` knobs. Picking a type moves
 * Decay and Damping to that type's starting points (§2.4).
 *
 * Display: the tail — level against time, with the pre-delay gap before it and
 * the faster-decaying highs (damping) drawn as a reference under it — beside
 * the wet signal's low/high cut. Decay, Pre-delay and both cuts can be dragged
 * on the picture.
 *
 * Live: the tail's area brightens with what was just played and gives it back
 * at the reverb's own rate — after the pre-delay, falling 60 dB per Decay — so
 * a long hall visibly holds a chord and a short room lets go of it.
 */
import { useRef } from "react";
import type { BlockId, ComboParamId, SliderParamId } from "../../bridge";
import { useSliderParam } from "../../hooks";
import {
  CutPlot,
  HRules,
  Plot,
  PlotHandle,
  PlotSplit,
  areaPath,
  clamp,
  curve,
  dbY,
  linePath,
  plot as p,
  useEase,
  useLive,
} from "./display";
import { REVERB_STARTING_POINTS, applyStartingPoints } from "./typeDefaults";
import { Editor, Group, HeaderSlot, K, Row, TypeStepper } from "./ui";

export function ReverbBody({ block = "reverb" }: { block?: BlockId }) {
  const id = (key: string): SliderParamId => `${block}_${key}` as SliderParamId;
  return (
    <>
      <HeaderSlot>
        <TypeStepper
          id={`${block}_type` as ComboParamId}
          label="Type"
          onPick={(index) => applyStartingPoints(block, REVERB_STARTING_POINTS, index)}
        />
      </HeaderSlot>
      <Editor
        display={
          <>
            <TailPlot block={block} />
            <PlotSplit />
            <CutPlot lowCut={id("lowcut")} highCut={id("highcut")} flex={0.6} label="Reverb filter" />
          </>
        }
      >
        <Row>
          <K id={id("decay")} label="Decay" size="lg" />
          <K id={id("predelay")} label="Pre-delay" size="lg" />
          <K id={id("mix")} label="Mix" size="lg" />
        </Row>
        <Row>
          <Group>
            <K id={id("size")} label="Size" />
            <K id={id("damping")} label="Damping" />
            <K id={id("mod")} label="Mod" />
            <K id={id("width")} label="Width" />
          </Group>
          <Group>
            <K id={id("lowcut")} label="Low cut" />
            <K id={id("highcut")} label="High cut" />
          </Group>
        </Row>
      </Editor>
    </>
  );
}

/* ───────────────────────────────── display ─────────────────────────────── */

/** Time axis: 0 to 24 s, compressed so the first half second gets a third of
 *  the width (where pre-delay and short rooms live). Inset on the left so a
 *  handle at t = 0 is not cut by the plot's edge. */
const T_MAX = 24;
const T_KNEE = 0.15;
const PAD = 12;
const timeX = (t: number, w: number): number =>
  PAD + (Math.log(1 + clamp(t, 0, T_MAX) / T_KNEE) / Math.log(1 + T_MAX / T_KNEE)) * (w - PAD);
const xTime = (x: number, w: number): number =>
  T_KNEE * (Math.pow(1 + T_MAX / T_KNEE, clamp((x - PAD) / (w - PAD), 0, 1)) - 1);

const TIME_TICKS: readonly (readonly [number, string])[] = [
  [0.1, "0.1 s"],
  [0.5, "0.5 s"],
  [2, "2 s"],
  [8, "8 s"],
];

/** The tail's level in dB, `t` seconds after the dry hit: silent through the
 *  pre-delay, a build-up that lengthens with size, then −60 dB per `decay`. */
function tailDb(t: number, pre: number, decay: number, size: number): number {
  const since = t - pre;
  if (since < 0) return -Infinity;
  const build = 0.004 + 0.06 * size;
  if (since < build) return -60 * (1 - since / build);
  return (-60 * (since - build)) / decay;
}

/** The tail's area at full input: enough to see, well short of a flash. */
const ENERGY_OPACITY = 0.28;

function TailPlot({ block }: { block: BlockId }) {
  const decay = useSliderParam(`${block}_decay` as SliderParamId);
  const predelay = useSliderParam(`${block}_predelay` as SliderParamId);
  const size = useSliderParam(`${block}_size` as SliderParamId);
  const damping = useSliderParam(`${block}_damping` as SliderParamId);

  /* What is drawn travels to a value that changed without a hand on it (a
     type's starting points, a preset); see useEase. */
  const decayS = useEase(decay.scaled);
  const sizeN = useEase(size.scaled);
  const pre = useEase(predelay.scaled) / 1000;
  /* What damping does to the highs, as a decay time: none = the full tail,
     full = a quarter of it. A picture of the direction, not a measurement. */
  const highDecay = decayS * (1 - 0.75 * useEase(damping.scaled));

  /* The tail answering the input. Levels are dB-linear over a 48 dB window, so
     60 dB per Decay is a straight fall of 60/48 per Decay second. */
  const energy = useRef<SVGPathElement>(null);
  const held = useRef(0);
  useLive((signal, dt) => {
    held.current = Math.max(signal.at(pre), held.current - (dt * 60) / 48 / Math.max(decayS, 0.05));
    if (energy.current) energy.current.style.opacity = (held.current * ENERGY_OPACITY).toFixed(3);
  });

  return (
    <Plot flex={1} aria-label="Reverb tail">
      {({ w, h }) => {
        const TOP = 6;
        const FLOOR = -60;
        const y = (db: number): number => dbY(Math.max(db, FLOOR), h, TOP, FLOOR);
        /* From the onset to where the tail reaches the floor: no ink along the
           silent stretches either side. */
        const tail = (time: number): [number, number][] => {
          const build = 0.004 + 0.06 * sizeN;
          const from = timeX(pre, w);
          const to = Math.min(w, timeX(pre + build + time, w));
          const points = curve(to - from, (dx) => y(tailDb(xTime(from + dx, w) + 1e-6, pre, time, sizeN)));
          return points.map(([dx, py]) => [from + dx, py]);
        };
        const main = tail(decayS);
        const highs = tail(highDecay);
        return (
          <>
            {TIME_TICKS.map(([t, text]) => {
              const x = Math.round(timeX(t, w)) + 0.5;
              return (
                <g key={t}>
                  <line className={p.grid} x1={x} x2={x} y1={0} y2={h} />
                  <text className={p.tick} x={x + 4} y={h - 6}>
                    {text}
                  </text>
                </g>
              );
            })}
            <HRules w={w} rules={[{ y: y(0), axis: true }]} />
            <path className={p.fill} d={areaPath(main, h)} />
            <path ref={energy} className={p.energy} d={areaPath(main, h)} />
            <path className={p.ghost} d={linePath(highs)} />
            <path className={p.live} d={linePath(main)} />
            <PlotHandle
              x={timeX(pre, w)}
              y={y(0)}
              label="Pre-delay"
              sliders={[predelay]}
              onMove={(x) => predelay.setScaled(clamp(xTime(x, w) * 1000, 0, predelay.properties.end))}
            />
            <PlotHandle
              x={timeX(pre + decayS * 0.5, w)}
              y={y(-30)}
              label="Decay"
              sliders={[decay]}
              onMove={(x) =>
                decay.setScaled(
                  clamp((xTime(x, w) - pre) * 2, decay.properties.start, decay.properties.end),
                )
              }
            />
          </>
        );
      }}
    </Plot>
  );
}
