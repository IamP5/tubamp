/**
 * Generic knob rows per block — the React port of `knobSpecsFor()`
 * (docs/research/current-ui-inventory.md §BlockParamPanel, "Knob specs per
 * block"). Ids and labels are verbatim, and so is the display order.
 *
 * Written once per BASE and mapped onto all 28 tokens: an instance's row is its
 * kind's row with the id prefix swapped (`comp_ratio` → `comp2_ratio`), which is
 * exactly how Parameters.cpp mints the instance parameters.
 *
 * `amp` is deliberately empty: every amp control lives in AmpBody, exactly like
 * the native panel; `amp2`'s row (docs/SPLIT.md §5) holds only its IN/OUT pair
 * for the same reason — model B management lives in Amp2Body. `cab`, `mod`,
 * `delay`, `split` and `mix` keep their generic row *and* get an extra body
 * above it (IR row / type combo / mode combo / phase toggle). `reverb` leaves
 * the table entirely: its rows depend on `reverb_algo` too, so it is built by
 * the `(kind, algo)` functions at the bottom of this file (docs/REVERB.md
 * §5.2). The fx slots are
 * empty too — a hosted plugin's parameters are discovered at load time, so they
 * can never appear in a static table of tubamp's own slider ids (FxSlotBody
 * renders them instead). `lane2` is empty because it never reaches a panel.
 */
import {
  blockRecord,
  instanceOf,
  kindOf,
  type BaseBlockId,
  type BlockId,
  type SliderParamId,
} from "../../bridge";

export interface KnobSpec {
  id: SliderParamId;
  /** Micro-label under the dial (rendered uppercase). */
  label: string;
  /**
   * Greyed and non-interactive: the parameter exists but has no meaning in the
   * current mode (docs/REVERB.md §5.2 — a silently inert knob is worse than a
   * greyed one). Only the reverb's `(kind, algo)` specs set it today.
   */
  disabled?: boolean;
  /**
   * Ceiling for the READOUT only, in the parameter's own units. The knob still
   * travels its full APVTS range — the parameter is genuinely still moving, and
   * clamping travel would need a write on every mode change — but the number
   * under it stops where the DSP stops (docs/REVERB.md §5: "the UI shows the
   * clamped value"). Only `reverb_decay` uses it, whose ceiling is per-mode.
   */
  displayMax?: number;
}

const BASE_KNOB_SPECS: Record<BaseBlockId, readonly KnobSpec[]> = {
  gate: [{ id: "gate_threshold", label: "THRESH" }],
  comp: [
    { id: "comp_threshold", label: "THRESH" },
    { id: "comp_ratio", label: "RATIO" },
    { id: "comp_attack", label: "ATTACK" },
    { id: "comp_release", label: "RELEASE" },
    { id: "comp_makeup", label: "MAKEUP" },
  ],
  drive: [
    { id: "drive_gain", label: "GAIN" },
    { id: "drive_tone", label: "TONE" },
    { id: "drive_level", label: "LEVEL" },
  ],
  /* All amp controls live in AmpBody. */
  amp: [],
  cab: [
    { id: "cab_lowcut", label: "LO CUT" },
    { id: "cab_highcut", label: "HI CUT" },
  ],
  eq: [
    { id: "eq_bass", label: "BASS" },
    { id: "eq_mid", label: "MID" },
    { id: "eq_treble", label: "TREBLE" },
  ],
  mod: [
    { id: "mod_rate", label: "RATE" },
    { id: "mod_depth", label: "DEPTH" },
    { id: "mod_mix", label: "MIX" },
  ],
  /* ratio + width appended per docs/STEREO.md §4; ratio only bites in Dual mode
     but stays visible (and turnable) in every mode, per spec. */
  delay: [
    { id: "delay_time", label: "TIME" },
    { id: "delay_feedback", label: "FDBK" },
    { id: "delay_mix", label: "MIX" },
    { id: "delay_ratio", label: "RATIO" },
    { id: "delay_width", label: "WIDTH" },
  ],
  /* Empty on purpose: the reverb's controls depend on `reverb_algo` as well as
     on the block (docs/REVERB.md §5.2), which a static per-token table cannot
     express — ReverbBody reads `reverbPrimarySpecs`/`REVERB_GROUPS` below
     instead, and the panel dispatches it before the generic row. */
  reverb: [],
  /* Hosted plugins bring their own parameters — see FxSlotBody. */
  fx1: [],
  fx2: [],
  fx3: [],
  /* docs/SPLIT.md §5 — amp2 mirrors amp's own IN/OUT pair; every other amp2
     control (model B) lives in Amp2Body, exactly like AmpBody. */
  amp2: [
    { id: "amp2_input", label: "IN" },
    { id: "amp2_output", label: "OUT" },
  ],
  /* SplitBody puts the mode combo above this — see DelayBody/ModBody. */
  split: [{ id: "split_xover", label: "X-OVER" }],
  /* MixBody puts the B-phase toggle above this — see DelayBody/ModBody. */
  mix: [
    { id: "mix_alevel", label: "A LEVEL" },
    { id: "mix_apan", label: "A PAN" },
    { id: "mix_blevel", label: "B LEVEL" },
    { id: "mix_bpan", label: "B PAN" },
    { id: "mix_level", label: "MASTER" },
  ],
  /* lane2 never reaches a panel — see BlockBody's guard in features/panel. */
  lane2: [],
};

