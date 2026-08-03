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
 */
import type {
  ComboParamId,
  SliderParamId,
  SliderProperties,
  ToggleParamId,
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

const s = (
  id: SliderParamId,
  name: string,
  min: number,
  max: number,
  interval: number,
  def: number,
  label = "",
  log = false,
): SliderSpec => ({ id, name, min, max, interval, def, label, log });

export const SLIDER_SPECS: readonly SliderSpec[] = [
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
  s("amp_cal_level", "Input Calibration Level", -60, 60, 0.1, 12, "dBu"),
  s("amp_slim", "Slim", 0, 1, 0.01, 0),
  s("cab_lowcut", "Cab Low Cut", 20, 500, 1, 80, "Hz", true),
  s("cab_highcut", "Cab High Cut", 2000, 20000, 1, 8000, "Hz", true),
  s("eq_bass", "Bass", 0, 10, 0.1, 5),
  s("eq_mid", "Middle", 0, 10, 0.1, 5),
  s("eq_treble", "Treble", 0, 10, 0.1, 5),
  s("mod_rate", "Mod Rate", 0.05, 10, 0.001, 1, "Hz", true),
  s("mod_depth", "Mod Depth", 0, 1, 0.001, 0.4),
  s("mod_mix", "Mod Mix", 0, 1, 0.001, 0.35),
  s("delay_time", "Delay Time", 20, 2000, 0.1, 420, "ms", true),
  s("delay_feedback", "Delay Feedback", 0, 0.95, 0.001, 0.35),
  s("delay_mix", "Delay Mix", 0, 1, 0.001, 0.25),
  s("reverb_size", "Reverb Size", 0, 1, 0.001, 0.5),
  s("reverb_damping", "Reverb Damping", 0, 1, 0.001, 0.5),
  s("reverb_mix", "Reverb Mix", 0, 1, 0.001, 0.25),
];

export const TOGGLE_SPECS: readonly {
  id: ToggleParamId;
  name: string;
  def: boolean;
}[] = [
  { id: "gate_on", name: "Gate On", def: true },
  { id: "comp_on", name: "Comp On", def: true },
  /* the only block bypassed by default */
  { id: "drive_on", name: "Drive On", def: false },
  { id: "amp_on", name: "Amp On", def: true },
  { id: "cab_on", name: "Cab On", def: true },
  { id: "eq_on", name: "EQ On", def: true },
  { id: "mod_on", name: "Mod On", def: true },
  { id: "delay_on", name: "Delay On", def: true },
  { id: "reverb_on", name: "Reverb On", def: true },
  { id: "amp_cal_input", name: "Calibrate Input", def: false },
];

export const COMBO_SPECS: readonly {
  id: ComboParamId;
  name: string;
  choices: string[];
  def: number;
}[] = [
  {
    id: "amp_out_mode",
    name: "Output Mode",
    choices: ["Raw", "Normalized", "Calibrated"],
    def: 1,
  },
  { id: "mod_type", name: "Mod Type", choices: ["Chorus", "Phaser", "Tremolo"], def: 0 },
];

/**
 * APVTS layout order (the 9 enables first, then Parameters.cpp order). Used for
 * `parameterIndex` in the mock, so Logic's touch-to-select behaves like the real
 * plugin when developing against the mock.
 */
export const PARAM_INDEX: Readonly<Record<string, number>> = Object.fromEntries(
  [
    "gate_on", "comp_on", "drive_on", "amp_on", "cab_on", "eq_on", "mod_on",
    "delay_on", "reverb_on",
    "input_trim", "output_level", "gate_threshold",
    "comp_threshold", "comp_ratio", "comp_attack", "comp_release", "comp_makeup",
    "drive_gain", "drive_tone", "drive_level",
    "amp_input", "amp_output", "amp_out_mode", "amp_cal_input", "amp_cal_level",
    "amp_slim",
    "cab_lowcut", "cab_highcut",
    "eq_bass", "eq_mid", "eq_treble",
    "mod_type", "mod_rate", "mod_depth", "mod_mix",
    "delay_time", "delay_feedback", "delay_mix",
    "reverb_size", "reverb_damping", "reverb_mix",
  ].map((id, i) => [id, i]),
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
