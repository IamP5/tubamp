/**
 * PROTOTYPE — THROWAWAY. Shared mock data + state for the TONE3000 in-plugin
 * browser prototype (docs/research/tone3000-api.md shapes, faked locally).
 * Not production code: no error handling, in-memory only, delete after the
 * design question ("what should the browser look like?") is answered.
 */
import { create } from "zustand";

/* ────────────────────────────── shapes ────────────────────────────── */

/** Mirrors the API's Tone (subset the browser UI needs). */
export interface ProtoTone {
  id: number;
  title: string;
  creator: { username: string; avatarHue: number };
  /** 'nam' tones are amp captures; 'ir' tones are cab impulse responses. */
  format: "nam" | "ir";
  gear: "amp" | "amp-cab" | "pedal" | "cab";
  description: string;
  /** Placeholder art: CSS gradient stops (real UI will show t3k images). */
  art: [string, string];
  downloadsCount: number;
  favoritesCount: number;
  makes: string[];
  tags: string[];
  sizes: ("standard" | "lite" | "feather" | "nano")[];
  /** ISO date — drives the "newest" sort. */
  createdAt: string;
  models: ProtoModel[];
}

/** Mirrors the API's Model (a downloadable file within a tone). */
export interface ProtoModel {
  id: number;
  name: string;
  size: "standard" | "lite" | "feather" | "nano";
  /** '2' = NAM A2 badge; null for IR .wav files. */
  architecture: "2" | null;
  fileKind: "nam" | "wav";
}

export type ProtoSort = "trending" | "newest" | "downloads";
export type ProtoKind = "models" | "irs";

export interface ProtoQuery {
  kind: ProtoKind;
  sort: ProtoSort;
  query: string;
  gear: string | null; // amp | amp-cab | pedal (models only)
  size: string | null; // standard | lite | feather (models only)
  favoritesOnly: boolean;
}

/* ─────────────────────────── mock catalog ─────────────────────────── */

let nextModelId = 9000;
const files = (
  toneTitle: string,
  sizes: ProtoTone["sizes"],
  format: "nam" | "ir",
): ProtoModel[] =>
  format === "ir"
    ? [1, 2, 3].slice(0, 1 + (toneTitle.length % 3)).map((n) => ({
        id: nextModelId++,
        name: `${toneTitle} — position ${n}`,
        size: "standard" as const,
        architecture: null,
        fileKind: "wav" as const,
      }))
    : sizes.map((size) => ({
        id: nextModelId++,
        name: `${toneTitle} (${size})`,
        size,
        architecture: "2" as const,
        fileKind: "nam" as const,
      }));

let nextToneId = 100;
const tone = (
  title: string,
  creator: string,
  format: "nam" | "ir",
  gear: ProtoTone["gear"],
  description: string,
  art: [string, string],
  downloads: number,
  favorites: number,
  makes: string[],
  tags: string[],
  sizes: ProtoTone["sizes"],
  createdAt: string,
): ProtoTone => ({
  id: nextToneId++,
  title,
  creator: {
    username: creator,
    avatarHue: (creator.charCodeAt(0) * 47) % 360,
  },
  format,
  gear,
  description,
  art,
  downloadsCount: downloads,
  favoritesCount: favorites,
  makes,
  tags,
  sizes,
  createdAt,
  models: files(title, sizes, format),
});

