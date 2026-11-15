/**
 * What the page knows about the plugin that reads the same in every language:
 * names, addresses and the chain. The sentences are in `src/i18n/`.
 */
import type { Arch } from "@/lib/release-schema";

export type Family = "gain" | "dynamics" | "tone" | "mod" | "time";
export type BlockId = "gate" | "comp" | "drive" | "amp" | "cab" | "eq" | "mod" | "delay" | "reverb";

/** `name` is what the plugin calls the block, so it is not translated. What it does is `blocks[id]` in the dictionary. */
export type Block = {
  id: BlockId;
  name: string;
  family: Family;
  types?: readonly string[];
};

export const SITE = {
  name: "tubamp",
  repoUrl: "https://github.com/IamP5/tubamp",
} as const;

/** The blocks in their default order. They can be reordered, removed and added again. */
export const CHAIN: readonly Block[] = [
  { id: "gate", name: "Gate", family: "dynamics" },
  { id: "comp", name: "Compressor", family: "dynamics" },
  { id: "drive", name: "Drive", family: "gain", types: ["Screamer"] },
  { id: "amp", name: "Amp", family: "gain" },
  { id: "cab", name: "Cab", family: "tone" },
  { id: "eq", name: "EQ", family: "tone" },
  { id: "mod", name: "Modulation", family: "mod", types: ["Chorus", "Phaser", "Tremolo"] },
  { id: "delay", name: "Delay", family: "time", types: ["Digital", "Tape", "Analog"] },
  { id: "reverb", name: "Reverb", family: "time", types: ["Plate", "Hall", "Room", "Spring"] },
];

/** `key` is the label's key under `nav.items` in the dictionary. */
export const NAV = [
  { href: "#chain", key: "chain" },
  { href: "#captures", key: "captures" },
  { href: "#effects", key: "effects" },
  { href: "#try", key: "try" },
  { href: "#faq", key: "faq" },
] as const;

export const ARCH_LABELS: Record<Arch, string> = { arm64: "Apple Silicon", x86_64: "Intel" };
