/**
 * Split editor (docs/SPLIT.md §2/§3). The mode sits in the band's header; the
 * body is only about the crossover, because that is the only thing to adjust.
 *
 *   Copy / L/R   one quiet line: what the two lanes carry
 *   X-Over       the crossover drawn (lane Low's low-pass and lane High's
 *                high-pass, Linkwitz-Riley 4th order, crossing at −6 dB) with a
 *                handle at the crossing, beside one large Crossover knob
 *
 * The crossover knob is absent outside X-Over (the engine never reads it
 * there), not dimmed. Lanes are named per mode (features/board/lanes.ts), never
 * coloured. Not instance-aware: split is a singleton.
 */
import { useComboParam, useSliderParam } from "../../hooks";
import { SPLIT_LR, SPLIT_XOVER, laneNames } from "../board/lanes";
import {
  FreqGrid,
  HRules,
  Plot,
  PlotHandle,
  clamp,
  curve,
  dbY,
  freqX,
  highCutDb,
  linePath,
  lowCutDb,
  plot as p,
  useEase,
  xFreq,
} from "./display";
import { ChoiceSegment, Editor, HeaderSlot, K, Row } from "./ui";
import l from "./lanes.module.css";

export function SplitBody() {
  const mode = useComboParam("split_mode").index;
  return (
    <>
      <HeaderSlot>
        <ChoiceSegment id="split_mode" label="Split mode" />
      </HeaderSlot>
      {mode === SPLIT_XOVER ? (
        <Editor display={<CrossoverPlot />}>
          <Row className={l.edge}>
            <K id="split_xover" label="Crossover" size="lg" />
          </Row>
        </Editor>
      ) : (
        <div className={l.note}>
          <p
            className={l.noteText}
            title={mode === SPLIT_LR ? "On a mono track both lanes get the same signal" : undefined}
          >
            {mode === SPLIT_LR
              ? "Lane L carries the left channel, lane R the right"
              : "Both lanes carry the full signal"}
          </p>
        </div>
      )}
    </>
  );
}

/* ───────────────────────────────── display ─────────────────────────────── */

/** Linkwitz-Riley 4th order (the engine's `LinkwitzRileyFilter`): each side is
 *  a 12 dB/oct Butterworth squared, so 24 dB/oct and −6 dB at the crossing. */
const lr4LowDb = (f: number, fc: number): number => 2 * highCutDb(f, fc);
const lr4HighDb = (f: number, fc: number): number => 2 * lowCutDb(f, fc);

function CrossoverPlot() {
  const xover = useSliderParam("split_xover");
  const names = laneNames(SPLIT_XOVER);
  /* Travels (on the log axis it is drawn on) to a value that changed without
     a hand on it; see useEase. */
  const fc = Math.exp(useEase(Math.log(xover.scaled)));
  const TOP = 3;
  const BOTTOM = -30;

  return (
    <Plot aria-label="Crossover">
      {({ w, h }) => {
        const y = (db: number): number => dbY(Math.max(BOTTOM - 6, db), h, TOP, BOTTOM);
        const low = curve(w, (x) => y(lr4LowDb(xFreq(x, w), fc)));
        const high = curve(w, (x) => y(lr4HighDb(xFreq(x, w), fc)));
        const captionY = y(0) + 18;
        return (
          <>
            <FreqGrid w={w} h={h} />
            <HRules w={w} rules={[{ y: y(0), axis: true }]} />
            <path className={p.live} d={linePath(low)} />
            <path className={p.live} d={linePath(high)} />
            <text className={p.caption} x={12} y={captionY}>
              {names.a}
            </text>
            <text className={p.caption} x={w - 12} y={captionY} textAnchor="end">
              {names.b}
            </text>
            <PlotHandle
              x={freqX(fc, w)}
              y={y(-6)}
              label="Crossover"
              sliders={[xover]}
              onMove={(x) =>
                xover.setScaled(clamp(xFreq(x, w), xover.properties.start, xover.properties.end))
              }
            />
          </>
        );
      }}
    </Plot>
  );
}
