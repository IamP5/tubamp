/**
 * Gate editor (docs/FX-ENGINES.md §4.1). Classic reads only Threshold; Tight
 * and Smooth add Range, Hold, Release and Sidechain HPF (dimmed in place in
 * Classic, so the layout holds still). Picking a mode moves Release, Hold and
 * Sidechain HPF to that mode's starting points.
 *
 * Display (after FabFilter Pro-G): the last four seconds of level scrolling
 * right to left — the detector level as a dim area, what passes the gate
 * (level less the gain reduction) as a meter-ink line — under the threshold, a
 * line you drag. Tight and Smooth close 6 dB under it (Gate.h), drawn as a
 * reference. The gain reduction itself is the bar at the right edge, with the
 * range (the attenuation when closed) marked on its scale.
 */
import { useEffect, useRef } from "react";
import { useReducedMotion } from "motion/react";
import { bridge, type BlockId, type ComboParamId, type SliderParamId } from "../../bridge";
import {
  GATE_GR_SCALE_DB,
  GATE_LEVEL_MAX_DB,
  GATE_LEVEL_MIN_DB,
  onFrame,
  useComboParam,
  useDynMeters,
  useSliderParam,
} from "../../hooks";
import { HRules, Plot, PlotHandle, clamp, lerp, plot as p, useEase } from "./display";
import { DYN_PAD_Y, GrMeter } from "./DynMeters";
import { GATE_STARTING_POINTS, applyStartingPoints } from "./typeDefaults";
import { Editor, Group, HeaderSlot, K, Row, TypeStepper } from "./ui";
import d from "./dyn.module.css";

const CLASSIC = 0;
/** Tight and Smooth close this far under the threshold (Gate.h). */
const HYSTERESIS_DB = 6;

export function GateBody({ block = "gate" }: { block?: BlockId }) {
  const id = (key: string): SliderParamId => `${block}_${key}` as SliderParamId;
  const mode = useComboParam(`${block}_mode` as ComboParamId);
  const classic = mode.index === CLASSIC;

  return (
    <>
      <HeaderSlot>
        <TypeStepper
          id={`${block}_mode` as ComboParamId}
          label="Mode"
          onPick={(index) => applyStartingPoints(block, GATE_STARTING_POINTS, index)}
        />
      </HeaderSlot>
      <Editor display={<GateDisplay block={block} classic={classic} />}>
        <Row>
          <K
            id={id("threshold")}
            label="Threshold"
            size="lg"
            tip={
              classic
                ? "Classic compares a mean-square level with the threshold"
                : "The threshold is the peak level of the filtered sidechain, in dBFS"
            }
          />
          <K id={id("range")} label="Range" size="lg" disabled={classic} />
        </Row>
        <Row>
          <Group>
            <K id={id("hold")} label="Hold" disabled={classic} />
            <K id={id("release")} label="Release" disabled={classic} />
            <div className={d.lastWide}>
              <K id={id("sc_hpf")} label="Sidechain HPF" disabled={classic} />
            </div>
          </Group>
        </Row>
      </Editor>
    </>
  );
}

/* ───────────────────────────────── display ─────────────────────────────── */

const HISTORY_S = 4;
/** dynMeters arrive at 30 Hz (docs/FX-ENGINES.md §2.8): one point per reading. */
const RATE_HZ = 30;
const N = HISTORY_S * RATE_HZ;
const DB_RULES = [-20, -40, -60, -80] as const;
const GR_TICKS = [0, 20, 40, 60] as const;

