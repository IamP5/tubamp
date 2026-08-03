/**
 * Generic knob rows per block — the React port of `knobSpecsFor()`
 * (docs/research/current-ui-inventory.md §BlockParamPanel, "Knob specs per
 * block"). Ids and labels are verbatim, and so is the display order.
 *
 * `amp` is deliberately empty: every amp control lives in AmpBody, exactly like
 * the native panel. `cab` and `mod` keep their generic row *and* get an extra
 * body above it (IR row / type combo).
 */
import type { BlockId, SliderParamId } from "../../bridge";

export interface KnobSpec {
  id: SliderParamId;
  /** Micro-label under the dial (rendered uppercase). */
  label: string;
}

export const KNOB_SPECS: Record<BlockId, readonly KnobSpec[]> = {
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
  delay: [
    { id: "delay_time", label: "TIME" },
    { id: "delay_feedback", label: "FDBK" },
    { id: "delay_mix", label: "MIX" },
  ],
  reverb: [
    { id: "reverb_size", label: "SIZE" },
    { id: "reverb_damping", label: "DAMP" },
    { id: "reverb_mix", label: "MIX" },
  ],
};
