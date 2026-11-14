/**
 * Every clip on the page. Each has `<name>.mp4`, `<name>.webm` and
 * `<name>.jpg` (poster) under public/media/<dir>/.
 *
 * `ui`: the real editor, recorded by `npm run capture` (scripts/capture.mjs).
 * `film`: generated atmosphere footage, never shown as the product.
 */
export type Clip = { dir: "ui" | "film"; name: string; width: number; height: number; alt: string };

const board = { dir: "ui", width: 1280, height: 800 } as const;
const editor = { dir: "ui", width: 1280, height: 296 } as const;
const film = { dir: "film", width: 1920, height: 1080 } as const;

export const CLIPS = {
  chainBuild: { name: "chain-build", ...board, alt: "Blocks being added to the chain on the board and dragged into a new order." },
  presets: { name: "presets", ...board, alt: "Stepping through presets; the chain rearranges for each one." },
  t3k: { name: "t3k", ...board, alt: "The TONE3000 browser open inside the plugin, scrolling captures and loading one." },
  amp: { name: "amp", ...board, alt: "The amp editor stepping through captures in the library." },
  split: { name: "split", ...board, alt: "A chain split into two lanes with a second amp and a mixer." },
  compare: { name: "compare", ...board, alt: "Switching between compare slots A and B." },
  gate: { name: "fx-gate", ...editor, alt: "The gate editor: the input level over time with a draggable threshold." },
  comp: { name: "fx-comp", ...editor, alt: "The compressor editor: its transfer curve changing with threshold and ratio." },
  drive: { name: "fx-drive", ...editor, alt: "The drive editor: the waveshaper curve bending as gain rises." },
  cab: { name: "fx-cab", ...editor, alt: "The cab editor: low and high cut on the response curve." },
  eq: { name: "fx-eq", ...editor, alt: "The EQ editor: dragging bands on the response curve." },
  mod: { name: "fx-mod", ...editor, alt: "The modulation editor: the LFO wave changing with rate and depth." },
  delay: { name: "fx-delay", ...editor, alt: "The delay editor: taps spreading as time and feedback change." },
  reverb: { name: "fx-reverb", ...editor, alt: "The reverb editor: the tail growing with decay." },
} as const satisfies Record<string, Clip>;

export type ClipKey = keyof typeof CLIPS;

export const FILMS = {
  tubes: { name: "tubes", ...film, alt: "Valve filaments glowing amber in the dark." },
  strings: { name: "strings", ...film, alt: "Three wound guitar strings lit amber on black, one of them vibrating." },
  speaker: { name: "speaker", ...film, alt: "A black speaker cone with an amber rim light, dust lifting off it." },
} as const satisfies Record<string, Clip>;

export const clipSrc = (clip: Clip) => ({
  mp4: `/media/${clip.dir}/${clip.name}.mp4`,
  webm: `/media/${clip.dir}/${clip.name}.webm`,
  poster: `/media/${clip.dir}/${clip.name}.jpg`,
});
