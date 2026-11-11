/**
 * Per-type starting points (docs/REVERB-DELAY.md §2.4, §3.4). A parameter has one
 * default, but a few knobs only make sense relative to the type: picking a type in
 * its menu moves those knobs to that type's entry and leaves the rest of the panel
 * as the user set it. Host automation and preset recall never go through here.
 *
 * Mirrors the tables in src/dsp/Delay.h (`kDelayColorDefault`,
 * `kDelayHighCutDefault`, `kDelayDuckDefault`) and src/dsp/Reverb.h
 * (`kReverbDecayDefault`, `kReverbDampingDefault`), indexed by the same choice
 * order as the type menus.
 */
import { bridge, type BlockId, type SliderParamId } from "../../bridge";

/** Knob key (the id after the block prefix) -> value per type, in parameter units. */
type StartingPoints = Readonly<Record<string, readonly number[]>>;

/** Digital, Analog, Tape, Lo-Fi, Diffuse. */
export const DELAY_STARTING_POINTS: StartingPoints = {
  color: [0, 0.4, 0.35, 0.4, 0.5],
  highcut: [7000, 12000, 12000, 7000, 7000],
  duck: [0.25, 0.15, 0.15, 0.2, 0.4],
};

/** Room, Hall, Plate, Spring, Ambient. */
export const REVERB_STARTING_POINTS: StartingPoints = {
  decay: [0.7, 2.2, 1.6, 1.8, 4.5],
  damping: [0.65, 0.65, 0.5, 0.5, 0.55],
};

/** Moves each knob in `points` on `block` to its entry for type `index`, each as one
 *  host gesture. Values are in parameter units; the range and skew come from the
 *  backend, exactly as `useSliderParam.setScaled` reads them. */
export function applyStartingPoints(block: BlockId, points: StartingPoints, index: number): void {
  for (const [key, values] of Object.entries(points)) {
    const value = values[index];
    if (value === undefined) continue;

    const state = bridge.sliderState(`${block}_${key}` as SliderParamId);
    const { start, end, skew } = state.properties;
    const proportion = Math.min(1, Math.max(0, (value - start) / (end - start)));

    state.beginGesture();
    state.setNormalisedValue(Math.pow(proportion, skew));
    state.endGesture();
  }
}
