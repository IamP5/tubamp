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

/** The whole APVTS layout, in order: 44 v1 parameters, 54 instance parameters,
 *  4 for the amp tone stack — 72 sliders, 26 toggles, 4 combos. */
const LAYOUT: readonly LayoutEntry[] = [
  ...V1_LAYOUT,
  ...INSTANCE_LAYOUT,
  ...AMP_EQ_LAYOUT,
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
