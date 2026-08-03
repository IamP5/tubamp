# Vercel Geist design system — extraction for tubamp (React/TS rebuild, dark-only)

Status: research doc for the React+TypeScript UI rebuild. Fixed 1120×700 WebView,
self-contained bundle (no CDN, no network at runtime). Dark mode ONLY — no light-theme
branch is needed anywhere in this doc or the resulting CSS.

Sources: [Geist intro](https://vercel.com/geist/introduction),
[Geist colors](https://vercel.com/geist/colors) (scale semantics; the live page does not
expose raw hex — see §0 for how the concrete hex values below were derived),
[vercel/geist-font](https://github.com/vercel/geist-font) (font vendoring),
[geist npm package](https://www.npmjs.com/package/geist) (inspected via `npm pack`, see
§5), astro-shop-ten.vercel.app (dark storefront reference for spacing/shadow feel).

## 0. How the numbers below were sourced

`vercel.com/geist/colors` documents the **scale semantics** (10 steps, 100→1000, each
step's role) but renders swatches as canvas/JS, not literal CSS text. The concrete hex
values in this doc are computed from the published Geist HSL/OKLCH source values (same
numbers used by `ephraimduncan/geist-colors`, a scrape of the Geist token set) converted
to sRGB hex via standard HSL→RGB. Treat sub-1% lightness differences as noise — these are
correct to the pixel for a WebView, not necessarily bit-identical to Vercel's internal
OKLCH-P3 pipeline (which only matters on wide-gamut displays; skip P3 for this project).

Geist gray-scale step semantics (same for every color family):

| step | role |
|---|---|
| 100 | default background (tint) |
| 200 | hover background |
| 300 | active background |
| 400 | default border |
| 500 | hover border |
| 600 | active border / low-contrast solid |
| 700 | solid fill, high contrast |
| 800 | solid fill hover |
| 900 | secondary text / icon |
| 1000 | primary text / icon |

## 1. Color tokens

### 1.1 Geist gray scale (dark) — raw reference

```css
--geist-gray-100: #1a1a1a;
--geist-gray-200: #1f1f1f;
--geist-gray-300: #292929;
--geist-gray-400: #2e2e2e;
--geist-gray-500: #454545;
--geist-gray-600: #878787;
--geist-gray-700: #8f8f8f;
--geist-gray-800: #7d7d7d;
--geist-gray-900: #a1a1a1;
--geist-gray-1000: #ededed;

/* true app background is darker than gray-100 — Geist's own page bg */
--geist-bg-100: #0a0a0a;   /* app background */
--geist-bg-200: #000000;   /* pure black, rarely used (e.g. video letterbox) */
```

### 1.2 Geist blue accent scale (dark)

```css
--geist-blue-100: #0f1c2e;
--geist-blue-200: #10233d;
--geist-blue-300: #0f2f57;
--geist-blue-400: #0d3868;
--geist-blue-500: #0a4380;
--geist-blue-600: #0090ff;  /* low-contrast solid / hover border */
--geist-blue-700: #0072f5;  /* primary solid — links, primary buttons */
--geist-blue-800: #0062d1;  /* solid hover */
--geist-blue-900: #52a8ff;  /* secondary text/icon on dark */
--geist-blue-1000: #ebf6ff;
```

### 1.3 Geist semantic scales (dark) — success/error/warning

```css
/* red — error */
--geist-red-100: #2a1314; --geist-red-200: #3c1618; --geist-red-300: #561a1e;
--geist-red-400: #671e21; --geist-red-500: #832126; --geist-red-600: #e5484d;
--geist-red-700: #e5484d; --geist-red-800: #d93036; --geist-red-900: #ff6166;
--geist-red-1000: #feecee;

/* amber — warning */
--geist-amber-100: #291800; --geist-amber-200: #331b00; --geist-amber-300: #4c2a00;
--geist-amber-400: #573300; --geist-amber-500: #6b4105; --geist-amber-600: #e79d13;
--geist-amber-700: #ffb224; --geist-amber-800: #ff990a; --geist-amber-900: #ff990a;
--geist-amber-1000: #fef3dc;

/* green — success */
--geist-green-100: #0b2212; --geist-green-200: #0f2e18; --geist-green-300: #12361b;
--geist-green-400: #0c451b; --geist-green-500: #126426; --geist-green-600: #1a9338;
--geist-green-700: #45a557; --geist-green-800: #398e4a; --geist-green-900: #62c073;
--geist-green-1000: #e5fbea;

/* teal / purple / pink also exist in Geist; unused directly here since tubamp already
   has its own per-block hues (§1.5), but kept for reference / future semantic needs
   (e.g. a "processing" or "info-alt" state) */
--geist-teal-700: #12a594;   --geist-purple-700: #8e4ec6;  --geist-pink-700: #ea3e83;
```

### 1.4 Semantic token mapping (what tubamp components actually consume)

```css
:root {
  /* backgrounds */
  --bg-app:        #0a0a0a;              /* geist-bg-100 — window background */
  --bg-surface:    #12151b;              /* tubamp panel, one step up (custom —
                                             between geist-gray-100 and 200, tuned
                                             slightly blue-black to avoid muddy neutral
                                             gray next to the colorful block accents) */
  --bg-elevated:   #161a21;              /* cards, popovers, param panel */
  --bg-elevated-hi:#1c222b;              /* hovered/selected surface, e.g. selected tile */
  --bg-overlay:    rgba(10, 10, 10, 0.72); /* modal/menu backdrop scrim */

  /* borders */
  --border:        #262e39;              /* default 1px stroke — analogous to geist-gray-400 role */
  --border-hover:  #333d4b;              /* hover stroke — geist-gray-500 role */
  --border-active: #454f5e;              /* active/pressed stroke */

  /* text */
  --text-primary:   #e6eaf0;             /* ~geist-gray-1000 role, tinted cool */
  --text-secondary: #8b95a6;             /* ~geist-gray-900 role */
  --text-tertiary:  #5a6473;             /* ~geist-gray-600/700 role, micro-copy */
  --text-disabled:  rgba(230, 234, 240, 0.35);

  /* accent (brand + interactive default) */
  --accent:         #ff7a1a;             /* tubamp brand amber-orange (not Geist blue —
                                             tubamp already committed to this as the
                                             wordmark/preset-pill color; keep it, it reads
                                             as "warm tube glow" against Geist's cool grays) */
  --accent-hover:   #ff8f3d;
  --accent-active:  #e56a10;
  --accent-fill-8:  color-mix(in srgb, var(--accent) 8%, transparent);
  --accent-fill-16: color-mix(in srgb, var(--accent) 16%, transparent);

  /* Geist blue kept as the *interactive/link/focus* accent distinct from tubamp's
     brand amber — used for generic UI affordances (checkbox check, selected menu row,
     primary CTA if ever needed) so it doesn't collide with the 9 block hues */
  --blue:           #0072f5;             /* geist-blue-700 */
  --blue-hover:     #0090ff;             /* geist-blue-600 */

  /* status */
  --success:  #45a557;                   /* geist-green-700 */
  --success-fill: color-mix(in srgb, var(--success) 12%, transparent);
  --warning:  #ffb224;                   /* geist-amber-700 */
  --warning-fill: color-mix(in srgb, var(--warning) 12%, transparent);
  --error:    #e5484d;                   /* geist-red-600/700 */
  --error-fill: color-mix(in srgb, var(--error) 12%, transparent);

  /* focus ring — Geist uses a 2px solid ring in blue-700 with a 1-2px offset gap
     (not a glow/box-shadow blur) */
  --focus-ring: 0 0 0 2px var(--bg-app), 0 0 0 4px var(--blue-hover);

  /* radii */
  --radius-sm: 6px;   /* buttons, inputs, small chips */
  --radius-md: 8px;   /* cards, menus */
  --radius-lg: 12px;  /* block tiles, param panel, modal */

  /* shadow (see §3.3) */
  --shadow-menu: 0 4px 16px rgba(0,0,0,0.5), 0 1px 2px rgba(0,0,0,0.4), inset 0 0 0 1px var(--border);
  --shadow-drag: 0 12px 32px rgba(0,0,0,0.6), 0 2px 8px rgba(0,0,0,0.4);
}
```

### 1.5 tubamp per-block accents — verified on Geist dark backgrounds

WCAG contrast ratios computed against `--bg-app` (#0A0A0A) and `--bg-elevated` (#12151B/
#161A21 similar enough to bucket together). **All 9 existing block colors pass AA (≥4.5:1,
most far exceed AAA 7:1) with zero adjustment needed** — they were already well-chosen
against a near-black ground:

| block   | hex       | vs `--bg-app` | vs `--bg-surface` | verdict |
|---------|-----------|---------------|--------------------|---------|
| gate    | `#A78BFA` | 7.27:1        | 6.72:1             | keep as-is |
| comp    | `#FBBF24` | 11.86:1       | 10.95:1            | keep as-is |
| drive   | `#F97316` | 7.06:1        | 6.52:1             | keep as-is |
| amp     | `#F43F5E` | 5.39:1        | 4.98:1             | keep as-is (lowest margin — do not darken further) |
| cab     | `#2DD4BF` | 10.64:1       | 9.82:1             | keep as-is |
| eq      | `#60A5FA` | 7.79:1        | 7.19:1             | keep as-is |
| mod     | `#4ADE80` | 11.36:1       | 10.49:1            | keep as-is |
| delay   | `#818CF8` | 6.64:1        | 6.13:1             | keep as-is |
| reverb  | `#22D3EE` | 10.96:1       | 10.12:1            | keep as-is |

Only `amp` (#F43F5E, 5.39:1) has meaningfully less headroom than the rest — fine for text/
icons (AA large-text and AA normal-text both pass), but avoid using it for sub-12px
micro-copy at low opacity; keep amp label text ≥13px or full-opacity.

Per-block tint/glow variants — define once per block via `color-mix`, generated from the
single base hex (no separate hardcoded 8%/16% hexes to keep in sync):

```css
:root {
  --block-gate:    #A78BFA;
  --block-comp:    #FBBF24;
  --block-drive:   #F97316;
  --block-amp:     #F43F5E;
  --block-cab:     #2DD4BF;
  --block-eq:      #60A5FA;
  --block-mod:     #4ADE80;
  --block-delay:   #818CF8;
  --block-reverb:  #22D3EE;
}

/* usage pattern, e.g. for gate — replicate per block or generate via a small
   TS helper: tint(block, pct) => `color-mix(in srgb, var(--block-${id}) ${pct}%, transparent)` */
.block-tile[data-block="gate"] {
  --block-fill-8:  color-mix(in srgb, var(--block-gate) 8%,  transparent); /* idle tint wash */
  --block-fill-16: color-mix(in srgb, var(--block-gate) 16%, transparent); /* selected/hover fill */
  --block-fill-25: color-mix(in srgb, var(--block-gate) 25%, transparent); /* active-press fill */
  --block-glow:    color-mix(in srgb, var(--block-gate) 35%, transparent); /* outer glow ring, blurred */
}
```

TS helper (avoids 9× duplicated CSS blocks):

```ts
// src/ui/theme/blockAccent.ts
export const BLOCK_ACCENT: Record<BlockId, string> = {
  gate: "#A78BFA", comp: "#FBBF24", drive: "#F97316", amp: "#F43F5E",
  cab: "#2DD4BF", eq: "#60A5FA", mod: "#4ADE80", delay: "#818CF8", reverb: "#22D3EE",
};
export function blockCssVars(id: BlockId): React.CSSProperties {
  const c = BLOCK_ACCENT[id];
  return {
    "--block-accent": c,
    "--block-fill-8": `color-mix(in srgb, ${c} 8%, transparent)`,
    "--block-fill-16": `color-mix(in srgb, ${c} 16%, transparent)`,
    "--block-fill-25": `color-mix(in srgb, ${c} 25%, transparent)`,
    "--block-glow": `color-mix(in srgb, ${c} 35%, transparent)`,
  } as React.CSSProperties;
}
```

Meter gradient (already spec'd in `docs/UI-REDESIGN.md`, carried forward unchanged):
`#34D399 → #FBBF24 (−12 dB) → #F87171 (−3 dB)`.

## 2. Typography

Two families only: **Geist Sans** (UI text) and **Geist Mono** (numeric readouts, kbd
hints, model filenames). No third font.

```css
:root {
  --font-sans: "Geist Sans", -apple-system, BlinkMacSystemFont, "Segoe UI", sans-serif;
  --font-mono: "Geist Mono", ui-monospace, "SF Mono", Menlo, monospace;
}
```

| element | family | size | weight | tracking | line-height | notes |
|---|---|---|---|---|---|---|
| app wordmark ("tubamp") | Sans | 15px | 600 (SemiBold) | 0 | 1 | next to the power/brand dot in HeaderBar |
| model name (loaded .nam) | Sans | 18px | 700 (Bold) | 0 | 1.2 | footer/header model readout |
| section title | Sans | 13px | 600 | 0.01em | 1.3 | e.g. "SIGNAL CHAIN", param panel group headers |
| block tile label | Sans | 10px | 600 | 0.06em, uppercase | 1 | `shortName`, under each chain tile |
| knob label | Sans | 11px | 500 | 0.04em, uppercase | 1.2 | above/below each rotary |
| value readout (dB, Hz, ms, %) | **Mono** | 11px | 500 | 0 | 1 | `font-feature-settings: "tnum" 1; font-variant-numeric: tabular-nums;` |
| body / menu item | Sans | 13px | 400 | 0 | 1.4 | default UI text |
| button label | Sans | 13px | 500 | 0.01em | 1 | |
| micro-copy (hints, timestamps, footer meta) | Sans | 11px | 400 | 0 | 1.3 | `--text-tertiary` |
| kbd hint | Mono | 10px | 500 | 0 | 1 | inside `<kbd>` chip, see §4 |

Tabular numerics for every live-updating readout (dB meters, param values, latency ms) is
mandatory — without it, digit width jitter causes visible layout shimmer at 60fps meter
refresh:

```css
.value-readout {
  font-family: var(--font-mono);
  font-feature-settings: "tnum" 1, "zero" 1; /* tnum = tabular figures, zero = slashed 0 */
  font-variant-numeric: tabular-nums;
}
```

## 3. Shape & depth

### 3.1 Radii

```css
--radius-sm: 6px;   /* inputs, buttons, chips, kbd */
--radius-md: 8px;   /* menu, tooltip, toast, segmented control */
--radius-lg: 12px;  /* block tile, param panel card, modal */
```

### 3.2 Borders over shadows (Geist's core depth trick)

Geist prefers a crisp 1px inset border to a soft shadow for *resting* surfaces — shadows
are reserved for elements that visually float above the layout (menus, dragged tiles,
modals, toasts). Implementation pattern: pair a background fill with `box-shadow: inset 0
0 0 1px var(--border)` rather than a real `border` when you also need a shadow on the same
element (avoids double-blur artifacts at the corner radius):

```css
.card {
  background: var(--bg-elevated);
  border-radius: var(--radius-lg);
  box-shadow: inset 0 0 0 1px var(--border);
}
.card:hover { box-shadow: inset 0 0 0 1px var(--border-hover); }
```

### 3.3 Shadow recipes for floating elements

```css
/* dropdown / popover / context menu */
--shadow-menu:
  0 0 0 1px var(--border),
  0 4px 16px rgba(0, 0, 0, 0.5),
  0 1px 2px rgba(0, 0, 0, 0.4);

/* dragged block tile (SignalChainView reorder) */
--shadow-drag:
  0 0 0 1px var(--border-hover),
  0 12px 32px rgba(0, 0, 0, 0.6),
  0 2px 8px rgba(0, 0, 0, 0.4);

/* modal / dialog */
--shadow-modal:
  0 0 0 1px var(--border),
  0 24px 64px rgba(0, 0, 0, 0.7);

/* toast */
--shadow-toast:
  0 0 0 1px var(--border),
  0 8px 24px rgba(0, 0, 0, 0.5);
```

### 3.4 Glow / gradient washes (subtle only — never a decorative hero gradient)

```css
/* app background: barely-there radial lightening toward top-center, per UI-REDESIGN.md */
.app-root {
  background:
    radial-gradient(120% 60% at 50% -10%, rgba(255,255,255,0.035), transparent 60%),
    var(--bg-app);
}

/* selected block tile outer glow — accent at low alpha, generous blur, no spread */
.block-tile[data-selected="true"] {
  box-shadow:
    0 0 0 2px var(--block-accent),
    0 0 16px 2px var(--block-glow); /* --block-glow = color-mix 35% from §1.5 */
}

/* focus ring glow variant for inputs (paired with --focus-ring outline) */
.input:focus-visible {
  box-shadow: var(--focus-ring);
}
```

## 4. Component patterns

All components: 1px border via `box-shadow: inset 0 0 0 1px`, `--radius-sm`/`md` per
table below, `transition: background-color 120ms ease, border-color 120ms ease, color
120ms ease` (no transition on layout-affecting props — avoids jank on the audio-thread-
adjacent 60fps UI).

### Buttons (32px height default)

| variant | rest | hover | active | disabled |
|---|---|---|---|---|
| **default** | bg `--bg-elevated`, border `--border`, text `--text-primary` | bg `--bg-elevated-hi`, border `--border-hover` | bg `--bg-elevated-hi`, border `--border-active`, scale 0.98 (transform, not layout) | 40% opacity, no pointer events |
| **primary** | bg `--accent`, text `#0a0a0a` (dark-on-amber for contrast), no border | bg `--accent-hover` | bg `--accent-active` | 40% opacity |
| **ghost** | transparent, text `--text-secondary` | bg `--accent-fill-8`, text `--text-primary` | bg `--accent-fill-16` | 40% opacity |

Height 32px, padding `0 12px`, `--radius-sm`, `font: 13px/1 var(--font-sans) 500`, gap 6px
for icon+label.

### Segmented control (A/B preset compare)

Single track `bg-surface`, `--radius-sm`, 1px inset border. Two (or N) equal-width
segments, 28px tall, no gaps between segments. Active segment: `bg-elevated-hi` pill
inset by 2px with its own `--radius-sm - 2px` and a 1px border in `--border-hover`,
animated via `transform: translateX()` (not `left`) over 150ms ease for the sliding pill,
`will-change: transform` while dragging is possible but not needed for a 2-state A/B.

### Menus / dropdowns

`bg-elevated`, `--radius-md`, `--shadow-menu`, padding 4px, min-width 180px. Items: 30px
tall, `--radius-sm`, padding `0 8px`, hover `bg accent-fill-8` (or `--bg-elevated-hi` for
non-accent items), 13px sans. Section separators: 1px `--border`, `margin: 4px -4px`.
Destructive items (e.g. "Remove from chain"): text `--error`, hover fill
`--error-fill`. Enter/exit: 100ms fade + 4px translateY, transform-origin near the
trigger.

### Inputs (text fields, number fields)

32px height (or 28px compact, e.g. inline rename), `bg-surface`, 1px inset border
`--border`, `--radius-sm`, padding `0 10px`, 13px sans text, placeholder
`--text-tertiary`. Focus: border → `--blue-hover`, plus `--focus-ring` box-shadow.
Invalid: border/ring swapped to `--error`.

### Tooltips

`bg-elevated-hi`, `--radius-sm`, 1px border, `--shadow-menu` (lighter — drop the 16px
blur layer, keep just the 1px ring + a 4px/0.4 alpha shadow), padding `4px 8px`, 11px
sans, `--text-primary`. 4px triangle caret via `clip-path` matching bg. Delay: 400ms show,
0ms hide. Never on touch (n/a here — WebView is mouse/trackpad only).

### Modal / dialog

Centered, `bg-elevated`, `--radius-lg`, `--shadow-modal`, backdrop `--bg-overlay` with
`backdrop-filter: blur(2px)` (cheap enough at 1120×700; skip if profiling shows cost).
Header 48px (13px 600 title + close `×` ghost button), body padding 20px, footer 56px
right-aligned button row with 8px gap. Enter: 150ms fade + scale from 0.97, ease-out.

### Toast

Bottom-right stack, `bg-elevated-hi`, `--radius-md`, `--shadow-toast`, 1px border,
max-width 320px, padding `10px 12px`, icon (16px, status-colored: success/error/warning)
+ 13px message. Auto-dismiss 4s with a 2px bottom progress bar in the status color
draining left→right. Enter: slide up 8px + fade 150ms; exit: fade 100ms.

### Tabs (if needed for settings/T3K browser panes)

Underline style, not pill: 32px row, 13px 500 text `--text-secondary`, active
`--text-primary` + 2px bottom bar in `--accent`, animated `transform: scaleX` from the
previously active tab's position (150ms ease) rather than crossfade.

### Kbd hints

`<kbd>`: `bg-surface`, 1px border `--border-hover`, `--radius-sm` (use `4px` here
specifically — kbd chips read better slightly tighter than the sm token), padding
`1px 5px`, `font: 10px/1 var(--font-mono) 500`, `--text-secondary`, subtle bottom-edge
"key" shadow: `box-shadow: inset 0 0 0 1px var(--border-hover), 0 1px 0 var(--border)`.

### Empty states

Centered in the container, icon 32px `--text-tertiary` at 40% opacity, 13px message
`--text-secondary`, optional 12px sub-line `--text-tertiary`, optional ghost-button CTA
below with 12px gap. E.g. empty signal chain lane before first block is added, or T3K
browser before auth.

### Loading

**Skeleton**: `bg-surface` block, `--radius` matching the real content, animated with a
diagonal shimmer sweep — `background: linear-gradient(100deg, transparent 30%,
rgba(255,255,255,0.06) 50%, transparent 70%)` on a `background-size: 200% 100%`,
`animation: shimmer 1.6s ease-in-out infinite`. **Spinner**: 16/20/24px, 2px stroke, SVG
circle with `stroke-dasharray` arc (not a full ring — Geist's spinner is a ~270° arc,
matching the rotary knob's own 270° language), `--text-secondary` color, rotate 700ms
linear infinite. Use spinner for sub-second/indeterminate ops (T3K auth round-trip,
model download); skeleton for known-shape content that's about to populate (preset list,
T3K browse grid).

### Scrollbars (thin dark custom — WebView, so this is real CSS, not host chrome)

```css
* {
  scrollbar-width: thin;             /* Firefox-style API, respected by Chromium WebViews */
  scrollbar-color: var(--border-hover) transparent;
}
*::-webkit-scrollbar { width: 8px; height: 8px; }
*::-webkit-scrollbar-track { background: transparent; }
*::-webkit-scrollbar-thumb {
  background: var(--border-hover);
  border-radius: 4px;
  border: 2px solid transparent;
  background-clip: padding-box;
}
*::-webkit-scrollbar-thumb:hover { background: var(--border-active); }
```

## 5. Font vendoring

**Source**: npm package `geist` (v1.7.2 as of this research; verified by `npm pack geist`
and inspecting the tarball directly — this is the same package Vercel ships for
next/font-less environments, maintained at
[vercel/geist-font](https://github.com/vercel/geist-font), OFL-1.1 licensed, bundling
permitted).

```
npm i geist
```

Relevant files inside `node_modules/geist/dist/fonts/`:

```
geist-sans/Geist-Regular.woff2
geist-sans/Geist-Medium.woff2
geist-sans/Geist-SemiBold.woff2
geist-sans/Geist-Bold.woff2
geist-mono/GeistMono-Regular.woff2
geist-mono/GeistMono-Medium.woff2
geist-mono/GeistMono-SemiBold.woff2
```

(Static-weight woff2 files exist per-weight — Regular/Medium/SemiBold/Bold cover every
weight used in §2's type table; skip Thin/Light/ExtraBold/Black/UltraBlack/Italic
variants and the two `-Variable.woff2` variable-font files entirely — this is a fixed
1120×700 plugin UI with a small, closed set of weights, so shipping the variable font
would only add dead weight. Also skip the `geist-pixel` family — not used.)

**Vendoring steps** (build-time, not a runtime dependency):

1. `npm i geist` (or `npm i -D geist` if only used at build time to copy files — plugin
   ships the static assets, not the package).
2. Copy the 7 files above into `src/ui/assets/fonts/` (or wherever the React build's
   static asset pipeline picks up local files for inlining/bundling).
3. Self-host via local `@font-face` — no Google Fonts / CDN `<link>`, per the no-network
   constraint:

```css
@font-face {
  font-family: "Geist Sans";
  src: url("./assets/fonts/geist-sans/Geist-Regular.woff2") format("woff2");
  font-weight: 400;
  font-style: normal;
  font-display: swap;
}
@font-face {
  font-family: "Geist Sans";
  src: url("./assets/fonts/geist-sans/Geist-Medium.woff2") format("woff2");
  font-weight: 500;
  font-style: normal;
  font-display: swap;
}
@font-face {
  font-family: "Geist Sans";
  src: url("./assets/fonts/geist-sans/Geist-SemiBold.woff2") format("woff2");
  font-weight: 600;
  font-style: normal;
  font-display: swap;
}
@font-face {
  font-family: "Geist Sans";
  src: url("./assets/fonts/geist-sans/Geist-Bold.woff2") format("woff2");
  font-weight: 700;
  font-style: normal;
  font-display: swap;
}
@font-face {
  font-family: "Geist Mono";
  src: url("./assets/fonts/geist-mono/GeistMono-Regular.woff2") format("woff2");
  font-weight: 400;
  font-style: normal;
  font-display: swap;
}
@font-face {
  font-family: "Geist Mono";
  src: url("./assets/fonts/geist-mono/GeistMono-Medium.woff2") format("woff2");
  font-weight: 500;
  font-style: normal;
  font-display: swap;
}
@font-face {
  font-family: "Geist Mono";
  src: url("./assets/fonts/geist-mono/GeistMono-SemiBold.woff2") format("woff2");
  font-weight: 600;
  font-style: normal;
  font-display: swap;
}
```

4. Since this is a fixed-size WebView (no arbitrary web page loading before the bundle is
   ready), `font-display: swap` mostly guards against a flash-of-fallback on cold start;
   consider `font-display: block` instead (~short block period, then swap) since the UI
   never scrolls past unrendered text — a brief invisible-text flash reads better than a
   fallback-font layout jump in a fixed 1120×700 frame with tight tile/label spacing.
5. Base stack applies the family with system fallbacks per §2's `--font-sans`/
   `--font-mono` vars, so a font-load failure degrades gracefully rather than breaking
   layout.
6. License: copy `node_modules/geist/LICENSE.txt` (OFL-1.1) into the repo's
   third-party-notices (e.g. `THIRD_PARTY_NOTICES.md`) alongside the JUCE/NAM ones
   already required for the AU build — bundling is explicitly permitted by OFL but
   attribution should still be retained per convention.

## 6. Quick-reference: full semantic token sheet

```css
:root {
  color-scheme: dark; /* only mode; skip light dql entirely */

  /* geist raw (kept for reference / building new semantic tokens later) */
  --geist-gray-100:#1a1a1a; --geist-gray-400:#2e2e2e; --geist-gray-900:#a1a1a1; --geist-gray-1000:#ededed;
  --geist-blue-700:#0072f5; --geist-blue-600:#0090ff;
  --geist-red-700:#e5484d; --geist-amber-700:#ffb224; --geist-green-700:#45a557;

  /* app */
  --bg-app:#0a0a0a; --bg-surface:#12151b; --bg-elevated:#161a21; --bg-elevated-hi:#1c222b;
  --bg-overlay:rgba(10,10,10,0.72);
  --border:#262e39; --border-hover:#333d4b; --border-active:#454f5e;
  --text-primary:#e6eaf0; --text-secondary:#8b95a6; --text-tertiary:#5a6473;
  --text-disabled:rgba(230,234,240,0.35);
  --accent:#ff7a1a; --accent-hover:#ff8f3d; --accent-active:#e56a10;
  --blue:#0072f5; --blue-hover:#0090ff;
  --success:#45a557; --warning:#ffb224; --error:#e5484d;
  --radius-sm:6px; --radius-md:8px; --radius-lg:12px;
  --font-sans:"Geist Sans",-apple-system,BlinkMacSystemFont,"Segoe UI",sans-serif;
  --font-mono:"Geist Mono",ui-monospace,"SF Mono",Menlo,monospace;

  /* block accents (unchanged from docs/UI-REDESIGN.md) */
  --block-gate:#A78BFA; --block-comp:#FBBF24; --block-drive:#F97316; --block-amp:#F43F5E;
  --block-cab:#2DD4BF; --block-eq:#60A5FA; --block-mod:#4ADE80; --block-delay:#818CF8;
  --block-reverb:#22D3EE;
}
```
