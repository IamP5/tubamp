/** Catalog vocabulary + the deterministic art fallback (docs/research/tone3000-api.md §Enums). */
import type { T3kBrowseKind, T3kBrowseShelf, T3kBrowseSort } from "../../bridge";

/** Display names for the API's raw Gear values (`full-rig` is the deprecated
 *  alias of `amp-cab` and can still be on stored tones). */
export const GEAR_LABELS: Record<string, string> = {
  amp: "Amp",
  "amp-cab": "Full rig",
  "full-rig": "Full rig",
  pedal: "Pedal",
  outboard: "Outboard",
  experimental: "Experimental",
  cab: "Cab",
  space: "Space",
};

/** Gear values each kind can filter on — the real Gear enum, split by what
 *  tones of that format carry: captures are amps/rigs/pedals/outboard, IRs are
 *  cabinets and spaces (rooms, plates, halls). */
export const GEAR_FILTERS: Record<T3kBrowseKind, readonly string[]> = {
  models: ["amp", "amp-cab", "pedal", "outboard", "experimental"],
  irs: ["cab", "space"],
};

export const SORT_LABELS: Record<T3kBrowseSort, string> = {
  "best-match": "Best match",
  trending: "Trending",
  newest: "Newest",
  downloads: "Most downloaded",
  oldest: "Oldest",
};

/** Menu order; best-match is only offered with a query. */
export const SORTS: T3kBrowseSort[] = [
  "best-match",
  "trending",
  "newest",
  "downloads",
  "oldest",
];

/** The sort actually in force: no pick = best-match with a query, else trending. */
export function effectiveSort(sort: T3kBrowseSort | null, query: string): T3kBrowseSort {
  const searching = query.trim() !== "";
  if (sort === null || (sort === "best-match" && !searching))
    return searching ? "best-match" : "trending";
  return sort;
}

export const SHELVES: { value: T3kBrowseShelf; label: string }[] = [
  { value: "all", label: "All" },
  { value: "favorites", label: "Favorites" },
  { value: "downloaded", label: "Recent" },
  { value: "created", label: "Mine" },
];

/** The official plugin's empty-state copy for the profile shelves. */
export const SHELF_EMPTY: Record<Exclude<T3kBrowseShelf, "all">, string> = {
  favorites: "Tones you favorite on TONE3000 will show up here.",
  downloaded: "Tones you download on TONE3000 will show up here.",
  created: "Tones you upload to TONE3000 will show up here.",
};

export const SIZE_LABELS: Record<string, string> = {
  standard: "Standard",
  lite: "Lite",
  feather: "Feather",
  nano: "Nano",
  custom: "Custom",
};

export const ARCH_LABELS: Record<string, string> = {
  "1": "A1",
  "2": "A2",
  custom: "Custom",
};

/** TONE3000 License enum → the name a person recognises. */
export const LICENSE_LABELS: Record<string, string> = {
  t3k: "TONE3000 license",
  "cc-by": "CC BY",
  "cc-by-sa": "CC BY-SA",
  "cc-by-nc": "CC BY-NC",
  "cc-by-nc-sa": "CC BY-NC-SA",
  "cc-by-nd": "CC BY-ND",
  "cc-by-nc-nd": "CC BY-NC-ND",
  cco: "CC0",
};

export function formatCount(n: number): string {
  return n >= 1000 ? `${(n / 1000).toFixed(n >= 10000 ? 0 : 1)}k` : String(n);
}

/** The tone's page; the API's `url` when given, else the canonical form. */
export function toneUrl(tone: { id: number; url?: string | null }): string {
  return tone.url && tone.url.startsWith("https://www.tone3000.com/")
    ? tone.url
    : `https://www.tone3000.com/tones/${tone.id}`;
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
