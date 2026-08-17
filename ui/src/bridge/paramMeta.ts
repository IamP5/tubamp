/**
 * Static parameter metadata — ranges, defaults, units, skew.
 *
 * The authority at runtime is the backend: `SliderProperties` arrive over the
 * relay and drive every conversion. This table exists for the two things the
 * relay does NOT carry:
 *   1. default values (double-click-to-reset on a knob), and
 *   2. the mock bridge, which has no backend to ask.
 * Values transcribed from src/Parameters.cpp (docs/research/current-ui-inventory.md
 * §Parameters). Ids are frozen — never edit one without editing the C++.
 *
 * `LAYOUT` below is ONE ordered list in APVTS layout order, and everything else
 * here is derived from it: the spec tables the mock builds its parameters from,
 * and `PARAM_INDEX`, which only means anything if the order matches the C++.
 */
import {
  DUPLICABLE_KINDS,
  EXTRA_INSTANCES,
  type ComboParamId,
  type DuplicableKind,
  type SliderParamId,
  type SliderProperties,
  type ToggleParamId,
} from "./types";

export interface SliderSpec {
  id: SliderParamId;
  name: string;
  min: number;
  max: number;
  interval: number;
  /** Default in parameter units. */
  def: number;
  /** Unit suffix appended after the numeric readout ("dB", "Hz", ":1", …). */
  label: string;
  /** `logRange()`: skew centred on the geometric mean of [min, max]. */
  log: boolean;
}

export interface ToggleSpec {
  id: ToggleParamId;
  name: string;
  def: boolean;
}

export interface ComboSpec {
  id: ComboParamId;
  name: string;
  choices: string[];
  def: number;
}

/** One APVTS parameter, tagged with the control that owns it. */
type LayoutEntry =
  | { control: "slider"; spec: SliderSpec }
  | { control: "toggle"; spec: ToggleSpec }
  | { control: "combo"; spec: ComboSpec };

const s = (
  id: SliderParamId,
  name: string,
  min: number,
  max: number,
  interval: number,
  def: number,
  label = "",
  log = false,
): LayoutEntry => ({
  control: "slider",
  spec: { id, name, min, max, interval, def, label, log },
});

const t = (id: ToggleParamId, name: string, def: boolean): LayoutEntry => ({
  control: "toggle",
  spec: { id, name, def },
});

const c = (
  id: ComboParamId,
  name: string,
  choices: string[],
  def: number,
): LayoutEntry => ({ control: "combo", spec: { id, name, choices, def } });

/* ─────────────────────────────── v1 layout ─────────────────────────────── */

/** Parameters.cpp's original layout, in order. The three fx bypasses sit at the
 *  END because that is where Parameters.cpp adds them — NOT with the other block
 *  enables, however much they read like enables. */
