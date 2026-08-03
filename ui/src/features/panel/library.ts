/**
 * Library stepping rules shared by the amp model row and the cab IR row.
 *
 * DOMAIN (current-ui-inventory.md §AmpBody `stepModel` / §CabBody `stepIr`):
 * prev/next over models and IRs is NOT the preset wrap rule. With nothing
 * currently loaded, "next" starts at the FIRST entry and "prev" at the LAST;
 * with something loaded it wraps normally. Keep the asymmetry.
 */
import type { FileEntry, ModelInfo } from "../../bridge";

export function stepIndex(
  currentIndex: number,
  delta: number,
  size: number,
): number | null {
  if (size <= 0) return null;
  if (currentIndex < 0) return delta > 0 ? 0 : size - 1;
  return (currentIndex + delta + size) % size;
}

export function indexOfPath(
  entries: readonly FileEntry[],
  path: string | null | undefined,
): number {
  if (!path) return -1;
  return entries.findIndex((entry) => entry.path === path);
}

/**
 * Is the NAM engine actually running the loaded capture?
 *
 * DOMAIN: the native panel distinguishes "a model path is set" from
 * `namEngine.hasModel()` and shows "NOT RUNNING" for the second case, so a
 * capture that never reached the audio thread cannot look loaded. The bridge
 * reports engine-side facts (sample rate, latency) only once the engine holds
 * the model, so a non-positive sample rate is the "file set, engine down" case.
 *
 * Defensive only today: the bridge never actually reports that state — modelVar()
 * sends null when the engine has no model, and sampleRateHz is never <= 0 — so
 * this is equivalent to `model !== null` until C++ starts emitting the distinction.
 */
export function isEngineLive(model: ModelInfo | null): boolean {
  return model !== null && model.sampleRateHz > 0;
}

/**
 * Badge text for a capture that was taken through a cabinet, or null when it
 * wasn't — or, just as often, when it doesn't say.
 *
 * DOMAIN: `gear_type` is optional in the NAM format. TONE3000's exporter writes
 * it (`amp_cab`, `amp_mic`, …), hand-trained captures usually don't, so a
 * missing gear type means "unknown" and must produce no badge at all — claiming
 * "AMP ONLY" for an unlabelled full rig would be worse than staying quiet.
 */
export function rigLabel(model: ModelInfo | null): string | null {
  if (!model?.includesCab) return null;

  switch (model.gearType) {
    case "cab":
      return "CAB";
    case "full_rig":
      return "FULL RIG";
    case "pedal_amp_cab":
    case "pedal_amp_mic":
      return "PEDAL + AMP + CAB";
    default:
      return "AMP + CAB";
  }
}

/** "48 kHz" — the native status line rounds to whole kHz. */
export function formatSampleRate(hz: number): string {
  return `${Math.round(hz / 1000)} kHz`;
}
