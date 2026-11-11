/**
 * Splitter body (docs/SPLIT.md §2/§5): what the split does, shown rather than
 * named.
 *
 *   [ flow diagram ]  [ Copy | L/R | X-Over ]      [ X-OVER knob ]
 *                     one line: what each lane gets  (X-Over only)
 *
 * The mode is a 3-way segmented control bound to `split_mode`; the sentence
 * under it and the diagram beside it follow the mode, and both name the lanes
 * the way the board's badges do (features/board/lanes.ts). The crossover knob
 * only exists in X-Over mode — it is meaningless in the other two — and grows
 * in rather than popping. Not instance-aware: split is a singleton.
 */
import { AnimatePresence, motion, useReducedMotion } from "motion/react";
import { Knob, Segmented } from "../../components";
import { useComboParam } from "../../hooks";
import {
  LANE_TINT,
  SPLIT_COPY,
  SPLIT_LR,
  SPLIT_XOVER,
  formatHz,
  laneNames,
  modeSummary,
  useXoverHz,
} from "../board/lanes";
import { duration, ease } from "../../theme/motion";
import l from "./lanes.module.css";

/** Frozen choice order (docs/SPLIT.md §2) — the fallback before the async
 *  properties push lands. */
const FALLBACK_CHOICES = ["Copy", "L/R", "X-Over"];

export function SplitBody() {
  const mode = useComboParam("split_mode");
  const xover = useXoverHz();
  const reduced = useReducedMotion() ?? false;
  const choices = mode.choices.length === 3 ? mode.choices : FALLBACK_CHOICES;
  const index = mode.index;

  return (
    <div className={l.splitBody}>
      <FlowDiagram mode={index} xover={xover} />

      <div className={l.splitControls} data-param-index={mode.parameterIndex}>
        <span className={l.microLabel}>Mode</span>
        <Segmented
          aria-label="Split mode"
          className={l.modeSegmented}
          value={String(index)}
          options={choices.map((label, i) => ({ value: String(i), label }))}
          onChange={(value) => mode.setIndex(Number(value))}
        />
        <p className={l.summary}>{modeSummary(index, xover)}</p>
      </div>

      <AnimatePresence initial={false}>
        {index === SPLIT_XOVER && (
          <motion.div
            key="xover"
            className={l.xoverCell}
            initial={reduced ? { opacity: 0 } : { opacity: 0, width: 0 }}
            animate={reduced ? { opacity: 1 } : { opacity: 1, width: 112 }}
            exit={reduced ? { opacity: 0 } : { opacity: 0, width: 0 }}
            transition={{ duration: reduced ? 0 : duration.fast, ease: ease.out }}
          >
            <Knob id="split_xover" label="X-Over" format={formatHz} />
          </motion.div>
        )}
      </AnimatePresence>
    </div>
  );
}

/* ─────────────────────────────── diagram ───────────────────────────────── */

const W = 196;
const H = 104;
const IN_X = 16;
const TRACK_A = 26;
const TRACK_B = 78;
const MID = H / 2;
const END_X = 150;

/** The fork from one input into both lanes. */
function forkPath(track: number): string {
  return `M${IN_X + 6} ${MID}C${IN_X + 46} ${MID} ${IN_X + 40} ${track} ${IN_X + 74} ${track}H${END_X}`;
}

/** A small filter response sketched on a lane: low-pass falls, high-pass rises. */
function responsePath(track: number, low: boolean): string {
  const x0 = 98;
  const x1 = 138;
  const top = track - 10;
  const bottom = track + 6;
  return low
    ? `M${x0} ${top}H${x0 + 18}C${x0 + 26} ${top} ${x0 + 30} ${bottom} ${x1} ${bottom}`
    : `M${x0} ${bottom}C${x0 + 10} ${bottom} ${x0 + 14} ${top} ${x0 + 22} ${top}H${x1}`;
}

/**
 * IN → the two lanes, drawn for the current mode: Copy forks one signal into
 * both; L/R runs the input's two channels straight into one lane each; X-Over
 * forks and filters — lows to lane A, highs to lane B. Lane ends carry the same
 * badges as the board.
 */
function FlowDiagram({ mode, xover }: { mode: number; xover: number }) {
  const names = laneNames(mode);
  const a = LANE_TINT.a;
  const b = LANE_TINT.b;

  return (
    <svg
      className={l.diagram}
      width={W}
      height={H}
      viewBox={`0 0 ${W} ${H}`}
      role="img"
      aria-label={modeSummary(mode, xover)}
    >
      <AnimatePresence initial={false}>
        <motion.g
          key={mode}
          initial={{ opacity: 0 }}
          animate={{ opacity: 1 }}
          exit={{ opacity: 0 }}
          transition={{ duration: duration.fast, ease: ease.out }}
        >
          {mode === SPLIT_LR ? (
            <>
              <path d={`M${IN_X + 6} ${TRACK_A}H${END_X}`} className={l.diagramWire} style={{ stroke: a.wire }} />
              <path d={`M${IN_X + 6} ${TRACK_B}H${END_X}`} className={l.diagramWire} style={{ stroke: b.wire }} />
              <circle cx={IN_X} cy={TRACK_A} r={5} className={l.diagramIn} />
              <circle cx={IN_X} cy={TRACK_B} r={5} className={l.diagramIn} />
              <text x={IN_X} y={TRACK_A - 10} className={l.diagramText}>L IN</text>
              <text x={IN_X} y={TRACK_B + 17} className={l.diagramText}>R IN</text>
            </>
          ) : (
            <>
              <path d={forkPath(TRACK_A)} className={l.diagramWire} style={{ stroke: a.wire }} />
              <path d={forkPath(TRACK_B)} className={l.diagramWire} style={{ stroke: b.wire }} />
              <circle cx={IN_X} cy={MID} r={5} className={l.diagramIn} />
              <text x={IN_X} y={MID + 18} className={l.diagramText}>IN</text>
            </>
          )}

          {mode === SPLIT_XOVER && (
            <>
              <rect x={94} y={TRACK_A - 14} width={48} height={24} rx={5} className={l.diagramFilter} />
              <rect x={94} y={TRACK_B - 14} width={48} height={24} rx={5} className={l.diagramFilter} />
              <path d={responsePath(TRACK_A, true)} className={l.diagramCurve} style={{ stroke: a.ink }} />
              <path d={responsePath(TRACK_B, false)} className={l.diagramCurve} style={{ stroke: b.ink }} />
              <text x={118} y={MID + 4} className={l.diagramText}>
                {formatHz(xover)}
              </text>
            </>
          )}
          {mode === SPLIT_COPY && (
            <text x={98} y={MID + 4} className={l.diagramText}>
              SAME SIGNAL
            </text>
          )}
        </motion.g>
      </AnimatePresence>

      <LaneEnd y={TRACK_A} name={names.a} side="a" />
      <LaneEnd y={TRACK_B} name={names.b} side="b" />
    </svg>
  );
}

/** A lane badge at the end of a diagram track — the board's badge, in SVG. */
function LaneEnd({ y, name, side }: { y: number; name: string; side: "a" | "b" }) {
  const tint = LANE_TINT[side];
  const w = 14 + name.length * 6.4;
  return (
    <g>
      <rect
        x={END_X + 2}
        y={y - 9}
        width={w}
        height={18}
        rx={9}
        fill={tint.fill}
        stroke={tint.ring}
        strokeWidth={1}
      />
      <text
        x={END_X + 2 + w / 2}
        y={y + 3.5}
        className={l.diagramBadge}
        style={{ fill: tint.ink }}
      >
        {name}
      </text>
    </g>
  );
}
