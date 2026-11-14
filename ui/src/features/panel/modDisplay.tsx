/**
 * The modulation block's LFO, drawn over two cycles (ui/DESIGN.md §Editors,
 * Modulation).
 *
 * The wave is the one the current type and voicing run (docs/FX-ENGINES.md
 * §4.5–4.7, src/dsp/Chorus.h, Phaser.cpp, Tremolo.cpp), its swing is the depth,
 * and where the voicing is stereo a ghost copy shows the right channel's LFO,
 * offset by its phase at the current width. A marker travels along the curve
 * at the real rate (the synced note value at 120 BPM when synced: the UI is not
 * told the host tempo). Under reduced motion the picture is static.
 *
 * Every value drawn is eased (useEase), so a type, voicing or preset moves the
 * wave to its new shape. Live: the area under the wave brightens with the
 * input level times the swing actually applied, so a deep setting visibly
 * works on what is played and the picture is quiet in silence (only the rate
 * marker moves then).
 */
import { useEffect, useLayoutEffect, useMemo, useRef } from "react";
import { useReducedMotion } from "motion/react";
import { onFrame } from "../../hooks";
import { Plot, areaPath, clamp, curve, linePath, plot as p, useEase, useLive } from "./display";
import m from "./modDisplay.module.css";

export const MOD_CHORUS = 0;
export const MOD_PHASER = 1;
export const MOD_TREMOLO = 2;

export interface LfoInput {
  kind: number;
  /** Voicing index within the kind (chorus mode, phaser model, trem voice). */
  voice: number;
  rateHz: number;
  depth: number;
  width: number;
  shape: number;
  wave: number;
}

interface Lfo {
  /** Value in [-1, 1] at `phase` cycles (1 = top of the swing). */
  at(phase: number): number;
  /** Swing as a share of the plot's half height. */
  swing: number;
  /** Right channel's phase lead in cycles, or null where both channels share one LFO. */
  right: number | null;
}

const frac = (x: number): number => x - Math.floor(x);
const sine = (ph: number): number => Math.sin(2 * Math.PI * ph);
/** 0 at phase 0, 1 at half a cycle. */
const raised = (ph: number): number => 0.5 - 0.5 * Math.cos(2 * Math.PI * ph);
const triangle = (ph: number): number => 1 - Math.abs(2 * frac(ph) - 1);

/** Phaser.cpp `rcTriangle`: halves are RC charge curves, phase 0 at the bottom. */
function rcTriangle(ph: number, kappa: number): number {
  const q = frac(ph);
  const rising = q < 0.5;
  const w = rising ? 2 * q : 2 * q - 1;
  const s = (1 - Math.exp(-kappa * w)) / (1 - Math.exp(-kappa));
  return rising ? 2 * s - 1 : 1 - 2 * s;
}

/** Tremolo Smooth's shape w in [0, 1] (FX-ENGINES §4.7.5). */
function smoothShape(ph: number, shape: number, wave: number): number {
  const q = frac(ph);
  const s = 0.1 + 0.8 * shape;
  const w0 = q < s ? (0.5 * q) / s : 0.5 + (0.5 * (q - s)) / (1 - s);
  const sn = raised(w0);
  const tri = 1 - Math.abs(2 * w0 - 1);
  const pulse = frac(q - s / 2) < s ? 1 : 0;
  return wave <= 0.5 ? sn + (tri - sn) * 2 * wave : tri + (pulse - tri) * (2 * wave - 1);
}

/** Tremolo Optical: the lamp (lit the last quarter) through the photocell's
 *  two-rate lag, run to its periodic state at `rateHz`. Returns 512 samples of
 *  w in [0, 1] (1 = loud) over one cycle. */
function opticalCycle(rateHz: number): Float32Array {
  const n = 512;
  const dt = 1 / (Math.max(0.01, rateHz) * n);
  const fall = 1 - Math.exp(-2 * Math.PI * 31.83 * dt);
  const rise = 1 - Math.exp(-2 * Math.PI * 7.96 * dt);
  const out = new Float32Array(n);
  let g = 1;
  for (let cycle = 0; cycle < 6; cycle++) {
    for (let i = 0; i < n; i++) {
      const target = i / n >= 0.75 ? 0 : 1;
      g += (target - g) * (target < g ? fall : rise);
      if (cycle === 5) out[i] = g;
    }
  }
  return out;
}