/** ~30 tones. Trending ≠ downloads ≠ newest so the sorts visibly differ. */
export const CATALOG: ProtoTone[] = [
  tone("Deluxe Reverb '65 — Edge of Breakup", "amalgam_audio", "nam", "amp", "Blackface AB763 captured at the sweet spot, vol 6. Touch-sensitive cleans that grit up when you dig in.", ["#8a3b12", "#1a0f0a"], 48213, 3120, ["Fender"], ["clean", "blackface", "breakup"], ["standard", "lite", "feather"], "2026-03-14"),
  tone("JCM800 2203 — Full Stack Roar", "tonehound", "nam", "amp", "1982 JCM800, master at 8. The rock rhythm channel. Pairs best with a greenback IR.", ["#b3541e", "#12100e"], 61540, 4470, ["Marshall"], ["rock", "crunch", "british"], ["standard", "lite"], "2025-11-02"),
  tone("AC30 Top Boost — Chime Machine", "brit_captures", "nam", "amp", "Vox AC30/6 TB, cut at noon. Jangle and chime with EL84 compression when pushed.", ["#c2841a", "#141006"], 35872, 2910, ["Vox"], ["chime", "british", "cleanish"], ["standard", "lite", "feather"], "2026-01-20"),
  tone("Dual Rectifier — Modern Scoop", "murkytones", "nam", "amp", "3-channel Recto, modern voicing, bold power. Tight palm mutes, huge low end.", ["#7d1f2d", "#0d0a10"], 52108, 3350, ["Mesa/Boogie"], ["metal", "high-gain", "scooped"], ["standard", "lite"], "2025-09-18"),
  tone("5150 Block Letter — Lead Channel", "riffworks", "nam", "amp", "The classic brown sound sequel. Lead channel, gain at 6, resonance 4.", ["#8f2727", "#100c0c"], 44930, 3860, ["Peavey"], ["high-gain", "lead", "hot-rodded"], ["standard", "lite", "feather"], "2026-05-30"),
  tone("Princeton Reverb — Bedroom Cream", "amalgam_audio", "nam", "amp-cab", "Full-rig capture: '68 Princeton into its stock 10\" through a 57. Instant record-ready clean.", ["#a86a2f", "#161009"], 29804, 2150, ["Fender"], ["clean", "full-rig", "lofi"], ["standard", "feather"], "2026-06-25"),
  tone("Plexi Super Lead — Vintage Growl", "tonehound", "nam", "amp", "1969 Super Lead jumped channels. Woody midrange growl, cleans up beautifully off the guitar volume.", ["#9c5518", "#130e08"], 38455, 3020, ["Marshall"], ["classic-rock", "plexi", "vintage"], ["standard", "lite"], "2025-12-11"),
  tone("Bassman '59 — Tweed Warmth", "vintage_vault", "nam", "amp", "Tweed Bassman 4×10 combo head section. The circuit every amp copied. Warm, midrangey push.", ["#b07d3a", "#15100a"], 21600, 1740, ["Fender"], ["tweed", "vintage", "blues"], ["standard", "lite", "feather"], "2026-02-08"),
  tone("SLO-100 — Liquid Lead", "labtones", "nam", "amp", "Soldano SLO overdrive channel. Smooth, singing sustain — the LA session lead sound.", ["#6d2f6b", "#0f0a12"], 26377, 2280, ["Soldano"], ["lead", "high-gain", "smooth"], ["standard", "lite"], "2026-04-19"),
  tone("BE-100 — Modern Brit Crunch", "riffworks", "nam", "amp", "Friedman BE channel, structure tight. Hot-rodded Marshall DNA with modern focus.", ["#a03a2a", "#120c0a"], 31240, 2540, ["Friedman"], ["crunch", "modern", "british"], ["standard", "lite", "feather"], "2026-07-12"),
  tone("JC-120 — Glass Cleans", "murkytones", "nam", "amp", "Roland Jazz Chorus, bright switch on. The flattest, glassiest solid-state clean ever made.", ["#2a6d8f", "#0a1014"], 18922, 1490, ["Roland"], ["clean", "solid-state", "glassy"], ["standard", "feather", "nano"], "2026-07-28"),
  tone("Rockerverb 50 — Citrus Grind", "brit_captures", "nam", "amp", "Orange Rockerverb dirty channel. Thick, saturated British grind with that Orange midrange bark.", ["#c26a1a", "#140f08"], 15733, 1180, ["Orange"], ["stoner", "crunch", "thick"], ["standard", "lite"], "2026-07-20"),
  tone("Ecstasy Blue — Boutique Drive", "labtones", "nam", "amp", "Bogner Ecstasy blue channel, plexi mode. Refined boutique overdrive with 3D depth.", ["#2f4f8f", "#0a0d14"], 12048, 986, ["Bogner"], ["boutique", "drive", "dynamic"], ["standard", "lite", "feather"], "2026-07-31"),
  tone("Dumble ODS — Holy Grail", "vintage_vault", "nam", "amp", "Overdrive Special #124 clone, OD channel. The most-imitated boutique voice, captured direct.", ["#6b4f1e", "#0f0d08"], 40120, 4980, ["Dumble"], ["boutique", "smooth", "grail"], ["standard"], "2026-06-02"),
  tone("Klon-Boosted Plexi Stack", "riffworks", "nam", "amp-cab", "Full-rig: Klon into '71 plexi into 4×12. Ready-to-play classic rock lead rig.", ["#8f6d1e", "#110e08"], 24310, 1870, ["Marshall", "Klon"], ["full-rig", "boosted", "lead"], ["standard", "lite"], "2026-07-05"),
  tone("Twin Reverb — Pedal Platform", "amalgam_audio", "nam", "amp", "'72 Twin at stage volume. Massive clean headroom — the definitive pedal platform.", ["#3a7d6b", "#0a1210"], 33590, 2400, ["Fender"], ["clean", "headroom", "platform"], ["standard", "lite", "feather"], "2025-10-27"),
  tone("Matchless DC30 — Class A Sparkle", "brit_captures", "nam", "amp", "DC30 channel 1, EF86 voice. Chimey class-A sparkle that sits perfectly in a mix.", ["#4f8f2f", "#0c1208"], 9840, 812, ["Matchless"], ["boutique", "chime", "class-a"], ["standard", "lite"], "2026-08-01"),
  tone("Two-Rock Classic — Studio Clean", "labtones", "nam", "amp", "Classic Reverb Signature. Big, bold, hi-fi cleans with Dumble lineage.", ["#2f6d8f", "#0a1014"], 11220, 940, ["Two-Rock"], ["clean", "boutique", "hifi"], ["standard", "feather"], "2026-07-25"),
  tone("RAT on the Edge", "pedal_lab", "nam", "pedal", "ProCo RAT distortion pedal capture, filter at 2 o'clock. Stack it in front of a clean amp model.", ["#454545", "#0d0d0d"], 8102, 640, ["ProCo"], ["pedal", "distortion", "stackable"], ["standard", "feather", "nano"], "2026-07-15"),
  tone("Tube Screamer TS808 — Mid Hump", "pedal_lab", "nam", "pedal", "The green machine at drive 3 / level 7. The eternal boost. A2 capture, nano available.", ["#2f8f4f", "#081208"], 14980, 1120, ["Ibanez"], ["pedal", "overdrive", "boost"], ["standard", "nano"], "2026-06-18"),
  // ── IRs ──
  tone("4×12 Greenback — 57+121 Blend", "ir_foundry", "ir", "cab", "1971 basketweave with G12M-25s. SM57 + R-121 blended at the cone/cap seam. The rock IR.", ["#1e4f8f", "#080c12"], 55214, 5310, ["Marshall", "Celestion"], ["4x12", "greenback", "blend"], ["standard"], "2025-12-01"),
  tone("2×12 Alnico Blue — Vintage 30 Mix", "ir_foundry", "ir", "cab", "Open-back 2×12, Blue + V30 mix. Chime with body — made for AC30-style captures.", ["#2a8f7d", "#081210"], 31780, 2670, ["Vox", "Celestion"], ["2x12", "alnico", "open-back"], ["standard"], "2026-02-14"),
  tone("Mesa OS 4×12 — V30 Tight", "cab_army", "ir", "cab", "Oversized Recto cab, quad V30s, 57 slightly off-axis. Tight low end for high gain.", ["#7d2a3b", "#0f080a"], 42350, 3240, ["Mesa/Boogie"], ["4x12", "v30", "metal"], ["standard"], "2026-01-09"),
  tone("1×12 Deluxe — Royer Room", "studio_irs", "ir", "cab", "Oxford 12K5-6 in a '65 Deluxe combo, R-121 plus a touch of room. Vintage air.", ["#8f6d3a", "#110e08"], 19240, 1580, ["Fender", "Oxford"], ["1x12", "vintage", "room"], ["standard"], "2026-05-22"),
  tone("4×10 Bassman Tweed Stack", "vintage_vault", "ir", "cab", "All four Jensen P10Rs blended. Tweed spank and grind, captured with vintage ribbons.", ["#a8842f", "#131008"], 12490, 1050, ["Fender", "Jensen"], ["4x10", "tweed", "jensen"], ["standard"], "2026-06-30"),
  tone("2×12 Lone Star — Fat & Wide", "cab_army", "ir", "cab", "Mesa Lone Star 2×12, MC90s, stereo-wide dual-mic. Big fat cleans and leads.", ["#2f8f6d", "#08120e"], 8420, 720, ["Mesa/Boogie"], ["2x12", "wide", "clean"], ["standard"], "2026-07-27"),
  tone("112 Blue Alnico — Close 57", "studio_irs", "ir", "cab", "Single Celestion Blue in a Deluxe-style cab. The chime cap, close-miked and bright.", ["#3a5f9f", "#080b12"], 15110, 1310, ["Celestion"], ["1x12", "alnico", "bright"], ["standard"], "2026-07-19"),
  tone("4×12 Uber — T75/V30 X-Pattern", "ir_foundry", "ir", "cab", "Bogner Uberkab X-pattern quad. Scooped T75 sizzle plus V30 mids in one IR.", ["#5f3a9f", "#0b0812"], 22870, 1740, ["Bogner", "Celestion"], ["4x12", "x-pattern", "modern"], ["standard"], "2026-04-03"),
  tone("Pine 1×10 — Lo-Fi Character", "studio_irs", "ir", "cab", "Vintage pine 1×10 with a worn ceramic speaker. Boxy on purpose — instant character.", ["#6d4f2f", "#0e0b08"], 4310, 386, ["Supro"], ["1x10", "lofi", "character"], ["standard"], "2026-08-02"),
];

