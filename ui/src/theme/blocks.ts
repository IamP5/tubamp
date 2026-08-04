/**
 * Per-block identity: accent colour, display strings, enable-param id.
 *
 * Mirrors `chain::BlockInfo` (src/dsp/ChainOrder.h). Both tables are written
 * once per BASE (the nine kinds plus the three fx slots, in frozen enum order)
 * and mapped onto all 24 instance tokens: an instance shares its kind's colour
 * and reads "Compressor 2" / "COMP 2" off the same entry, so the two can never
 * drift apart.
 */
import type { CSSProperties } from "react";
import {
  blockRecord,
  instanceOf,
  kindOf,
  type BaseBlockId,
  type BlockId,
  type ToggleParamId,
} from "../bridge/types";

const BASE_ACCENT: Record<BaseBlockId, string> = {
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

export const BLOCK_ACCENT: Record<BlockId, string> = blockRecord(
  (id) => BASE_ACCENT[kindOf(id)],
);

export interface BlockInfo {
  id: BlockId;
  /** Full name — panel header, tooltips, menu section headers. */
  displayName: string;
  /** Tile caption, uppercase. */
  shortName: string;
  enableParamId: ToggleParamId;
}

const BASE_INFO: Record<BaseBlockId, { displayName: string; shortName: string }> = {
  gate: { displayName: "Noise Gate", shortName: "GATE" },
  comp: { displayName: "Compressor", shortName: "COMP" },
  drive: { displayName: "Overdrive", shortName: "DRIVE" },
  amp: { displayName: "Amp", shortName: "AMP" },
  cab: { displayName: "Cabinet IR", shortName: "CAB" },
  eq: { displayName: "Tone Stack", shortName: "EQ" },
  mod: { displayName: "Modulation", shortName: "MOD" },
  delay: { displayName: "Delay", shortName: "DELAY" },
  reverb: { displayName: "Reverb", shortName: "REVERB" },
  /* The slot's own identity. Once a plugin is loaded the panel header shows the
     plugin's name instead, but the card caption stays "FX 1" — the slot is the
     thing that lives in the chain, the plugin is what is in it. */
  fx1: { displayName: "FX Slot 1", shortName: "FX 1" },
  fx2: { displayName: "FX Slot 2", shortName: "FX 2" },
  fx3: { displayName: "FX Slot 3", shortName: "FX 3" },
};

export const BLOCK_INFO: Record<BlockId, BlockInfo> = blockRecord((id) => {
  const base = BASE_INFO[kindOf(id)];
  const instance = instanceOf(id);
  return {
    id,
    // Instance 1 keeps the unnumbered name: it is the only one there is until a
    // second is added, and renaming it to "Compressor 1" would churn presets'
    // vocabulary for nothing.
    displayName: instance === 0 ? base.displayName : `${base.displayName} ${instance + 1}`,
    shortName: instance === 0 ? base.shortName : `${base.shortName} ${instance + 1}`,
    // Every block's bypass is `<token>_on` — the rule Parameters.cpp mints them
    // with, for all 24 tokens.
    enableParamId: `${id}_on` as ToggleParamId,
  };
});

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
