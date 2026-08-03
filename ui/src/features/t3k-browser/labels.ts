/** Catalog vocabulary + the deterministic art fallback (docs/research/tone3000-api.md §Enums). */
import type { T3kBrowseSort } from "../../bridge";

export const GEAR_LABELS: Record<string, string> = {
  amp: "Amp",
  "amp-cab": "Full rig",
  pedal: "Pedal",
  cab: "Cab",
};

/** Gear values the browse request may carry; models only (IR tones are all cabs). */
export const GEAR_FILTERS = ["amp", "amp-cab", "pedal"] as const;

export const SORT_LABELS: Record<T3kBrowseSort, string> = {
  trending: "Trending",
  newest: "Newest",
  downloads: "Most downloaded",
};

export const SORTS: T3kBrowseSort[] = ["trending", "newest", "downloads"];

export const SIZE_LABELS: Record<string, string> = {
  standard: "Standard",
  lite: "Lite",
  feather: "Feather",
  nano: "Nano",
  custom: "Custom",
};

export function formatCount(n: number): string {
  return n >= 1000 ? `${(n / 1000).toFixed(n >= 10000 ? 0 : 1)}k` : String(n);
}

/**
 * Two hues hashed from the tone id — the placeholder that stands in whenever a
 * tone has no image or the WebView refuses to load the remote one. Deterministic
 * so a tone keeps the same art across pages and sessions.
 */
export function artHues(id: number): [number, number] {
  const hash = Math.abs(Math.imul(id, 2654435761));
  const from = hash % 360;
  return [from, (from + 25 + (hash % 40)) % 360];
}