/** Deterministic trending score: recent-ish downloads bias, stable per tone. */
const trendScore = (t: ProtoTone): number =>
  t.downloadsCount * (0.6 + ((t.id * 37) % 100) / 100) +
  Date.parse(t.createdAt) / 3.6e7;

/* ───────────────────────── query (fake search API) ───────────────────────── */

export function searchCatalog(q: ProtoQuery, favorites: Set<number>): ProtoTone[] {
  const needle = q.query.trim().toLowerCase();
  let rows = CATALOG.filter((t) =>
    q.kind === "irs" ? t.format === "ir" : t.format === "nam",
  );
  if (q.favoritesOnly) rows = rows.filter((t) => favorites.has(t.id));
  if (q.gear) rows = rows.filter((t) => t.gear === q.gear);
  if (q.size) rows = rows.filter((t) => (t.sizes as string[]).includes(q.size!));
  if (needle)
    rows = rows.filter((t) =>
      [t.title, t.description, t.creator.username, ...t.makes, ...t.tags]
        .join(" ")
        .toLowerCase()
        .includes(needle),
    );
  const sorted = [...rows];
  if (q.sort === "newest")
    sorted.sort((a, b) => Date.parse(b.createdAt) - Date.parse(a.createdAt));
  else if (q.sort === "downloads")
    sorted.sort((a, b) => b.downloadsCount - a.downloadsCount);
  else sorted.sort((a, b) => trendScore(b) - trendScore(a));
  return sorted;
}

