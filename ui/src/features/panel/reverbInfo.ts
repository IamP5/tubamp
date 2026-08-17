/**
 * Copy + lookups for the reverb panel's hover info strip. Pure data — no React.
 *
 * Source of truth for every statement below: docs/REVERB.md §3–5 (including the
 * 2026-08-07 amended blockquotes). If the engine's per-mode semantics change
 * there, this file is the UI surface that rots — update them together.
 *
 * Keys are BASE parameter ids with the `reverb_` prefix stripped (never
 * instance ids — reverb2/reverb3 share the copy), plus `g:*` group captions and
 * `m:<algo>` mode blurbs. The range chunk is DERIVED from `SLIDER_SPEC_BY_ID`
 * and `REVERB_DECAY_CEILING`, never hand-written, so it cannot drift from the
 * APVTS ranges; greyed controls report the literal "inert here" instead.
 */
import type { BlockId, SliderParamId } from "../../bridge";
import { REVERB_ALGO_CHOICES, SLIDER_SPEC_BY_ID } from "../../bridge/paramMeta";
import { forInstance, REVERB_ALGO, REVERB_DECAY_CEILING } from "./knobSpecs";

export type RvInfoKey =
  | "decay"
  | "mix"
  | "size"
  | "shimmer"
  | "shimmer_interval"
  | "predelay"
  | "diffusion"
  | "lowcut"
  | "highcut"
  | "mod"
  | "bassmult"
  | "erlevel"
  | "color"
  | "tilt"
  | "duck"
  | "damping"
  | "width"
  | "g:space"
  | "g:tone"
  | "g:tail"
  | `m:${number}`;

export interface RvInfoLine {
  /** Mono uppercase chip at the left of the strip. */
  key: string;
  /** Plain-English one-liner. */
  text: string;
  /** Derived `min…max · dflt N` chunk, "inert here", "4 choices", or absent. */
  range?: string;
}

/* ── mode blurbs (idle line + segment hover) ─────────────────────────────── */

const MODE_BLURBS: readonly string[] = [
  "the tightest, driest machine — a small 1970s-voiced box; tails read up to 2.5 s.",
  "a dense, metallic-smooth studio plate with a fast bloom; tails read up to 4.5 s.",
  "big, smooth and modern — the longest tails of the six, up to 10 s.",
  "a real spring tank: chirpy, dispersive, drippy. SIZE becomes TANK, DIFF becomes DWELL.",
  "Hall's space plus a pitch-shifted voice regenerating in the tail — turn SHIMMER up.",
  "not a tank: the reverb swells up backwards and cuts off dead. DECAY becomes WINDOW.",
];

/** The current mode's blurb — the strip's idle state, and the preview shown
 *  while hovering an unselected mode segment. */
export function reverbModeLine(algo: number): RvInfoLine {
  const index = algo >= 0 && algo < MODE_BLURBS.length ? algo : REVERB_ALGO.room;
  return { key: REVERB_ALGO_CHOICES[index], text: MODE_BLURBS[index] };
}

/* ── group captions ──────────────────────────────────────────────────────── */

const GROUP_LINES: Readonly<Record<string, RvInfoLine>> = {
  "g:space": {
    key: "SPACE",
    text: "where the sound is — distance, smear, early bounces and stereo spread.",
  },
  "g:tone": {
    key: "TONE",
    text: "what colour the reverb is — what feeds it, and how the machine is voiced.",
  },
  "g:tail": {
    key: "TAIL",
    text: "what the tail does over time — how it fades, wobbles and gets out of your way.",
  },
};

/* ── parameter copy ──────────────────────────────────────────────────────── */

interface RvParamCopy {
  key: string;
  text: string;
  /** Greyed in this mode: the range slot reads "inert here" instead. */
  inert?: boolean;
}

/** Base row per parameter — the copy shown unless a mode overrides it. */
const PARAM_BASE: Readonly<Record<string, RvParamCopy>> = {
  decay: { key: "DECAY", text: "how long the tail rings after you stop playing." },
  mix: {
    key: "MIX",
    text: "dry/wet balance. Start around 25% and back off until the reverb sits behind the note.",
  },
  size: {
    key: "SIZE",
    text: "how big the space feels — bigger spreads the echoes out and slows the build-up.",
  },
  /* Base = the five non-Shimmer modes; Shimmer overrides with the live copy. */
  shimmer: {
    key: "SHIMMER",
    text: "only Shimmer mode has a pitch-shifted voice — switch the mode to bring this to life.",
    inert: true,
  },
  predelay: {
    key: "PRE-DELAY",
    text: "a gap of silence before the reverb starts, so your attack stays clear of the wash.",
  },
  diffusion: {
    key: "DIFFUSION",
    text: "how fast discrete echoes smear into a smooth wash — low = slapback, high = cloud.",
  },
  erlevel: {
    key: "EARLY REFLECTIONS",
    text: "level of the first distinct bounces you hear before the wash arrives.",
  },
  width: {
    key: "WIDTH",
    text: "stereo spread of the wet signal only — 0 collapses it to mono, 1 is full width.",
  },
  lowcut: {
    key: "LOW CUT",
    text: "high-passes what feeds the reverb — lift it to keep boom and mud out of the tail.",
  },
  highcut: {
    key: "HIGH CUT",
    text: "low-passes what feeds the reverb — pull it down to tame fizz from a distorted amp.",
  },
  tilt: {
    key: "TILT",
    text: "tips the wet tone warm or bright around 800 Hz without changing the decay time.",
  },
  color: {
    key: "COLOR",
    text: "ages the machine: 0 is clean and modern, 1 is a duller, grittier vintage box.",
  },
  damping: {
    key: "DAMPING",
    text: "how fast the highs die out compared to the mids — up = darker, faster-fading top.",
  },
  bassmult: {
    key: "BASS MULTIPLIER",
    text: "how much longer the lows ring than the mids — up for a boomier, sustaining low end.",
  },
  mod: {
    key: "MODULATION",
    text: "pitch wobble inside the tail — keeps long decays from ringing on one static tone.",
  },
  duck: {
    key: "DUCKING",
    text: "pushes the wet down while you play and lets it swell back when you stop.",
  },
};

