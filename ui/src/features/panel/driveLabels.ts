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
