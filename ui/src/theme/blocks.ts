/**
 * Per-block identity: display strings, the enable-param id and the family the
 * block belongs to. A block has no colour of its own (ui/DESIGN.md); its family
 * has one, shared by every block that does the same kind of job.
 *
 * Mirrors `chain::BlockInfo` (src/dsp/ChainOrder.h). Both tables are written
 * once per BASE (the ten kinds, the three fx slots and the split-path's own
 * split/lane2/mix furniture — docs/SPLIT.md §1 — in frozen enum order) and
 * mapped onto all 28 tokens: an instance reads "Compressor 2" / "Comp 2" off
 * its kind's entry, so the two can never drift apart.
 */
import {
  blockRecord,
  instanceOf,
  kindOf,
  type BaseBlockId,
  type BlockId,
  type ToggleParamId,
} from "../bridge/types";

/**
 * What a block does. The accent takes the family's hue inside anything stamped
 * `data-family` (theme/tokens.css): a tile, the editor band. Plugins and the
 * split furniture have none and stay neutral.
 */
export type BlockFamily = "dynamics" | "gain" | "tone" | "mod" | "time";

const BASE_FAMILY: Partial<Record<BaseBlockId, BlockFamily>> = {
  gate: "dynamics",
  comp: "dynamics",
  drive: "gain",
  amp: "gain",
  amp2: "gain",
  cab: "tone",
  eq: "tone",
  mod: "mod",
  delay: "time",
  reverb: "time",
};

export interface BlockInfo {
  id: BlockId;
  /** Undefined for plugins and routing: no hue of their own. */
  family: BlockFamily | undefined;
  /** Full name — panel header, tooltips, menu section headers. */
  displayName: string;
  /** Tile caption — the same name where space is tight ("Comp", "Mod"). */
  shortName: string;
  enableParamId: ToggleParamId;
}

const BASE_INFO: Record<BaseBlockId, { displayName: string; shortName: string }> = {
  gate: { displayName: "Gate", shortName: "Gate" },
  comp: { displayName: "Compressor", shortName: "Comp" },
  drive: { displayName: "Drive", shortName: "Drive" },
  amp: { displayName: "Amp", shortName: "Amp" },
  cab: { displayName: "Cab", shortName: "Cab" },
  eq: { displayName: "EQ", shortName: "EQ" },
  mod: { displayName: "Modulation", shortName: "Mod" },
  delay: { displayName: "Delay", shortName: "Delay" },
  reverb: { displayName: "Reverb", shortName: "Reverb" },
  /* The slot's own identity. Once a plugin is loaded the panel header shows the
     plugin's name instead, but the card caption stays "FX 1" — the slot is the
     thing that lives in the chain, the plugin is what is in it. */
  fx1: { displayName: "Plugin 1", shortName: "Plugin 1" },
  fx2: { displayName: "Plugin 2", shortName: "Plugin 2" },
  fx3: { displayName: "Plugin 3", shortName: "Plugin 3" },
  /* Verbatim from ChainOrder.h's blockInfos (docs/SPLIT.md §1). */
  amp2: { displayName: "Amp 2", shortName: "Amp 2" },
  split: { displayName: "Split", shortName: "Split" },
  lane2: { displayName: "Lane B", shortName: "Lane B" },
  mix: { displayName: "Mixer", shortName: "Mixer" },
};

export const BLOCK_INFO: Record<BlockId, BlockInfo> = blockRecord((id) => {
  const base = BASE_INFO[kindOf(id)];
  const instance = instanceOf(id);
  return {
    id,
    family: BASE_FAMILY[kindOf(id)],
    // Instance 1 keeps the unnumbered name: it is the only one there is until a
    // second is added, and renaming it to "Compressor 1" would churn presets'
    // vocabulary for nothing.
    displayName: instance === 0 ? base.displayName : `${base.displayName} ${instance + 1}`,
    shortName: instance === 0 ? base.shortName : `${base.shortName} ${instance + 1}`,
    // Every block's bypass is `<token>_on` — the rule Parameters.cpp mints them
    // with, for every token except lane2, which has none of its own: ChainOrder.h's
    // blockInfos gives it split's ("split_on") since it is split's own boundary,
    // not that it is ever read — lane2 never reaches a panel.
    enableParamId: (id === "lane2" ? "split_on" : `${id}_on`) as ToggleParamId,
  };
});