const V1_LAYOUT: readonly LayoutEntry[] = [
  t("gate_on", "Gate On", true),
  t("comp_on", "Comp On", true),
  /* the only block bypassed by default */
  t("drive_on", "Drive On", false),
  t("amp_on", "Amp On", true),
  t("cab_on", "Cab On", true),
  t("eq_on", "EQ On", true),
  t("mod_on", "Mod On", true),
  t("delay_on", "Delay On", true),
  t("reverb_on", "Reverb On", true),

  s("input_trim", "Input Trim", -24, 24, 0.1, 0, "dB"),
  s("output_level", "Output Level", -60, 12, 0.1, 0, "dB"),

  s("gate_threshold", "Gate Threshold", -100, 0, 0.1, -80, "dB"),

  s("comp_threshold", "Comp Threshold", -60, 0, 0.1, -20, "dB"),
  s("comp_ratio", "Comp Ratio", 1, 20, 0.01, 4, ":1", true),
  s("comp_attack", "Comp Attack", 0.1, 100, 0.01, 5, "ms", true),
  s("comp_release", "Comp Release", 10, 1000, 0.1, 120, "ms", true),
  s("comp_makeup", "Comp Makeup", 0, 24, 0.1, 0, "dB"),

  s("drive_gain", "Drive Gain", 0, 36, 0.1, 12, "dB"),
  s("drive_tone", "Drive Tone", 500, 12000, 1, 4000, "Hz", true),
  s("drive_level", "Drive Level", -24, 12, 0.1, 0, "dB"),

  s("amp_input", "Amp Input", -20, 20, 0.1, 0, "dB"),
  s("amp_output", "Amp Output", -40, 40, 0.1, 0, "dB"),
  c("amp_out_mode", "Output Mode", ["Raw", "Normalized", "Calibrated"], 1),
  t("amp_cal_input", "Calibrate Input", false),
  s("amp_cal_level", "Input Calibration Level", -60, 60, 0.1, 12, "dBu"),
  s("amp_slim", "Slim", 0, 1, 0.01, 0),

  s("cab_lowcut", "Cab Low Cut", 20, 500, 1, 80, "Hz", true),
  s("cab_highcut", "Cab High Cut", 2000, 20000, 1, 8000, "Hz", true),

  s("eq_bass", "Bass", 0, 10, 0.1, 5),
  s("eq_mid", "Middle", 0, 10, 0.1, 5),
  s("eq_treble", "Treble", 0, 10, 0.1, 5),

  c("mod_type", "Mod Type", ["Chorus", "Phaser", "Tremolo"], 0),
  s("mod_rate", "Mod Rate", 0.05, 10, 0.001, 1, "Hz", true),
  s("mod_depth", "Mod Depth", 0, 1, 0.001, 0.4),
  s("mod_mix", "Mod Mix", 0, 1, 0.001, 0.35),

  s("delay_time", "Delay Time", 20, 2000, 0.1, 420, "ms", true),
  s("delay_feedback", "Delay Feedback", 0, 0.95, 0.001, 0.35),
  s("delay_mix", "Delay Mix", 0, 1, 0.001, 0.25),

  s("reverb_size", "Reverb Size", 0, 1, 0.001, 0.5),
  s("reverb_damping", "Reverb Damping", 0, 1, 0.001, 0.5),
  s("reverb_mix", "Reverb Mix", 0, 1, 0.001, 0.25),

  /* External AU slots ship enabled: an empty slot is a pass-through anyway, so
     "on" only starts costing anything once a plugin is actually loaded. */
  t("fx1_on", "FX 1 On", true),
  t("fx2_on", "FX 2 On", true),
  t("fx3_on", "FX 3 On", true),
];

/* ──────────────────────── v2 instances + amp tone stack ────────────────── */

/** How a kind names itself in a host parameter list. Not derivable from the v1
 *  names: eq's own parameters carry no prefix at all ("Bass", "Middle"). */
const KIND_PARAM_LABEL: Record<DuplicableKind, string> = {
  comp: "Comp",
  drive: "Drive",
  eq: "EQ",
  mod: "Mod",
  delay: "Delay",
  reverb: "Reverb",
};

/** "Comp Threshold" + 2 → "Comp 2 Threshold"; "Bass" + 2 → "EQ 2 Bass". Logic
 *  lists parameters by name, so three identical "Comp Threshold" rows would be
 *  unusable. */
function instanceName(name: string, kind: DuplicableKind, instance: number): string {
  const label = KIND_PARAM_LABEL[kind];
  const tail = name.startsWith(`${label} `) ? name.slice(label.length + 1) : name;
  return `${label} ${instance} ${tail}`;
}

/**
 * Instance 2 and 3 clone instance 1's entry: same range, default, step and unit,
 * a `<kind><n>_` id and an instance-numbered host name — which is exactly how
 * Parameters.cpp mints them. The id casts are safe by construction: the ids come
 * from the same kind/key pairs the frozen unions in ./types are built from.
 */
function instanceEntry(
  base: LayoutEntry,
  kind: DuplicableKind,
  instance: number,
): LayoutEntry {
  const id = `${kind}${instance}${base.spec.id.slice(kind.length)}`;
  const name = instanceName(base.spec.name, kind, instance);
  switch (base.control) {
    case "slider":
      return { control: "slider", spec: { ...base.spec, id: id as SliderParamId, name } };
    case "toggle":
      // Every instance enable ships ON, including drive's (whose instance 1 is
      // the one block bypassed by default) — a block is not in the chain until
      // it is added, so adding one and hearing nothing is the wrong answer.
      return {
        control: "toggle",
        spec: { ...base.spec, id: id as ToggleParamId, name, def: true },
      };
    case "combo":
      return { control: "combo", spec: { ...base.spec, id: id as ComboParamId, name } };
  }
}

/** A kind's own parameters in layout order — its enable first, because the v1
 *  enables come before everything else. */
function entriesOfKind(kind: DuplicableKind): LayoutEntry[] {
  return V1_LAYOUT.filter((entry) => entry.spec.id.startsWith(`${kind}_`));
}

