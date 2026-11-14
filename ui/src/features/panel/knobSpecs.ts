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
 * `delay`, `reverb`, `split` and `mix` keep their generic row *and* get an extra body
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
  /* gate / comp / eq / mod: every knob any type reads, most-used first; the typed
     bodies pick their subset from FX_TYPE_SPECS below (docs/FX-ENGINES.md §7.2). */
  gate: [
    { id: "gate_threshold", label: "THRESH" },
    { id: "gate_release", label: "RELEASE" },
    { id: "gate_hold", label: "HOLD" },
    { id: "gate_range", label: "RANGE" },
    { id: "gate_sc_hpf", label: "SC HPF" },
  ],
  comp: [
    { id: "comp_threshold", label: "THRESH" },
    { id: "comp_ratio", label: "RATIO" },
    { id: "comp_attack", label: "ATTACK" },
    { id: "comp_release", label: "RELEASE" },
    { id: "comp_makeup", label: "MAKEUP" },
    { id: "comp_knee", label: "KNEE" },
    { id: "comp_mix", label: "MIX" },
    { id: "comp_sc_hpf", label: "SC HPF" },
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
    { id: "cab_mix", label: "MIX" },
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
    { id: "mod_width", label: "WIDTH" },
    { id: "mod_tone", label: "TONE" },
    { id: "mod_shape", label: "SHAPE" },
    { id: "mod_wave", label: "WAVE" },
  ],
  /* ratio + width appended per docs/STEREO.md §4; ratio only bites in Dual mode
     but stays visible (and turnable) in every mode, per spec. */
  delay: [
    { id: "delay_time", label: "TIME" },
    { id: "delay_feedback", label: "FDBK" },
    { id: "delay_mix", label: "MIX" },
    { id: "delay_lowcut", label: "LO CUT" },
    { id: "delay_highcut", label: "HI CUT" },
    { id: "delay_mod", label: "MOD" },
    { id: "delay_color", label: "COLOR" },
    { id: "delay_duck", label: "DUCK" },
    { id: "delay_ratio", label: "RATIO" },
    { id: "delay_width", label: "WIDTH" },
  ],
  reverb: [
    { id: "reverb_predelay", label: "PRE-DLY" },
    { id: "reverb_decay", label: "DECAY" },
    { id: "reverb_size", label: "SIZE" },
    { id: "reverb_damping", label: "DAMP" },
    { id: "reverb_lowcut", label: "LO CUT" },
    { id: "reverb_highcut", label: "HI CUT" },
    { id: "reverb_mod", label: "MOD" },
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

/** Maps a row written for a kind's first instance onto `block`'s own ids:
 *  `comp2` + `comp_ratio` → `comp2_ratio`. Safe by construction: the pooled
 *  instance ids in bridge/types are minted from the same kind/key pairs. */
function forInstance(block: BlockId, specs: readonly KnobSpec[]): readonly KnobSpec[] {
  const kind = kindOf(block);
  if (instanceOf(block) === 0) return specs;
  return specs.map((spec) => ({
    ...spec,
    id: `${block}${spec.id.slice(kind.length)}` as SliderParamId,
  }));
}

export const KNOB_SPECS: Record<BlockId, readonly KnobSpec[]> = blockRecord((id) =>
  forInstance(id, BASE_KNOB_SPECS[kindOf(id)]),
);

/**
 * Per-type rows for the typed bodies (docs/FX-ENGINES.md §7.2), written once per
 * base like BASE_KNOB_SPECS and mapped onto an instance by `typedSpecs`. Each
 * list is exactly the knobs that type reads, in display order.
 */
const k = (id: SliderParamId, label: string): KnobSpec => ({ id, label });

const FX_TYPE_SPECS = {
  gateClassic: [k("gate_threshold", "THRESH")],
  gateFull: BASE_KNOB_SPECS.gate,
  compClassic: BASE_KNOB_SPECS.comp.slice(0, 5),
  compFull: BASE_KNOB_SPECS.comp,
  eqClassic: BASE_KNOB_SPECS.eq,
  eqStack: [...BASE_KNOB_SPECS.eq, k("eq_level", "LEVEL")],
  /* Parametric, grouped by where it acts on the signal. */
  eqParamLow: [k("eq_lowcut", "LO CUT"), k("eq_bass", "BASS")],
  eqParamMid: [k("eq_mid", "MID"), k("eq_mid_freq", "FREQ"), k("eq_mid_q", "Q")],
  eqParamHigh: [k("eq_treble", "TREBLE"), k("eq_highcut", "HI CUT")],
  eqParamOut: [k("eq_tilt", "TILT"), k("eq_level", "LEVEL")],
  eqGraphicLevel: [k("eq_level", "LEVEL")],
  modPhaser: BASE_KNOB_SPECS.mod.slice(0, 3),
  modChorus: BASE_KNOB_SPECS.mod.slice(0, 5),
  modTrem: BASE_KNOB_SPECS.mod,
} satisfies Record<string, readonly KnobSpec[]>;

export type FxTypeSpecName = keyof typeof FX_TYPE_SPECS;

/** A named per-type row, with its ids swapped to `block`'s instance. */
export function typedSpecs(block: BlockId, name: FxTypeSpecName): readonly KnobSpec[] {
  return forInstance(block, FX_TYPE_SPECS[name]);
}

/** The drive knobs are always gain / tone / level; only the labels change per
 *  type (docs/FX-ENGINES.md §4.3.2, §7.2). Index = `drive_type`. */
export const DRIVE_LABELS: readonly (readonly [string, string, string])[] = [
  ["GAIN", "TONE", "LEVEL"], // Classic
  ["DRIVE", "TONE", "LEVEL"], // Screamer
  ["DIST", "FILTER", "LEVEL"], // Distortion
  ["SUSTAIN", "TONE", "LEVEL"], // Fuzz
  ["GAIN", "HI CUT", "LEVEL"], // Boost
  ["BOOST", "FREQ", "LEVEL"], // Treble
];
