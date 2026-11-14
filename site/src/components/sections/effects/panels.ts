import { CHAIN, type Block } from "@/data/site";
import { CLIPS, type Clip } from "@/data/media";

/** The blocks that have an editor strip, in chain order. The amp has its own chapter. */
const EDITOR_IDS = ["gate", "comp", "drive", "cab", "eq", "mod", "delay", "reverb"] as const satisfies readonly (keyof typeof CLIPS)[];

export type EffectPanel = { block: Block; clip: Clip };

const blocks = new Map(CHAIN.map((block) => [block.id, block]));

export const EFFECT_PANELS: readonly EffectPanel[] = EDITOR_IDS.map((id) => {
  const block = blocks.get(id);
  if (!block) throw new Error(`No block "${id}" in CHAIN`);
  return { block, clip: CLIPS[id] };
});
