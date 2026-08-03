/**
 * "Does the loaded capture already carry a cabinet, and is the Cab block about
 * to stack a second one on top of it?"
 *
 * DOMAIN: NAM captures declare what was recorded in `metadata.gear_type`
 * (docs/REACT-UI.md §Bridge contract). A rig taken through a cab or a mic
 * (`amp_cab`, `amp_mic`, the `pedal_amp_*` variants) already contains the
 * speaker, so an IR after it is a second cabinet in series — usually a mistake,
 * occasionally deliberate, never something the UI should silently fix.
 *
 * Two levels, because they answer different questions:
 *  - `captureHasCab` is a property of the file. It holds whether or not the
 *    capture is currently being heard, so the amp badge is stable.
 *  - `doubleCab` is a property of the *signal path*: both blocks have to be in
 *    the chain and live for the stacking to actually happen. Bypassing either
 *    one resolves it, which is exactly what the cab panel offers to do.
 *
 * A capture with no gear metadata at all reports false for both — absence of a
 * declaration is not a declaration of absence.
 */
import type { BlockId } from "../bridge/types";
import { useStore } from "../store";
import { useToggleParam } from "./useToggleParam";

export interface RigCab {
  captureHasCab: boolean;
  doubleCab: boolean;
}

export function useRigCab(): RigCab {
  const model = useStore((st) => st.model);
  const chainOrder = useStore((st) => st.chainOrder);
  const ampOn = useToggleParam("amp_on");
  const cabOn = useToggleParam("cab_on");

  const captureHasCab = model?.includesCab ?? false;
  const inPath = (block: BlockId, on: boolean) => on && chainOrder.includes(block);

  return {
    captureHasCab,
    doubleCab:
      captureHasCab &&
      inPath("amp", ampOn.value) &&
      inPath("cab", cabOn.value),
  };
}
