/**
 * Per-block identity: accent colour, display strings, enable-param id.
 *
 * Mirrors `chain::BlockInfo` (src/dsp/ChainOrder.h) — the array order below is
 * the frozen enum order, which is also the add-menu order and `defaultOrder()`.
 */
import type { CSSProperties } from "react";
import { BLOCK_IDS, type BlockId, type ToggleParamId } from "../bridge/types";

export const BLOCK_ACCENT: Record<BlockId, string> = {
  gate: "#a78bfa",
  comp: "#fbbf24",
  drive: "#f97316",
  amp: "#f43f5e",
  cab: "#2dd4bf",
  eq: "#60a5fa",
  mod: "#4ade80",
  delay: "#818cf8",
  reverb: "#22d3ee",
  /* One hue for all three external-AU slots: they are one kind of block, and
     three more distinct hues next to the nine built-ins would read as three more
     built-ins. Which slot it is comes from the caption ("FX 1"), not the colour. */
  fx1: "#e879f9",
  fx2: "#e879f9",
  fx3: "#e879f9",
};

export interface BlockInfo {
  id: BlockId;
  /** Full name — panel header, tooltips, menu section headers. */
  displayName: string;
  /** Tile caption, uppercase. */
  shortName: string;
  enableParamId: ToggleParamId;
}

export const BLOCK_INFO: Record<BlockId, BlockInfo> = {
  gate: {
    id: "gate",
    displayName: "Noise Gate",
    shortName: "GATE",
    enableParamId: "gate_on",
  },
  comp: {
    id: "comp",
    displayName: "Compressor",
    shortName: "COMP",
    enableParamId: "comp_on",
  },
  drive: {
    id: "drive",
    displayName: "Overdrive",
    shortName: "DRIVE",
    enableParamId: "drive_on",
  },
  amp: {
    id: "amp",
    displayName: "Amp",
    shortName: "AMP",
    enableParamId: "amp_on",
  },
  cab: {
    id: "cab",
    displayName: "Cabinet IR",
    shortName: "CAB",
    enableParamId: "cab_on",
  },
  eq: {
    id: "eq",
    displayName: "Tone Stack",
    shortName: "EQ",
    enableParamId: "eq_on",
  },
  mod: {
    id: "mod",
    displayName: "Modulation",
    shortName: "MOD",
    enableParamId: "mod_on",
  },
  delay: {
    id: "delay",
    displayName: "Delay",
    shortName: "DELAY",
    enableParamId: "delay_on",
  },
  reverb: {
    id: "reverb",
    displayName: "Reverb",
    shortName: "REVERB",
    enableParamId: "reverb_on",
  },
  /* The slot's own identity. Once a plugin is loaded the dock header shows the
     plugin's name instead, but the card caption stays "FX 1" — the slot is the
     thing that lives in the chain, the plugin is what is in it. */
  fx1: {
    id: "fx1",
    displayName: "FX Slot 1",
    shortName: "FX 1",
    enableParamId: "fx1_on",
  },
  fx2: {
    id: "fx2",
    displayName: "FX Slot 2",
    shortName: "FX 2",
    enableParamId: "fx2_on",
  },
  fx3: {
    id: "fx3",
    displayName: "FX Slot 3",
    shortName: "FX 3",
    enableParamId: "fx3_on",
  },
};

/** Frozen enum order — default chain order and add-menu order. */
export const DEFAULT_BLOCK_ORDER: readonly BlockId[] = BLOCK_IDS;

/** --bg-surface from theme/tokens.css. --block-dim is flattened against it here
 *  because color-mix() (which could do it live) needs Safari 16.2 and the build
 *  baseline is safari14. */
const BG_SURFACE = "#12151b";

function channels(hex: string): [number, number, number] {
  const n = parseInt(hex.slice(1), 16);
  return [(n >> 16) & 0xff, (n >> 8) & 0xff, n & 0xff];
}

/** `color-mix(in srgb, hex <pct>%, transparent)` — mixing with transparent
 *  leaves the channels alone and lands the alpha on the percentage. */
function tint(hex: string, alpha: number): string {
  const [r, g, b] = channels(hex);
  return `rgba(${r}, ${g}, ${b}, ${alpha})`;
}

/** `color-mix(in srgb, hex <pct>%, over)` between two opaque colours. */
function blend(hex: string, pct: number, over: string): string {
  const a = channels(hex);
  const b = channels(over);
  const c = a.map((v, i) => Math.round(v * pct + b[i]! * (1 - pct)));
  return `rgb(${c[0]}, ${c[1]}, ${c[2]})`;
}

/**
 * Inline equivalent of the `[data-block="…"]` rules in tokens.css, for elements
 * where setting the attribute is awkward (portalled menus, SVG connectors).
 * Prefer the attribute when you can — it keeps the style object stable.
 *
 * Must stay byte-identical in effect to those rules: same percentages, same
 * surface colour.
 */
export function blockCssVars(id: BlockId): CSSProperties {
  const c = BLOCK_ACCENT[id];
  return {
    "--block-accent": c,
    "--block-fill-8": tint(c, 0.08),
    "--block-fill-16": tint(c, 0.16),
    "--block-fill-25": tint(c, 0.25),
    "--block-glow": tint(c, 0.35),
    "--block-dim": blend(c, 0.55, BG_SURFACE),
  } as CSSProperties;
}
