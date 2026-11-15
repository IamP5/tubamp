/**
 * The cab kind's two instances, `cab` and `cab2`: one editor, one parameter
 * set each (`${cab}_lowcut`, `${cab}_highcut`, `${cab}_mix`). The only thing
 * that differs between them is which IR slot of the bridge holds the file, and
 * that lives here and nowhere else. A TONE3000 target for a cab is its token.
 */
import {
  KIND_INSTANCES,
  bridge,
  type BlockId,
  type CabToken,
  type ErrorResult,
  type ImportResult,
  type IrInfo,
  type MissingMedia,
  type SliderParamId,
  type UiState,
} from "../bridge";

export interface CabSlot {
  ir(s: Pick<UiState, "ir" | "irB">): IrInfo | null;
  missing: keyof MissingMedia;
  load(path: string): Promise<ErrorResult>;
  clear(): Promise<void>;
  import(): Promise<ImportResult>;
}

export const CAB_SLOTS: Record<CabToken, CabSlot> = {
  cab: {
    ir: (s) => s.ir,
    missing: "ir",
    load: (path) => bridge.loadIr(path),
    clear: () => bridge.clearIr(),
    import: () => bridge.importIr("cab"),
  },
  cab2: {
    ir: (s) => s.irB,
    missing: "irB",
    load: (path) => bridge.loadIrB(path),
    clear: () => bridge.clearIrB(),
    import: () => bridge.importIr("cab2"),
  },
};

export const CAB_TOKENS: readonly CabToken[] = KIND_INSTANCES.cab;

export function isCabToken(id: string | null | undefined): id is CabToken {
  return (CAB_TOKENS as readonly (string | null | undefined)[]).includes(id);
}

/** "Cab" while one cab is placed; "Cab 1" / "Cab 2" only when both are and a
 *  sentence has to say which. */
export function cabLabel(cab: CabToken, order: readonly BlockId[]): string {
  const both = CAB_TOKENS.every((token) => order.includes(token));
  return both ? `Cab ${CAB_TOKENS.indexOf(cab) + 1}` : "Cab";
}

/** `want` when it is in the chain, else whichever cab is, else `cab`: a load
 *  never lands in a cab the chain does not run. */
export function placedCab(want: CabToken, order: readonly BlockId[]): CabToken {
  if (order.includes(want)) return want;
  return CAB_TOKENS.find((token) => order.includes(token)) ?? "cab";
}

/** A cab's own parameter id. */
export const cabSlider = (cab: CabToken, key: "lowcut" | "highcut" | "mix") =>
  `${cab}_${key}` as SliderParamId;