/** `comp2` + `comp_ratio` → `comp2_ratio`; instance 1 keeps the legacy id. Safe
 *  by construction: the pooled instance ids in bridge/types are minted from the
 *  same kind/key pairs. Exported for the reverb body's group rows and info
 *  strip, which both key their tables on BASE ids and map at render time. */
export function forInstance(block: BlockId, id: SliderParamId): SliderParamId {
  if (instanceOf(block) === 0) return id;
  return `${block}${id.slice(kindOf(block).length)}` as SliderParamId;
}

export const KNOB_SPECS: Record<BlockId, readonly KnobSpec[]> = blockRecord((id) => {
  const base = BASE_KNOB_SPECS[kindOf(id)];
  if (instanceOf(id) === 0) return base;
  return base.map((spec) => ({ ...spec, id: forInstance(id, spec.id) }));
});

/* ────────────────── reverb, keyed by (kind, algo) — §5.2 ────────────────── */

/**
 * `reverb_algo`'s frozen choice order (docs/REVERB.md §4). The list itself is
 * the backend's (`ComboProperties.choices`); these are the indices the UI has
 * to reason about.
 */
export const REVERB_ALGO = {
  room: 0,
  plate: 1,
  hall: 2,
  spring: 3,
  shimmer: 4,
  reverse: 5,
} as const;

/**
 * Algo index → the `data-reverb-algo` attribute value ReverbBody stamps on its
 * root, keying the per-mode accent palettes in theme/tokens.css. Same frozen
 * order as `REVERB_ALGO_CHOICES`.
 */
export const REVERB_ALGO_KEYS: readonly string[] = [
  "room",
  "plate",
  "hall",
  "spring",
  "shimmer",
  "reverse",
];

/**
 * `reverb_decay`'s per-mode ceiling (§4.1's decay ceilings, and §3.12's 1.5 s
 * window ceiling standing in for one), mirroring `ReverbEngine::decayCeilingFor`.
 * All six modes are shipped now, so every entry is the mode's own ceiling — no
 * fallbacks. The knob's own range is 0.2 .. 30 s and does not change with the
 * mode; only the readout is clamped (see `displayMax`). Exported for the info
 * strip's derived "reads to N s" range chunk (reverbInfo.ts).
 */
export const REVERB_DECAY_CEILING: Record<number, number> = {
  [REVERB_ALGO.room]: 2.5,
  [REVERB_ALGO.plate]: 4.5,
  [REVERB_ALGO.hall]: 10.0,
  [REVERB_ALGO.spring]: 4.0,
  [REVERB_ALGO.shimmer]: 10.0,
  [REVERB_ALGO.reverse]: 1.5,
};

/**
 * The four primary dials. In Reverse `reverb_decay` IS the window length
 * (§3.12), so it is relabelled WINDOW rather than left saying DECAY while
 * meaning something else (§5.2: a silent semantic override is worse than a
 * relabel), and in every mode its readout stops at the mode's ceiling — over
 * most of Reverse's travel a live-but-lying number would otherwise read up to
 * 30 s against a window pinned at 1.5 s. Spring repurposes `reverb_size` as
 * the spring length, relabelled TANK (§5.2).
 *
 * SHIMMER is the fourth dial and it is ALWAYS MOUNTED, greyed outside Shimmer
 * mode (docs/REVERB.md §5.2, amended): the reserved fourth cell would
 * otherwise be an empty hole in five of six modes, and a greyed dial whose
 * hover strip says "switch the mode to bring this to life" teaches the mode
 * set. DECAY/MIX/SIZE never move by a pixel across a mode change.
 */