function GateDisplay({ block, classic }: { block: BlockId; classic: boolean }) {
  const meters = useDynMeters();
  const threshold = useSliderParam(`${block}_threshold` as SliderParamId);
  const range = useSliderParam(`${block}_range` as SliderParamId);
  /* What is drawn travels to a value set without a hand on it (a mode's
     starting points, a preset); see useEase. */
  const thresholdDb = useEase(threshold.scaled);
  const rangeDb = useEase(range.scaled);
  const reduced = useReducedMotion() ?? false;
  const level = useRef<SVGPathElement | null>(null);
  const passed = useRef<SVGPathElement | null>(null);

  /* Two ring buffers of the raw readings (level, level less reduction), one
     point per dynMeters event. The meters' smoothed values would draw their own
     fall time into the trace (a gate opening over half a second), so the trace
     reads the event itself; between readings it slides by the time elapsed. */
  useEffect(() => {
    if (reduced) return;
    const levels = new Float32Array(N).fill(-120);
    const outs = new Float32Array(N).fill(-120);
    let head = 0;
    let lastAt = 0;
    let settled = false;
    const stopEvents = bridge.on("dynMeters", ({ gate }) => {
      levels[head] = gate.level;
      outs[head] = gate.level - Math.max(0, gate.gr);
      head = (head + 1) % N;
      lastAt = performance.now();
      settled = false;
    });
    const stopFrames = onFrame((_, now) => {
      const area = level.current;
      const line = passed.current;
      if (!area || !line || settled) return;
      const frac = Math.min(1, (now - lastAt) * (RATE_HZ / 1000));
      settled = frac >= 1;
      const w = Number(area.dataset.w);
      const h = Number(area.dataset.h);
      const step = w / (N - 2);
      let a = `M0 ${h}`;
      let l = "";
      let yLevel = h;
      let yOut = h;
      for (let i = 0; i < N; i += 1) {
        const k = (head + i) % N;
        const x = Math.max(0, w - (N - 1 - i + frac) * step).toFixed(1);
        yLevel = levelY(levels[k]!, h);
        yOut = levelY(outs[k]!, h);
        a += `L${x} ${yLevel.toFixed(1)}`;
        l += `${i === 0 ? "M" : "L"}${x} ${yOut.toFixed(1)}`;
      }
      /* Hold the newest reading out to the right edge. */
      area.setAttribute("d", `${a}L${w} ${yLevel.toFixed(1)}L${w} ${h}Z`);
      line.setAttribute("d", `${l}L${w} ${yOut.toFixed(1)}`);
    });
    return () => {
      stopEvents();
      stopFrames();
    };
  }, [reduced]);

  return (
    <>
      <Plot aria-label="Gate level">
        {({ w, h }) => {
          const yThr = levelY(thresholdDb, h);
          const yClose = levelY(thresholdDb - HYSTERESIS_DB, h);
          return (
            <>
              <HRules w={w} rules={DB_RULES.map((db) => ({ y: levelY(db, h), label: `−${-db} dB` }))} />
              <path ref={level} className={p.level} d="" data-w={w} data-h={h} />
              <path ref={passed} className={p.meterLine} d="" />
              {!classic && <line className={p.ghost} x1={0} x2={w} y1={yClose} y2={yClose} />}
              <line className={p.live} x1={0} x2={w} y1={yThr} y2={yThr} />
              <PlotHandle
                x={w - 20}
                y={yThr}
                label="Threshold"
                sliders={[threshold]}
                onMove={(_, y) =>
                  threshold.setScaled(
                    clamp(
                      lerp(y, DYN_PAD_Y, h - DYN_PAD_Y, GATE_LEVEL_MAX_DB, GATE_LEVEL_MIN_DB),
                      threshold.properties.start,
                      threshold.properties.end,
                    ),
                  )
                }
              />
            </>
          );
        }}
      </Plot>
      <GrMeter
        value={meters.gateGr}
        scaleDb={GATE_GR_SCALE_DB}
        ticks={GR_TICKS}
        floorDb={classic ? undefined : -rangeDb}
      />
    </>
  );
}

/** Level (dBFS) to y on the gate meter's window, inset so 0 dB and the
 *  threshold handle at the top stay whole. */
function levelY(db: number, h: number): number {
  const clamped = clamp(db, GATE_LEVEL_MIN_DB, GATE_LEVEL_MAX_DB);
  return lerp(clamped, GATE_LEVEL_MAX_DB, GATE_LEVEL_MIN_DB, DYN_PAD_Y, h - DYN_PAD_Y);
}
