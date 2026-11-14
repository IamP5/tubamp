/**
 * Note values of the delay and modulation Sync menus, for the displays.
 *
 * The UI is not told the host tempo, so a synced time is drawn at 120 BPM —
 * the tempo the engines themselves fall back to when the host reports none
 * (docs/REVERB-DELAY.md §1.1). The knob shows the note value; only the picture
 * assumes the tempo.
 */

export const DRAW_BPM = 120;

/** "1/8." → 0.75 quarter-note beats; "Off" or anything else → null. Covers
 *  plain, triplet (T) and dotted (.) values, and whole multiples ("2/1"). */
export function noteBeats(text: string): number | null {
  const match = /^(\d+)\/(\d+)([T.])?$/.exec(text.trim());
  if (!match) return null;
  const beats = (4 * Number(match[1])) / Number(match[2]);
  if (match[3] === "T") return (beats * 2) / 3;
  if (match[3] === ".") return beats * 1.5;
  return beats;
}

/** Seconds a note value lasts at the drawing tempo; null when not synced. */
export function noteSeconds(text: string): number | null {
  const beats = noteBeats(text);
  return beats === null ? null : (beats * 60) / DRAW_BPM;
}