function lfoFor(input: LfoInput, optical: Float32Array): Lfo {
  const { kind, voice, rateHz, depth, width, shape, wave } = input;
  if (kind === MOD_CHORUS) {
    switch (voice) {
      case 1: // Wide: triangle, right voice a quarter cycle on
        return { at: (ph) => 2 * triangle(ph) - 1, swing: depth, right: 0.25 * width };
      case 2: // CE: bent triangle; the swing shrinks as 1/f above 2.86 Hz
        return {
          at: (ph) => {
            const u = triangle(ph);
            return 2 * u * (1 + 0.1 * (u - 1)) - 1;
          },
          swing: depth * Math.min(1, 2.86 / Math.max(rateHz, 1e-3)),
          right: 0.25 * width,
        };
      case 3: // Dimension: one triangle, the other line in antiphase
        return { at: (ph) => 2 * triangle(ph) - 1, swing: depth, right: 0.5 * width };
      case 4: {
        // Tri: slow sine plus the fixed 6 Hz, 3-cent shimmer (cents law: the
        // delay swing goes as cents / rate, against 20 cents × depth for the slow one)
        const ripple = Math.min(0.5, rateHz / (40 * Math.max(depth, 0.01)));
        const ratio = 6 / Math.max(rateHz, 1e-3);
        return {
          at: (ph) => (2 * raised(ph) - 1 + ripple * (2 * raised(ph * ratio) - 1)) / (1 + ripple),
          swing: depth,
          right: width / 3,
        };
      }
      default: // Classic: one sine shared by both channels
        return { at: sine, swing: depth, right: null };
    }
  }
  if (kind === MOD_PHASER) {
    /* Phase 90 and Small Stone run at the pedals' own depth, one LFO for both. */
    if (voice === 1) return { at: (ph) => rcTriangle(ph, -0.007), swing: 1, right: null };
    if (voice === 2) return { at: (ph) => rcTriangle(ph, 0.702), swing: 1, right: null };
    return { at: sine, swing: depth, right: null };
  }
  /* Tremolo: right channel spread = 180° × width (voices 1–4; Classic is mono). */
  const spread = width / 2;
  switch (voice) {
    case 1: {
      const exponent = 1 - spread; // Smooth's loudness law on both channels
      return {
        at: (ph) => 2 * Math.pow(smoothShape(ph, shape, wave), exponent) - 1,
        swing: depth,
        right: spread,
      };
    }
    case 2:
      return {
        at: (ph) => 2 * optical[Math.floor(frac(ph) * optical.length)]! - 1,
        swing: 0.9 * depth,
        right: spread,
      };
    case 3: // Bias: cos LFO, dip at phase 0
    case 4: // Harmonic: the low band's gain (the high band moves in antiphase)
      return { at: (ph) => 2 * raised(ph) - 1, swing: depth, right: spread };
    default: // Classic: 0.5 + 0.5 sin, dip at 0.75
      return { at: sine, swing: depth, right: null };
  }
}

const CYCLES = 2;
const PAD_X = 12;
const PAD_Y = 16;
/** The wave's area at full input and full swing: enough to see, short of a flash. */
const ENERGY_OPACITY = 0.32;

export function LfoPlot(props: LfoInput) {
  const reduced = useReducedMotion() ?? false;
  /* What is drawn travels to a value that changed without a hand on it; the
     kind and voicing are choices and switch the wave's law at once. */
  const eased: LfoInput = {
    kind: props.kind,
    voice: props.voice,
    rateHz: useEase(props.rateHz),
    depth: useEase(props.depth),
    width: useEase(props.width),
    shape: useEase(props.shape),
    wave: useEase(props.wave),
  };
  /* The lamp's lag depends on the rate; recomputed only once the rate is at rest. */
  const optical = useMemo(() => opticalCycle(props.rateHz), [props.rateHz]);
  const lfo = lfoFor(eased, optical);

  /* The modulation working on what is played: input level × applied swing. */
  const energy = useRef<SVGPathElement>(null);
  const swing = clamp(lfo.swing, 0, 1);
  useLive((signal) => {
    if (energy.current) energy.current.style.opacity = (signal.now() * swing * ENERGY_OPACITY).toFixed(3);
  });

  return (
    <Plot flex={1} aria-label="Modulation LFO">
      {({ w, h }) => {
        const mid = h / 2;
        const half = Math.max(0, h / 2 - PAD_Y) * Math.min(1, Math.max(0, lfo.swing));
        const span = w - 2 * PAD_X;
        const point = (phase: number, lead = 0): [number, number] => [
          PAD_X + (phase / CYCLES) * span,
          mid - lfo.at(phase + lead) * half,
        ];
        const trace = (lead: number): [number, number][] =>
          curve(span, (x) => point((x / span) * CYCLES, lead)[1], 1).map(([x, y]) => [x + PAD_X, y]);
        const live = trace(0);
        const showRight = lfo.right !== null && lfo.right > 0.002;
        return (
          <>
            <line className={p.axis} x1={0} x2={w} y1={Math.round(mid) + 0.5} y2={Math.round(mid) + 0.5} />
            {showRight && <path className={p.ghost} d={linePath(trace(lfo.right!))} />}
            <path className={p.fill} d={areaPath(live, mid)} />
            <path ref={energy} className={p.energy} d={areaPath(live, mid)} />
            <path className={p.live} d={linePath(live)} />
            {!reduced && <LfoMarker rateHz={eased.rateHz} point={point} />}
          </>
        );
      }}
    </Plot>
  );
}

/** The phase marker: moved per frame by direct DOM writes, never a re-render. */
function LfoMarker({
  rateHz,
  point,
}: {
  rateHz: number;
  point: (phase: number) => [number, number];
}) {
  const dot = useRef<SVGCircleElement | null>(null);
  const latest = useRef({ rateHz, point });
  useLayoutEffect(() => {
    latest.current = { rateHz, point };
  });

  useEffect(() => {
    let phase = 0;
    return onFrame((dt) => {
      const el = dot.current;
      if (!el) return;
      phase = (phase + dt * latest.current.rateHz) % CYCLES;
      const [x, y] = latest.current.point(phase);
      el.setAttribute("transform", `translate(${x.toFixed(1)} ${y.toFixed(1)})`);
    });
  }, []);

  const [x0, y0] = point(0);
  return <circle ref={dot} className={m.marker} r={3.5} transform={`translate(${x0} ${y0})`} />;
}
