/**
 * Generic knob rows per block — the React port of `knobSpecsFor()`
 * (docs/research/current-ui-inventory.md §BlockParamPanel, "Knob specs per
 * block"). Ids and labels are verbatim, and so is the display order.
 *
 * Written once per BASE and mapped onto all 24 tokens: an instance's row is its
 * kind's row with the id prefix swapped (`comp_ratio` → `comp2_ratio`), which is
 * exactly how Parameters.cpp mints the instance parameters.
 *
 * `amp` is deliberately empty: every amp control lives in AmpBody, exactly like
 * the native panel. `cab` and `mod` keep their generic row *and* get an extra
 * body above it (IR row / type combo). The fx slots are empty too — a hosted
 * plugin's parameters are discovered at load time, so they can never appear in a
 * static table of tubamp's own slider ids (FxSlotBody renders them instead).
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
  /* Hosted plugins bring their own parameters — see FxSlotBody. */
  fx1: [],
  fx2: [],
  fx3: [],
};

export const KNOB_SPECS: Record<BlockId, readonly KnobSpec[]> = blockRecord((id) => {
  const kind = kindOf(id);
  const base = BASE_KNOB_SPECS[kind];
  if (instanceOf(id) === 0) return base;
  // `comp2` + `comp_ratio` → `comp2_ratio`. Safe by construction: the pooled
  // instance ids in bridge/types are minted from the same kind/key pairs.
  return base.map((spec) => ({
    ...spec,
    id: `${id}${spec.id.slice(kind.length)}` as SliderParamId,
  }));
});
