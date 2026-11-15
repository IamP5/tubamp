/**
 * The amp kind's two instances, `amp` and `amp2`: one editor, one parameter
 * set each (`${amp}_input`, `${amp}_eq_bass`, …). The only thing that differs
 * between them is which engine slot of the bridge holds the capture, and that
 * lives here and nowhere else. A TONE3000 target for an amp is its token.
 */
import {
  KIND_INSTANCES,
  bridge,
  type AmpToken,
  type BlockId,
  type ComboParamId,
  type ErrorResult,
  type MissingMedia,
  type ModelInfo,
  type SliderParamId,
  type ToggleParamId,
  type UiState,
} from "../bridge";

export interface AmpSlot {
  model(s: Pick<UiState, "model" | "modelB">): ModelInfo | null;
  missing: keyof MissingMedia;
  load(path: string): Promise<ErrorResult>;
  clear(): Promise<void>;
}

export const AMP_SLOTS: Record<AmpToken, AmpSlot> = {
  amp: {
    model: (s) => s.model,
    missing: "model",
    load: (path) => bridge.loadModel(path),
    clear: () => bridge.clearModel(),
  },
  amp2: {
    model: (s) => s.modelB,
    missing: "modelB",
    load: (path) => bridge.loadModelB(path),
    clear: () => bridge.clearModelB(),
  },
};

export const AMP_TOKENS: readonly AmpToken[] = KIND_INSTANCES.amp;

export function isAmpToken(id: string | null | undefined): id is AmpToken {
  return (AMP_TOKENS as readonly (string | null | undefined)[]).includes(id);
}

/** "Amp" while one amp is placed; "Amp 1" / "Amp 2" only when both are and a
 *  sentence has to say which. */
export function ampLabel(amp: AmpToken, order: readonly BlockId[]): string {
  const both = AMP_TOKENS.every((token) => order.includes(token));
  return both ? `Amp ${AMP_TOKENS.indexOf(amp) + 1}` : "Amp";
}

/** `want` when it is in the chain, else whichever amp is, else `amp`: a load
 *  never lands in an engine the chain does not run. */
export function placedAmp(want: AmpToken, order: readonly BlockId[]): AmpToken {
  if (order.includes(want)) return want;
  return AMP_TOKENS.find((token) => order.includes(token)) ?? "amp";
}

/** Every amp control besides the capture, by key. */
const AMP_SLIDERS = ["input", "output", "cal_level", "slim", "eq_bass", "eq_mid", "eq_treble"] as const;
const AMP_TOGGLES = ["cal_input", "eq_on"] as const;
const AMP_COMBOS = ["out_mode"] as const;

export const ampSlider = (amp: AmpToken, key: (typeof AMP_SLIDERS)[number]) =>
  `${amp}_${key}` as SliderParamId;
export const ampToggle = (amp: AmpToken, key: (typeof AMP_TOGGLES)[number]) =>
  `${amp}_${key}` as ToggleParamId;
export const ampCombo = (amp: AmpToken, key: (typeof AMP_COMBOS)[number]) =>
  `${amp}_${key}` as ComboParamId;

/** Writes every control of `from` onto `to`. Both amps share ranges, so the
 *  normalised value carries over as is. */
export function copyAmpSettings(from: AmpToken, to: AmpToken): void {
  for (const key of AMP_SLIDERS) {
    const target = bridge.sliderState(ampSlider(to, key));
    target.beginGesture();
    target.setNormalisedValue(bridge.sliderState(ampSlider(from, key)).getNormalisedValue());
    target.endGesture();
  }
  for (const key of AMP_TOGGLES)
    bridge.toggleState(ampToggle(to, key)).setValue(bridge.toggleState(ampToggle(from, key)).getValue());
  for (const key of AMP_COMBOS)
    bridge.comboState(ampCombo(to, key)).setChoiceIndex(bridge.comboState(ampCombo(from, key)).getChoiceIndex());
}