const INSTANCE_LAYOUT: readonly LayoutEntry[] = DUPLICABLE_KINDS.flatMap((kind) =>
  EXTRA_INSTANCES.flatMap((instance) =>
    entriesOfKind(kind).map((base) => instanceEntry(base, kind, instance)),
  ),
);

/** The amp's built-in tone stack (R2). Shaped like an eq but owned by the amp
 *  block, so the instance rule above cannot mint it — hand-written on purpose.
 *  Defaults: on, and 5.0 = flat. */
const AMP_EQ_LAYOUT: readonly LayoutEntry[] = [
  t("amp_eq_on", "Amp EQ On", true),
  s("amp_eq_bass", "Amp EQ Bass", 0, 10, 0.1, 5),
  s("amp_eq_mid", "Amp EQ Middle", 0, 10, 0.1, 5),
  s("amp_eq_treble", "Amp EQ Treble", 0, 10, 0.1, 5),
];

/* ─────────────────────── v2 stereo/dual-mode append ─────────────────────── */

/**
 * docs/STEREO.md §4 append: instance 1's own entries for three params that had
 * no v1 counterpart, so `entriesOfKind()` above cannot find them in V1_LAYOUT —
 * declared by hand instead, then cloned to instances 2/3 the same way. (The
 * dual-NAM stereo amp part of that spec is superseded by `amp2` as a chain
 * block — docs/SPLIT.md — so there is no `amp_stereo` entry here.)
 */
const DELAY_MODE_1 = c(
  "delay_mode",
  "Delay Mode",
  ["Stereo", "Ping-Pong", "Dual"],
  0,
);
const DELAY_RATIO_1 = s("delay_ratio", "Delay Ratio", 25, 200, 0.1, 100, "%");
const DELAY_WIDTH_1 = s("delay_width", "Delay Width", 0, 1, 0.001, 1);
const REVERB_WIDTH_1 = s("reverb_width", "Reverb Width", 0, 1, 0.001, 1);

/** `instanceEntry()` for both extra instances at once. */
function stereoInstances(base: LayoutEntry, kind: DuplicableKind): LayoutEntry[] {
  return EXTRA_INSTANCES.map((instance) => instanceEntry(base, kind, instance));
}

/** Appended, mirroring createParameterLayout()'s append order exactly — this is
 *  APVTS index order, not the order §4 happens to list the controls in: mode/
 *  ratio/width per delay instance, then the three reverb widths (no
 *  `amp_stereo` — superseded, see above). The derived tables below are keyed
 *  by id, so only PARAM_INDEX depends on this. */
const [DELAY_MODE_2, DELAY_MODE_3] = stereoInstances(DELAY_MODE_1, "delay");
const [DELAY_RATIO_2, DELAY_RATIO_3] = stereoInstances(DELAY_RATIO_1, "delay");
const [DELAY_WIDTH_2, DELAY_WIDTH_3] = stereoInstances(DELAY_WIDTH_1, "delay");

const STEREO_LAYOUT: readonly LayoutEntry[] = [
  DELAY_MODE_1,
  DELAY_RATIO_1,
  DELAY_WIDTH_1,
  DELAY_MODE_2,
  DELAY_RATIO_2,
  DELAY_WIDTH_2,
  DELAY_MODE_3,
  DELAY_RATIO_3,
  DELAY_WIDTH_3,
  REVERB_WIDTH_1,
  ...stereoInstances(REVERB_WIDTH_1, "reverb"),
];

/* ──────────────────────── v2 split-path append (docs/SPLIT.md) ──────────── */

/**
 * Appended after the stereo-chain block, in `createParameterLayout()`'s exact
 * order: amp2's own input/output (range + default copied verbatim from
 * `amp_input`/`amp_output`), then split's enable/mode/crossover, then mix's
 * enable, five level/pan controls and master level.
 */
const SPLIT_LAYOUT: readonly LayoutEntry[] = [
  t("amp2_on", "Amp 2 On", true),
  s("amp2_input", "Amp 2 Input", -20, 20, 0.1, 0, "dB"),
  s("amp2_output", "Amp 2 Output", -40, 40, 0.1, 0, "dB"),

  t("split_on", "Split On", true),
  c("split_mode", "Split Mode", ["Copy", "L/R", "X-Over"], 0),
  s("split_xover", "Split X-Over", 100, 4000, 1, 800, "Hz", true),

  t("mix_on", "Mix On", true),
  s("mix_alevel", "Mix A Level", -60, 12, 0.1, 0, "dB"),
  s("mix_blevel", "Mix B Level", -60, 12, 0.1, 0, "dB"),
  s("mix_apan", "Mix A Pan", -1, 1, 0.001, 0),
  s("mix_bpan", "Mix B Pan", -1, 1, 0.001, 0),
  t("mix_bphase", "Mix B Phase", false),
  s("mix_level", "Mix Level", -60, 12, 0.1, 0, "dB"),
];

