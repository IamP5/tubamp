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
 * above it (IR row / type combo / mode combo / phase toggle). The fx slots are
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
  reverb: [
    { id: "reverb_size", label: "SIZE" },
    { id: "reverb_damping", label: "DAMP" },
    { id: "reverb_mix", label: "MIX" },
    { id: "reverb_width", label: "WIDTH" },
  ],
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
