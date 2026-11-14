/** The line shown when Drive and Reverb trade places. Belongs in src/data/site.ts once that file is open to edits. */
export const SWAP_LINE = {
  title: "Any order.",
  body: "Swap the drive and the reverb, and the whole tail distorts.",
} as const;

export const pad = (n: number) => String(n).padStart(2, "0");