export function reverbPrimarySpecs(block: BlockId, algo: number): readonly KnobSpec[] {
  return [
    {
      id: forInstance(block, "reverb_decay"),
      label: algo === REVERB_ALGO.reverse ? "WINDOW" : "DECAY",
      displayMax: REVERB_DECAY_CEILING[algo] ?? REVERB_DECAY_CEILING[REVERB_ALGO.room],
    },
    { id: forInstance(block, "reverb_mix"), label: "MIX" },
    {
      id: forInstance(block, "reverb_size"),
      label: algo === REVERB_ALGO.spring ? "TANK" : "SIZE",
    },
    {
      id: forInstance(block, "reverb_shimmer"),
      label: "SHIMMER",
      disabled: algo !== REVERB_ALGO.shimmer,
    },
  ];
}

/** One row of a captioned tier-2 group. Ids are BASE ids — ReverbGroup maps
 *  them through `forInstance` at render time, exactly like the primary specs. */
export interface ReverbGroupRow {
  id: SliderParamId;
  /** Short label — buys the 44px label column that fits three groups at the
   *  1000px window floor. The full name lives in the hover strip, in
   *  ParamSlider's title tooltip, and in docs/REVERB.md. */
  label: string;
  /** §3.12: nothing to act on once the tank is replaced by the reverse window —
   *  no recirculating decay to shape (damping, bassmult) and no separate ER bus
   *  (erlevel). Asserted bit-inert by §3.12's gate 6, so greying is a statement
   *  about the DSP, not a guess. */
  inertInReverse?: boolean;
}

export interface ReverbGroup {
  caption: string;
  /** `data-rv-key` for the caption → the hover strip's group blurb. */
  infoKey: string;
  /** Right-aligned caption note, keyed by algo index. */
  note?: Readonly<Record<number, string>>;
  rows: readonly ReverbGroupRow[];
}

/**
 * The twelve tier-2 parameters as three captioned columns of four, in
 * signal-flow order: SPACE (where the sound is) · TONE (what colour it is) ·
 * TAIL (what it does over time). Per-mode deltas: Spring relabels
 * DIFF→DWELL (§5.2, applied by `reverbGroupRowLabel`); Reverse greys
 * EARLY/BASS/DAMP in place. Nothing else moves, ever.
 */
export const REVERB_GROUPS: readonly ReverbGroup[] = [
  {
    caption: "SPACE",
    infoKey: "g:space",
    note: { [REVERB_ALGO.spring]: "DWELL drives the spring" },
    rows: [
      { id: "reverb_predelay", label: "PRE" },
      { id: "reverb_diffusion", label: "DIFF" },
      { id: "reverb_erlevel", label: "EARLY", inertInReverse: true },
      { id: "reverb_width", label: "WIDTH" },
    ],
  },
  {
    caption: "TONE",
    infoKey: "g:tone",
    rows: [
      { id: "reverb_lowcut", label: "LO CUT" },
      { id: "reverb_highcut", label: "HI CUT" },
      { id: "reverb_tilt", label: "TILT" },
      { id: "reverb_color", label: "COLOR" },
    ],
  },
  {
    caption: "TAIL",
    infoKey: "g:tail",
    note: { [REVERB_ALGO.reverse]: "no recirculation here" },
    rows: [
      { id: "reverb_damping", label: "DAMP", inertInReverse: true },
      { id: "reverb_bassmult", label: "BASS", inertInReverse: true },
      { id: "reverb_mod", label: "MOD" },
      { id: "reverb_duck", label: "DUCK" },
    ],
  },
];

/** Spring repurposes `reverb_diffusion` as Dwell drive, relabelled DWELL
 *  (§5.2) — the one per-mode relabel in the groups. */
export function reverbGroupRowLabel(row: ReverbGroupRow, algo: number): string {
  if (algo === REVERB_ALGO.spring && row.id === "reverb_diffusion") return "DWELL";
  return row.label;
}
