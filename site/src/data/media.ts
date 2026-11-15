/**
 * Each clip has `<name>.mp4`, `<name>.webm` and `<name>.jpg` (poster) under
 * public/media/<dir>/.
 *
 * `ui`: the real editor, recorded by `npm run capture` (scripts/capture.mjs).
 * `film`: generated atmosphere footage, never shown as the product.
 */
import type { Dictionary } from "../i18n/en.ts";

export type MediaName = keyof Dictionary["media"];
/** `name` is also the key of the clip's description under `media` in the dictionaries (`src/i18n/`). */
export type Clip = { dir: "ui" | "film"; name: MediaName; width: number; height: number };

const board = { dir: "ui", width: 1280, height: 800 } as const;
const editor = { dir: "ui", width: 1280, height: 296 } as const;
const film = { dir: "film", width: 1920, height: 1080 } as const;
const loop = { dir: "film", width: 1280, height: 720 } as const;

export const CLIPS = {
  chainBuild: { name: "chain-build", ...board },
  presets: { name: "presets", ...board },
  t3k: { name: "t3k", ...board },
  amp: { name: "amp", ...board },
  split: { name: "split", ...board },
  compare: { name: "compare", ...board },
  gate: { name: "fx-gate", ...editor },
  comp: { name: "fx-comp", ...editor },
  drive: { name: "fx-drive", ...editor },
  cab: { name: "fx-cab", ...editor },
  eq: { name: "fx-eq", ...editor },
  mod: { name: "fx-mod", ...editor },
  delay: { name: "fx-delay", ...editor },
  reverb: { name: "fx-reverb", ...editor },
} as const satisfies Record<string, Clip>;

export type ClipKey = keyof typeof CLIPS;

export const FILMS = {
  tubes: { name: "tubes", ...film },
  strings: { name: "strings", ...film },
  capture: { name: "capture-720", ...loop },
  speaker: { name: "speaker", ...film },
} as const satisfies Record<string, Clip>;

export const clipSrc = (clip: Clip) => ({
  mp4: `/media/${clip.dir}/${clip.name}.mp4`,
  webm: `/media/${clip.dir}/${clip.name}.webm`,
  poster: `/media/${clip.dir}/${clip.name}.jpg`,
});

/**
 * A film as numbered stills (`001.avif` onwards) so the scroll can show any
 * frame at once; a video has to seek. One folder per tier, named by its width.
 * `npm run film:frames` writes them.
 */
export type Sequence = {
  base: string;
  count: number;
  poster: string;
  tiers: readonly { width: number; height: number }[];
};

export const CAPTURE_SEQUENCE: Sequence = {
  base: "/media/film/capture",
  count: 121,
  poster: "/media/film/capture.jpg",
  tiers: [
    { width: 1920, height: 1080 },
    { width: 2560, height: 1440 },
  ],
};

export const frameSrc = (seq: Sequence, width: number, index: number) =>
  `${seq.base}/${width}/${String(index + 1).padStart(3, "0")}.avif`;
