/**
 * Per-type starting points (docs/REVERB-DELAY.md §2.4, §3.4). A parameter has one
 * default, but a few knobs only make sense relative to the type: picking a type in
 * its menu moves those knobs to that type's entry and leaves the rest of the panel
 * as the user set it. Host automation and preset recall never go through here.
 *
 * Mirrors the tables in src/dsp/Delay.h (`kDelayColorDefault`,
 * `kDelayHighCutDefault`, `kDelayDuckDefault`) and src/dsp/Reverb.h
 * (`kReverbDecayDefault`, `kReverbDampingDefault`), and — docs/FX-ENGINES.md
 * §2.7.4, §7.3 — Gate.h, Compressor.h, Drive.h, Chorus.h, Phaser.h and
 * Tremolo.h, indexed by the same choice order as the type menus.
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

/** Classic, Tight, Smooth (Gate.h `kGateReleaseDefault`, `kGateHoldDefault`). */
export const GATE_STARTING_POINTS: StartingPoints = {
  release: [30, 30, 80],
  hold: [6, 6, 10],
};

/** Classic, Clean, Squash, FET, Opto (Compressor.h `kCompKneeDefault`,
 *  `kCompAttackDefault`, `kCompReleaseDefault`). */
export const COMP_STARTING_POINTS: StartingPoints = {
  knee: [6, 6, 6, 4, 12],
  attack: [5, 5, 2.5, 0.8, 10],
  release: [120, 120, 150, 55, 60],
};

/** Classic, Screamer, Distortion, Fuzz, Boost, Treble (Drive.h `kDriveGainDefault`,
 *  `kDriveToneDefault`, `kDriveLevelDefault`; Distortion and Fuzz levels are the
 *  measured L2 = -8.2 and L3 = 2.2). */
export const DRIVE_STARTING_POINTS: StartingPoints = {
  gain: [12, 12, 18, 18, 12, 12],
  tone: [4000, 4000, 2449, 2449, 12000, 3000],
  level: [0, 0, -8.2, 2.2, 0, 0],
};

/** Classic, Wide, CE, Dimension, Tri (Chorus.h). */
export const CHORUS_STARTING_POINTS: StartingPoints = {
  rate: [1, 1, 0.8, 0.25, 0.6],
  depth: [0.4, 0.4, 0.7, 0.8, 0.5],
  mix: [0.35, 0.35, 0.5, 0.5, 0.5],
};

/** Classic, Phase 90, Small Stone (Phaser.h `kPhaserRateDefault`). */
export const PHASER_STARTING_POINTS: StartingPoints = {
  rate: [1, 1.07, 0.32],
};

/** Classic, Smooth, Optical, Bias, Harmonic (Tremolo.h). */
export const TREM_STARTING_POINTS: StartingPoints = {
  rate: [1, 4, 5, 5, 4],
  depth: [0.4, 0.6, 0.7, 0.75, 0.8],
  width: [1, 0, 0, 0, 0],
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