/** Fake network latency so variants must show a loading state. */
export function fetchCatalog(
  q: ProtoQuery,
  favorites: Set<number>,
): Promise<ProtoTone[]> {
  return new Promise((resolve) =>
    setTimeout(() => resolve(searchCatalog(q, favorites)), 220),
  );
}

/* ─────────────────────── shared prototype state ─────────────────────── */

export interface DownloadState {
  /** 0..1 while downloading; 1 + path set = done. */
  progress: number;
  done: boolean;
}

interface ProtoState {
  favorites: Set<number>;
  toggleFavorite(toneId: number): void;
  /** modelId → download state (simulated). */
  downloads: Record<number, DownloadState>;
  /** The model currently loaded into the amp/cab block, if any. */
  loaded: { toneId: number; modelId: number; name: string; kind: "nam" | "wav" } | null;
  /** Simulate download → auto-load on completion. */
  downloadAndLoad(tone: ProtoTone, model: ProtoModel): void;
}

export const useProtoStore = create<ProtoState>((set, get) => ({
  favorites: new Set([CATALOG[1].id, CATALOG[13].id, CATALOG[20].id]),
  toggleFavorite: (toneId) =>
    set((st) => {
      const next = new Set(st.favorites);
      if (next.has(toneId)) next.delete(toneId);
      else next.add(toneId);
      return { favorites: next };
    }),
  downloads: {},
  loaded: null,
  downloadAndLoad: (t, model) => {
    if (get().downloads[model.id] && !get().downloads[model.id].done) return;
    set((st) => ({
      downloads: { ...st.downloads, [model.id]: { progress: 0, done: false } },
    }));
    const tick = () => {
      const cur = get().downloads[model.id];
      if (!cur || cur.done) return;
      const progress = Math.min(1, cur.progress + 0.13 + Math.random() * 0.09);
      if (progress >= 1) {
        set((st) => ({
          downloads: { ...st.downloads, [model.id]: { progress: 1, done: true } },
          loaded: {
            toneId: t.id,
            modelId: model.id,
            name: model.name,
            kind: model.fileKind,
          },
        }));
      } else {
        set((st) => ({
          downloads: { ...st.downloads, [model.id]: { progress, done: false } },
        }));
        setTimeout(tick, 120 + Math.random() * 160);
      }
    };
    setTimeout(tick, 160);
  },
}));

/* ─────────────────────────── tiny format helpers ─────────────────────────── */

export const formatCount = (n: number): string =>
  n >= 1000 ? `${(n / 1000).toFixed(n >= 10000 ? 0 : 1)}k` : String(n);

export const GEAR_LABELS: Record<string, string> = {
  amp: "Amp",
  "amp-cab": "Full rig",
  pedal: "Pedal",
  cab: "Cab",
};

export const SORT_LABELS: Record<ProtoSort, string> = {
  trending: "Trending",
  newest: "Newest",
  downloads: "Most downloaded",
};