/* ─────────────────── reverb engine append (docs/REVERB.md §5) ───────────── */

/**
 * `reverb_algo`'s choice list — FROZEN at six entries and declared in full from
 * day one (docs/REVERB.md §4), so `AudioParameterChoice`'s normalisation
 * divisor `index / (numChoices − 1)` never changes and no recorded automation
 * lane is ever repointed. All six are live in the combo (ReverbBody) now that
 * Spring (Stage 2) and Shimmer (Stage 3) have shipped.
 */
export const REVERB_ALGO_CHOICES = [
  "Room",
  "Plate",
  "Hall",
  "Spring",
  "Shimmer",
  "Reverse",
] as const;

/**
 * `reverb_shimmer_interval`'s choice list — FROZEN at four entries (§3.11),
 * same normalisation argument as `REVERB_ALGO_CHOICES` above. Default index 2
 * = "+1 Oct". Inert outside Shimmer (§5.2).
 */
export const REVERB_SHIMMER_INTERVAL_CHOICES = [
  "-1 Oct",
  "+5th",
  "+1 Oct",
  "+1 Oct & +5th",
] as const;

/**
 * The twelve-parameter reverb batch (`kVersionHint3`) for instance 1, in the
 * spec's order. Like the stereo block above, none of these have a v1
 * counterpart, so `entriesOfKind()` cannot find them — hand-declared, then
 * cloned to instances 2/3 by the same `instanceEntry()` rule.
 *
 * `reverb_size`/`_damping`/`_mix`/`_width` are NOT here: their ids, ranges and
 * defaults are unchanged (§5) and they now drive the new engine.
 *
 * Defaults are the authority this table exists for (double-click-to-reset), and
 * they are transcribed from §5's table verbatim: absent `reverb_algo` resolving
 * to 0 = Room is the whole backward-compatibility keystone (§5.3).
 */
const REVERB_BATCH_1: readonly LayoutEntry[] = [
  c("reverb_algo", "Reverb Algorithm", [...REVERB_ALGO_CHOICES], 0),
  s("reverb_decay", "Reverb Decay", 0.2, 30, 0.01, 2, "s", true),
  s("reverb_predelay", "Reverb Pre-Delay", 0, 250, 0.1, 0, "ms"),
  s("reverb_diffusion", "Reverb Diffusion", 0, 1, 0.001, 0.7),
  s("reverb_lowcut", "Reverb Low Cut", 20, 800, 1, 20, "Hz", true),
  s("reverb_highcut", "Reverb High Cut", 1200, 20000, 1, 20000, "Hz", true),
  s("reverb_mod", "Reverb Mod", 0, 1, 0.001, 0.35),
  /* A decay MULTIPLIER, not an EQ, and logRange() in Parameters.cpp: the skew is
     centred on the geometric mean of 0.25 and 4.0, which is exactly the x1.0
     default — so the mock has to declare it too or the default sits at 20 % of
     travel here and at 50 % in the plugin. */
  s("reverb_bassmult", "Reverb Bass Mult", 0.25, 4, 0.01, 1, "x", true),
  s("reverb_erlevel", "Reverb ER Level", 0, 1, 0.001, 0.5),
  s("reverb_color", "Reverb Color", 0, 1, 0.001, 0),
  s("reverb_tilt", "Reverb Tilt", -1, 1, 0.001, 0),
  s("reverb_duck", "Reverb Duck", 0, 1, 0.001, 0),
];

/** Expanded exactly the way `createParameterLayout()` appends the batch
 *  (src/Parameters.cpp): instance 1's combo and its eleven sliders, then the
 *  same twelve for instance 2, then for instance 3 — instance-major, not
 *  id-major. Only PARAM_INDEX depends on this order — the derived tables below
 *  are keyed by id — and PARAM_INDEX is what the mock reports as
 *  `parameterIndex`, so getting it wrong makes Logic's touch-to-select behave
 *  differently in dev than in the plugin. */
const REVERB_BATCH_1_LAYOUT: readonly LayoutEntry[] = [
  ...REVERB_BATCH_1,
  ...EXTRA_INSTANCES.flatMap((instance) =>
    REVERB_BATCH_1.map((base) => instanceEntry(base, "reverb", instance)),
  ),
];