/** Per-mode overrides — the strip key changes exactly where the control's label
 *  changes (WINDOW, TANK, DWELL), and per-mode meaning changes ride along. */
const PARAM_OVERRIDES: Readonly<
  Record<string, Readonly<Record<number, RvParamCopy>>>
> = {
  decay: {
    [REVERB_ALGO.reverse]: {
      key: "WINDOW",
      text: "how long the backwards swell takes to build before it cuts off dead.",
    },
  },
  size: {
    [REVERB_ALGO.spring]: {
      key: "TANK",
      text: "the spring's physical length — longer springs boing longer and smear pitch harder.",
    },
  },
  shimmer: {
    [REVERB_ALGO.shimmer]: {
      key: "SHIMMER",
      text: "level of the pitch-shifted voice in the tail — the classic ethereal choir layer.",
    },
  },
  diffusion: {
    [REVERB_ALGO.spring]: {
      key: "DWELL",
      text: "how hard the signal drives the spring's input — up adds springy grit and overdrive.",
    },
    [REVERB_ALGO.reverse]: {
      key: "DIFFUSION",
      text: "smears the input into the swell — too low reads as separate slaps instead of one rise.",
    },
  },
  erlevel: {
    [REVERB_ALGO.spring]: {
      key: "EARLY REFLECTIONS",
      text: "Spring has no early-reflection network, so this has little effect here — left live per spec.",
    },
    [REVERB_ALGO.reverse]: {
      key: "EARLY REFLECTIONS",
      text: "inert in Reverse: the window IS the early-to-late build, so there is no separate early bus.",
      inert: true,
    },
  },
  color: {
    [REVERB_ALGO.room]: {
      key: "COLOR",
      text: "ages the machine — at full COLOR, Room is the darkest and coarsest of the six, a 1970s box.",
    },
    [REVERB_ALGO.plate]: {
      key: "COLOR",
      text: "ages the machine — Plate's full COLOR lands around a 1980s plate: dirtier than Hall, cleaner than Room.",
    },
    [REVERB_ALGO.hall]: {
      key: "COLOR",
      text: "ages the machine — Hall stays nearly modern even at full COLOR; its grit ceiling is lower.",
    },
    [REVERB_ALGO.shimmer]: {
      key: "COLOR",
      text: "ages the machine — Hall stays nearly modern even at full COLOR; its grit ceiling is lower.",
    },
    [REVERB_ALGO.spring]: {
      key: "COLOR",
      text: "Spring's age is in its physics — COLOR only shifts tone and grit here.",
    },
    [REVERB_ALGO.reverse]: {
      key: "COLOR",
      text: "only the tone and grit axes act here — the swell line has nothing further to degrade.",
    },
  },
  damping: {
    [REVERB_ALGO.reverse]: {
      key: "DAMPING",
      text: "inert in Reverse: there is no recirculating tail to damp.",
      inert: true,
    },
  },
  bassmult: {
    [REVERB_ALGO.reverse]: {
      key: "BASS MULTIPLIER",
      text: "inert in Reverse: there is no recirculating decay for the lows to outlast.",
      inert: true,
    },
  },
  mod: {
    [REVERB_ALGO.reverse]: {
      key: "MODULATION",
      text: "moves the swell's tap positions around, adding subtle motion to the rise.",
    },
  },
};

/* ── derived range chunk ─────────────────────────────────────────────────── */

/**
 * `0…1 · dflt 0.5`, `0.2…30 s · reads to 10.0 s · dflt 2` — computed from the
 * instance's own slider spec (every instance shares its base's range by
 * construction, but the lookup goes through `forInstance` anyway so the strip
 * can never disagree with the control it describes).
 */
function rangeChunk(baseId: SliderParamId, algo: number, block: BlockId): string {
  const spec = SLIDER_SPEC_BY_ID[forInstance(block, baseId)];
  const unit = spec.label ? ` ${spec.label}` : "";
  const span = `${spec.min}…${spec.max}${unit}`;
  if (baseId === "reverb_decay") {
    const ceiling = REVERB_DECAY_CEILING[algo] ?? REVERB_DECAY_CEILING[REVERB_ALGO.room];
    return `${span} · reads to ${ceiling.toFixed(1)} s · dflt ${spec.def}`;
  }
  return `${span} · dflt ${spec.def}`;
}

/* ── the lookup ──────────────────────────────────────────────────────────── */

/** Resolve one hovered `data-rv-key` to the strip's three text slots. */
export function reverbInfoLine(k: RvInfoKey, algo: number, block: BlockId): RvInfoLine {
  if (k.startsWith("m:")) return reverbModeLine(Number(k.slice(2)));
  const group = GROUP_LINES[k];
  if (group) return group;
  if (k === "shimmer_interval") {
    return {
      key: "INTERVAL",
      text: "pitch the shimmer voice is shifted to — +1 Oct is the classic; add the 5th to thicken it.",
      range: "4 choices",
    };
  }
  const copy = PARAM_OVERRIDES[k]?.[algo] ?? PARAM_BASE[k];
  if (!copy) return reverbModeLine(algo);
  return {
    key: copy.key,
    text: copy.text,
    range: copy.inert === true
      ? "inert here"
      : rangeChunk(`reverb_${k}` as SliderParamId, algo, block),
  };
}
