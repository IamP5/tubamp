/**
 * Every block's `*_on` parameter, bound once at board level.
 *
 * Nine fixed `useToggleParam` calls (BLOCK_IDS is frozen, so the hook count is
 * constant) — the card LEDs, the kebab menu labels and the connector dimming all
 * read from the same snapshot, and none of them has to subscribe individually.
 * Replaces the native view's 100ms `refreshFromParams` poll with a real
 * subscription, so host automation / A-B recall shows up immediately.
 */
import { BLOCK_IDS, type BlockId } from "../../bridge/types";
import { useToggleParam, type ToggleParam } from "../../hooks";

export type BlockToggles = Record<BlockId, ToggleParam>;

export function useBlockToggles(): BlockToggles {
  const gate = useToggleParam("gate_on");
  const comp = useToggleParam("comp_on");
  const drive = useToggleParam("drive_on");
  const amp = useToggleParam("amp_on");
  const cab = useToggleParam("cab_on");
  const eq = useToggleParam("eq_on");
  const mod = useToggleParam("mod_on");
  const delay = useToggleParam("delay_on");
  const reverb = useToggleParam("reverb_on");
  return { gate, comp, drive, amp, cab, eq, mod, delay, reverb };
}

/** Blocks currently bypassed — the set the connector layer dims against. */
export function bypassedBlocks(toggles: BlockToggles): Set<BlockId> {
  const off = new Set<BlockId>();
  for (const id of BLOCK_IDS) if (!toggles[id].value) off.add(id);
  return off;
}
