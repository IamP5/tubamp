/**
 * The words and number formats the whole UI shares (ui/DESIGN.md §Words), so a
 * parameter reads the same on a knob, in a display and in a tooltip.
 */
import { formatParamValue } from "../bridge/paramMeta";
import type { SliderProperties } from "../bridge/types";

const WORDS: Record<string, string> = {
  THRESH: "Threshold",
  FDBK: "Feedback",
  "PRE-DLY": "Pre-delay",
  "LO CUT": "Low cut",
  "HI CUT": "High cut",
  "X-OVER": "Crossover",
  DAMP: "Damping",
  MOD: "Mod",
  IN: "Input",
  OUT: "Output",
  "SC HPF": "Sidechain HPF",
  "A LEVEL": "A level",
  "B LEVEL": "B level",
  "A PAN": "A pan",
  "B PAN": "B pan",
  "CAL LEVEL": "Cal level",
  Q: "Q",
  EQ: "EQ",
};

/** A raw label ("THRESH", "LO CUT") as the words a person reads: sentence case,
 *  real acronyms kept. Already-sentence-case input passes through unchanged. */
export function paramLabel(label: string): string {
  const known = WORDS[label.toUpperCase()] ?? WORDS[label];
  if (known) return known;
  if (label !== label.toUpperCase()) return label;
  const lower = label.toLowerCase();
  return lower.charAt(0).toUpperCase() + lower.slice(1);
}

/** "C", "33L", "33R" for a −1..1 pan. */
export function panText(scaled: number): string {
  const pct = Math.round(Math.abs(scaled) * 100);
  if (pct === 0) return "C";
  return `${pct}${scaled < 0 ? "L" : "R"}`;
}

/**
 * A slider value as every surface prints it: pan as C / 33L / 33R, a unitless
 * 0..1 amount as a percentage, kHz above 1000 Hz, seconds above 1000 ms, and a
 * ratio or percentage without the stray space.
 */
export function paramValue(
  id: string,
  scaled: number,
  properties: Pick<SliderProperties, "interval" | "label" | "start" | "end">,
): string {
  if (id.endsWith("pan")) return panText(scaled);
  const unit = properties.label;
  if (!unit && properties.start >= 0 && properties.end <= 1) {
    return `${Math.round(scaled * 100)}%`;
  }
  if (unit === "Hz" && Math.abs(scaled) >= 1000) {
    const k = scaled / 1000;
    return `${k >= 10 ? k.toFixed(1) : k.toFixed(2)} kHz`;
  }
  if (unit === "ms" && Math.abs(scaled) >= 1000) {
    return `${(scaled / 1000).toFixed(2)} s`;
  }
  return formatParamValue(scaled, properties)
    .replace(" :1", ":1")
    .replace(/(?:\.0+)? %$/, "%");
}

/** Engine status as it is printed once, in the status bar: "48 kHz · 1.9 ms". */
export function engineStatus(sampleRate: number, latencySamples: number): string {
  if (!sampleRate) return "";
  const khz = sampleRate / 1000;
  const rate = `${Number.isInteger(khz) ? khz : khz.toFixed(1)} kHz`;
  const ms = (latencySamples / sampleRate) * 1000;
  return `${rate} · ${ms.toFixed(1)} ms`;
}