/**
 * The Shimmer batch (`kVersionHint4`, Stage 3, §5): level then interval, for
 * instance 1. Appended as its own block — same shape as REVERB_BATCH_1, one
 * kVersionHint per shipped batch, never interleaved into an earlier one
 * (§5.3).
 */
const REVERB_BATCH_2: readonly LayoutEntry[] = [
  s("reverb_shimmer", "Reverb Shimmer", 0, 1, 0.001, 0),
  c("reverb_shimmer_interval", "Reverb Shimmer Interval", [...REVERB_SHIMMER_INTERVAL_CHOICES], 2),
];

/** Expanded instance-major like REVERB_BATCH_1_LAYOUT above, but appended
 *  AFTER all three instances of batch 1 (`createParameterLayout()`'s batch-2
 *  block runs after the whole batch-1 loop, not interleaved per instance). */
const REVERB_BATCH_2_LAYOUT: readonly LayoutEntry[] = [
  ...REVERB_BATCH_2,
  ...EXTRA_INSTANCES.flatMap((instance) =>
    REVERB_BATCH_2.map((base) => instanceEntry(base, "reverb", instance)),
  ),
];

const REVERB_LAYOUT: readonly LayoutEntry[] = [
  ...REVERB_BATCH_1_LAYOUT,
  ...REVERB_BATCH_2_LAYOUT,
];

/** The whole APVTS layout, in order: 44 v1 parameters, 54 instance parameters,
 *  4 for the amp tone stack, 12 for the stereo-chain append, 13 for the
 *  split-path append, 36 for the reverb-engine batch-1 append, 6 for the
 *  Shimmer batch-2 append — 125 sliders, 30 toggles, 14 combos. */
const LAYOUT: readonly LayoutEntry[] = [
  ...V1_LAYOUT,
  ...INSTANCE_LAYOUT,
  ...AMP_EQ_LAYOUT,
  ...STEREO_LAYOUT,
  ...SPLIT_LAYOUT,
  ...REVERB_LAYOUT,
];

/* ────────────────────────────── derived tables ─────────────────────────── */

export const SLIDER_SPECS: readonly SliderSpec[] = LAYOUT.flatMap((entry) =>
  entry.control === "slider" ? [entry.spec] : [],
);

export const TOGGLE_SPECS: readonly ToggleSpec[] = LAYOUT.flatMap((entry) =>
  entry.control === "toggle" ? [entry.spec] : [],
);

export const COMBO_SPECS: readonly ComboSpec[] = LAYOUT.flatMap((entry) =>
  entry.control === "combo" ? [entry.spec] : [],
);

/**
 * APVTS layout order. Used for `parameterIndex` in the mock, so Logic's
 * touch-to-select behaves like the real plugin when developing against it.
 */
export const PARAM_INDEX: Readonly<Record<string, number>> = Object.fromEntries(
  LAYOUT.map((entry, index) => [entry.spec.id, index]),
);

export const SLIDER_SPEC_BY_ID: Readonly<Record<SliderParamId, SliderSpec>> =
  Object.fromEntries(SLIDER_SPECS.map((spec) => [spec.id, spec])) as Record<
    SliderParamId,
    SliderSpec
  >;

/** Default in parameter units (what double-click on a knob resets to). */
export function defaultScaledValue(id: SliderParamId): number {
  return SLIDER_SPEC_BY_ID[id].def;
}

/** JUCE's `NormalisableRange::setSkewForCentre` — puts `centre` at 0.5. */
export function skewForCentre(
  min: number,
  max: number,
  centre: number,
): number {
  return Math.log(0.5) / Math.log((centre - min) / (max - min));
}

export function skewFor(spec: SliderSpec): number {
  if (!spec.log) return 1;
  const centre = Math.sqrt(Math.max(spec.min, 1e-6) * spec.max);
  return skewForCentre(spec.min, spec.max, centre);
}

/** Decimal places implied by a step interval, matching JUCE's default text. */
export function decimalsForInterval(interval: number): number {
  if (interval <= 0) return 2;
  if (interval >= 1) return 0;
  return Math.min(3, Math.ceil(-Math.log10(interval)));
}

/** "12.0 dB", "4.00 :1", "8000 Hz" — JUCE's default slider text, reproduced. */
export function formatParamValue(
  scaled: number,
  properties: Pick<SliderProperties, "interval" | "label">,
): string {
  const text = scaled.toFixed(decimalsForInterval(properties.interval));
  return properties.label ? `${text} ${properties.label}` : text;
}
