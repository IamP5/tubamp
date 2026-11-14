/**
 * Lane identity — what the two lanes of a split region are CALLED and what
 * colour they wear, everywhere they are named: the board's lane badges, lane
 * wires and lane [+] slots, the SPLIT/MIX card notes, and the Splitter and
 * Mixer panels. One table, so a lane is never "A" on the board and "L" in a
 * panel.
 *
 * Names follow `split_mode` (docs/SPLIT.md §2, frozen choice order):
 *   Copy   → A / B      both lanes get the full signal
 *   L/R    → L / R      lane A takes the left input channel, lane B the right
 *   X-Over → LOW / HIGH lane A takes everything below the crossover, B above
 * (src/PluginProcessor.cpp `splitLanes` is the authority for those meanings;
 * a mono host splits L/R like Copy.)
 *
 * Colours are deliberately quiet — a warm and a cool tint at low alpha, so the
 * lanes read as two tracks of one region rather than two more block hues. Low
 * is warm and high is cool, which is also the only colour convention any of
 * the three modes has. Literal rgba, never color-mix (Safari 14 floor).
 */
import { useComboParam, useSliderParam } from "../../hooks";
import type { BlockId } from "../../bridge/types";
import type { LaneSide } from "../../chain/structure";

export type { LaneSide };

/** `split_mode` choice indices, frozen with the parameter. */
export const SPLIT_COPY = 0;
export const SPLIT_LR = 1;
export const SPLIT_XOVER = 2;

export interface LaneTint {
  /** Badge text / icon colour. */
  ink: string;
  /** Badge fill. */
  fill: string;
  /** Badge ring and dashed slot border. */
  ring: string;
  /** Lane wire stroke. */
  wire: string;
}

export const LANE_TINT: Record<LaneSide, LaneTint> = {
  a: {
    ink: "#f2c27b",
    fill: "rgba(242, 194, 123, 0.12)",
    ring: "rgba(242, 194, 123, 0.38)",
    wire: "rgba(242, 194, 123, 0.5)",
  },
  b: {
    ink: "#8fb8ff",
    fill: "rgba(143, 184, 255, 0.12)",
    ring: "rgba(143, 184, 255, 0.38)",
    wire: "rgba(143, 184, 255, 0.5)",
  },
};

const NAMES: readonly Record<LaneSide, string>[] = [
  { a: "A", b: "B" },
  { a: "L", b: "R" },
  { a: "LOW", b: "HIGH" },
];

/** Badge text for each lane in split mode `mode`. */
export function laneNames(mode: number): Record<LaneSide, string> {
  return NAMES[mode] ?? NAMES[SPLIT_COPY]!;
}

/** "800 Hz" / "1.2 kHz" — the crossover as a person would say it. */
export function formatHz(hz: number): string {
  if (hz >= 1000) {
    const k = hz / 1000;
    return `${k >= 10 ? Math.round(k) : Math.round(k * 10) / 10} kHz`;
  }
  return `${Math.round(hz)} Hz`;
}

/** What lane `side` receives in `mode`, as a sentence (badge tooltip). */
export function laneHint(mode: number, side: LaneSide, xoverHz: number): string {
  const name = laneNames(mode)[side];
  if (mode === SPLIT_LR)
    return side === "a"
      ? `Lane ${name} receives the left input channel.`
      : `Lane ${name} receives the right input channel.`;
  if (mode === SPLIT_XOVER)
    return side === "a"
      ? `Lane ${name} receives everything below ${formatHz(xoverHz)}.`
      : `Lane ${name} receives everything above ${formatHz(xoverHz)}.`;
  return `Lane ${name} receives the full signal.`;
}

/** One line for the Splitter panel: what each lane gets in `mode`. */
export function modeSummary(mode: number, xoverHz: number): string {
  if (mode === SPLIT_LR)
    return "Lane L gets the left input channel, lane R the right. On a mono track both get the same signal.";
  if (mode === SPLIT_XOVER)
    return `Lane LOW gets everything below ${formatHz(xoverHz)}, lane HIGH everything above it.`;
  return "Both lanes get the full signal — two rigs, side by side.";
}

/** Pan position the way a console scribble strip shows it: C, 40L, 100R. */
export function formatPan(value: number): string {
  const pct = Math.round(Math.abs(value) * 100);
  if (pct === 0) return "C";
  return `${pct}${value < 0 ? "L" : "R"}`;
}

/** Level for a compact read-out: "" at unity, otherwise "-6dB" / "+1.5dB". */
export function formatLevelShort(db: number): string {
  const r = Math.round(db * 10) / 10;
  if (r === 0) return "";
  const text = Number.isInteger(r) ? r.toFixed(0) : r.toFixed(1);
  return `${r > 0 ? "+" : ""}${text}dB`;
}

/** The split's current mode, its lane names and the crossover — the board and
 *  both panels read lanes through this, so they can never disagree. */
export function useLaneNames(): {
  mode: number;
  names: Record<LaneSide, string>;
} {
  const mode = useComboParam("split_mode").index;
  return { mode, names: laneNames(mode) };
}

/** The crossover in Hz, for copy that mentions it. Re-renders on every change
 *  of the knob — keep it in leaf components. */
export function useXoverHz(): number {
  return useSliderParam("split_xover").scaled;
}

/* ───────────────────────────── power wording ────────────────────────────── */

/**
 * What a block's power control does, in the words that are true for it. SPLIT
 * and MIX are not bypasses in the ordinary sense — `split_on` off mutes lane B
 * at the mixer and `mix_on` off sums the lanes at unity (PluginProcessor.cpp,
 * `mixLanes`) — and "Bypass" on a dual-amp rig would silence one amp without
 * saying so.
 */
export function powerLabel(block: BlockId, enabled: boolean): string {
  if (block === "split") return enabled ? "Mute lane B" : "Unmute lane B";
  if (block === "mix") return enabled ? "Sum lanes at unity" : "Use levels and pans";
  return enabled ? "Bypass" : "Enable";
}

export function powerTip(block: BlockId): string {
  if (block === "split") return "Off mutes lane B: only lane A reaches the mixer";
  if (block === "mix") return "Off sums the two lanes at unity, ignoring levels, pans and invert";
  return "Enable or bypass this block";
}
