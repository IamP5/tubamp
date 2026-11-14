/**
 * Library stepping rules shared by the media cards' library rows (amp, amp 2,
 * cab).
 *
 * DOMAIN (current-ui-inventory.md §AmpBody `stepModel` / §CabBody `stepIr`):
 * prev/next over models and IRs is NOT the preset wrap rule. With nothing
 * currently loaded, "next" starts at the FIRST entry and "prev" at the LAST;
 * with something loaded it wraps normally. Keep the asymmetry.
 */
import type { FileEntry } from "../../bridge";

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
