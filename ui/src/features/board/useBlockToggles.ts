/**
 * Every block's `*_on` parameter, bound once at board level.
 *
 * Twenty-four fixed `useToggleParam` calls (BLOCK_IDS is frozen, so the hook
 * count is constant) — the card LEDs, the kebab menu labels and the connector
 * dimming all read from the same snapshot, and none of them has to subscribe
 * individually. Replaces the native view's 100ms `refreshFromParams` poll with a
 * real subscription, so host automation / A-B recall shows up immediately.
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
  const fx1 = useToggleParam("fx1_on");
  const fx2 = useToggleParam("fx2_on");
  const fx3 = useToggleParam("fx3_on");
  const comp2 = useToggleParam("comp2_on");
  const comp3 = useToggleParam("comp3_on");
  const drive2 = useToggleParam("drive2_on");
  const drive3 = useToggleParam("drive3_on");
  const eq2 = useToggleParam("eq2_on");
  const eq3 = useToggleParam("eq3_on");
  const mod2 = useToggleParam("mod2_on");
  const mod3 = useToggleParam("mod3_on");
  const delay2 = useToggleParam("delay2_on");
  const delay3 = useToggleParam("delay3_on");
  const reverb2 = useToggleParam("reverb2_on");
  const reverb3 = useToggleParam("reverb3_on");
  return {
    gate,
    comp,
    drive,
    amp,
    cab,
    eq,
    mod,
    delay,
    reverb,
    fx1,
    fx2,
    fx3,
    comp2,
    comp3,
    drive2,
    drive3,
    eq2,
    eq3,
    mod2,
    mod3,
    delay2,
    delay3,
    reverb2,
    reverb3,
  };
}

/** Blocks currently bypassed — the set the connector layer dims against. */
export function bypassedBlocks(toggles: BlockToggles): Set<BlockId> {
  const off = new Set<BlockId>();
  for (const id of BLOCK_IDS) if (!toggles[id].value) off.add(id);
  return off;
}
