/**
 * Block glyphs — the React port of the native `src/ui/BlockIcons.h` (deleted with
 * `src/ui/*`, geometry preserved here verbatim).
 *
 * This is the SINGLE source of block iconography: board cards, the [+] picker,
 * the param-dock header and every menu row all render these paths (`BlockIcon`
 * in ./icons.tsx is a thin size/stroke preset over `BlockGlyph`). Do not add a
 * second, simplified glyph set — the same block must look identical everywhere.
 *
 * Every JUCE path was authored inside a 0..1 unit box and *stroked* (never
 * filled), which maps 1:1 onto `viewBox="0 0 1 1"` + `fill="none"`. Stroke width
 * is expressed in device pixels and divided by `size` so a glyph keeps a
 * constant 2px stroke at any render size (the same thing `drawUnitPath` did with
 * a `PathStrokeType` on a scaled path).
 */
import type { ReactNode } from "react";
import type { BlockId } from "../bridge/types";

export interface GlyphProps {
  /** Rendered box in CSS px. */
  size?: number;
  /** Stroke weight in CSS px (kept visually constant across sizes). */
  strokeWidth?: number;
  className?: string;
}

function Glyph({
  size = 28,
  strokeWidth = 2,
  className,
  children,
}: GlyphProps & { children: ReactNode }) {
  return (
    <svg
      width={size}
      height={size}
      viewBox="0 0 1 1"
      fill="none"
      stroke="currentColor"
      strokeWidth={strokeWidth / size}
      strokeLinecap="round"
      strokeLinejoin="round"
      aria-hidden="true"
      focusable="false"
      className={className}
    >
      {children}
    </svg>
  );
}

/* ─────────────────────────────── block glyphs ──────────────────────────── */

/** Two tall bars with a gap and a threshold tick between them. */
export const GateGlyph = (p: GlyphProps) => (
  <Glyph {...p}>
    <path d="M0.2 0.1V0.9M0.8 0.1V0.9M0.4 0.5H0.6" />
  </Glyph>
);

/** Two arrows converging on a horizontal line. */
export const CompGlyph = (p: GlyphProps) => (
  <Glyph {...p}>
    <path d="M0.1 0.5H0.9M0.5 0.08V0.34M0.38 0.22L0.5 0.36L0.62 0.22M0.5 0.92V0.66M0.38 0.78L0.5 0.64L0.62 0.78" />
  </Glyph>
);

/** A hard-clipped wave. */
export const DriveGlyph = (p: GlyphProps) => (
  <Glyph {...p}>
    <path d="M0.06 0.52L0.22 0.16L0.4 0.16L0.6 0.84L0.78 0.84L0.94 0.48" />
  </Glyph>
);

/** Amp head: body, carry handle and two control dots. */
export const AmpGlyph = (p: GlyphProps) => (
  <Glyph {...p}>
    {/* addRoundedRectangle (0.10, 0.32, 0.80, 0.48, corner 0.06) */}
    <path d="M0.16 0.32H0.84A0.06 0.06 0 0 1 0.9 0.38V0.74A0.06 0.06 0 0 1 0.84 0.8H0.16A0.06 0.06 0 0 1 0.1 0.74V0.38A0.06 0.06 0 0 1 0.16 0.32Z" />
    <path d="M0.38 0.32Q0.5 0.08 0.62 0.32" />
    <path d="M0.18 0.62H0.82" />
    <circle cx="0.285" cy="0.455" r="0.035" />
    <circle cx="0.445" cy="0.455" r="0.035" />
  </Glyph>
);

/** Cab: cabinet square with a speaker cone and dust cap. */
export const CabGlyph = (p: GlyphProps) => (
  <Glyph {...p}>
    {/* addRoundedRectangle (0.10, 0.10, 0.80, 0.80, corner 0.08) */}
    <path d="M0.18 0.1H0.82A0.08 0.08 0 0 1 0.9 0.18V0.82A0.08 0.08 0 0 1 0.82 0.9H0.18A0.08 0.08 0 0 1 0.1 0.82V0.18A0.08 0.08 0 0 1 0.18 0.1Z" />
    <circle cx="0.5" cy="0.5" r="0.26" />
    <circle cx="0.5" cy="0.5" r="0.06" />
  </Glyph>
);

/** Three faders at different positions. */
export const EqGlyph = (p: GlyphProps) => (
  <Glyph {...p}>
    <path d="M0.22 0.12V0.88M0.5 0.12V0.88M0.78 0.12V0.88M0.12 0.34H0.32M0.4 0.62H0.6M0.68 0.26H0.88" />
  </Glyph>
);

/** Two full sine cycles. */
export const ModGlyph = (p: GlyphProps) => (
  <Glyph {...p}>
    <path d="M0.06 0.5Q0.17 0.06 0.28 0.5Q0.39 0.94 0.5 0.5Q0.61 0.06 0.72 0.5Q0.83 0.94 0.94 0.5" />
  </Glyph>
);

/** Three diminishing taps. */
export const DelayGlyph = (p: GlyphProps) => (
  <Glyph {...p}>
    <path d="M0.18 0.12V0.88M0.5 0.28V0.72M0.82 0.41V0.59" />
  </Glyph>
);

/**
 * Three arcs radiating to the right of a source point.
 *
 * `addCentredArc(0.14, 0.5, r, r, 0, 0.22π, 0.78π)` with JUCE's 12-o'clock-zero,
 * clockwise angles → SVG sweep-flag 1 between (cx + r·sin a, cy − r·cos a).
 * Radii are 0.24 / 0.44 / 0.62 rather than the native 0.26 / 0.48 / 0.70 so the
 * glyph stays inside the unit box (SVG clips its viewBox; JUCE did not).
 */
export const ReverbGlyph = (p: GlyphProps) => (
  <Glyph {...p}>
    <path d="M0.293 0.3151A0.24 0.24 0 0 1 0.293 0.6849" />
    <path d="M0.4205 0.161A0.44 0.44 0 0 1 0.4205 0.839" />
    <path d="M0.5352 0.0223A0.62 0.62 0 0 1 0.5352 0.9777" />
    <circle cx="0.14" cy="0.5" r="0.04" />
  </Glyph>
);

/**
 * External-AU slot: a module body with pins on both sides.
 *
 * No native ancestor — the fx slots postdate BlockIcons.h. Drawn in the same unit
 * box and stroke-only convention as the ported glyphs, and deliberately generic:
 * one glyph serves all three slots, because what is in the slot is named in text,
 * not iconography.
 */
export const FxGlyph = (p: GlyphProps) => (
  <Glyph {...p}>
    {/* Rounded rect (0.22, 0.22, 0.56, 0.56, corner 0.06). */}
    <path d="M0.28 0.22H0.72A0.06 0.06 0 0 1 0.78 0.28V0.72A0.06 0.06 0 0 1 0.72 0.78H0.28A0.06 0.06 0 0 1 0.22 0.72V0.28A0.06 0.06 0 0 1 0.28 0.22Z" />
    <path d="M0.22 0.36H0.08M0.22 0.64H0.08" />
    <path d="M0.78 0.36H0.92M0.78 0.64H0.92" />
  </Glyph>
);

export const BLOCK_GLYPH: Record<BlockId, (p: GlyphProps) => ReactNode> = {
  gate: GateGlyph,
  comp: CompGlyph,
  drive: DriveGlyph,
  amp: AmpGlyph,
  cab: CabGlyph,
  eq: EqGlyph,
  mod: ModGlyph,
  delay: DelayGlyph,
  reverb: ReverbGlyph,
  fx1: FxGlyph,
  fx2: FxGlyph,
  fx3: FxGlyph,
};

/** Dispatch, mirroring `icons::makeIcon (chain::BlockId)`. */
export function BlockGlyph({ block, ...rest }: GlyphProps & { block: BlockId }) {
  const Component = BLOCK_GLYPH[block];
  return <Component {...rest} />;
}

/* ────────────────────────────── endpoint glyphs ────────────────────────── */

/** IN: an instrument jack (sleeve circle + cable). */
export const InputGlyph = (p: GlyphProps) => (
  <Glyph {...p}>
    <circle cx="0.32" cy="0.5" r="0.22" />
    <path d="M0.54 0.5H0.92" />
  </Glyph>
);

/** OUT: a speaker cone. */
export const OutputGlyph = (p: GlyphProps) => (
  <Glyph {...p}>
    <path d="M0.14 0.36H0.34L0.58 0.14V0.86L0.34 0.64H0.14Z" />
    <path d="M0.7838 0.3162A0.26 0.26 0 0 1 0.7838 0.6838" />
  </Glyph>
);

/** The classic IEC power symbol: broken ring + stem. */
export const PowerGlyph = (p: GlyphProps) => (
  <Glyph {...p}>
    <path d="M0.704 0.2934A0.32 0.32 0 1 1 0.296 0.2934" />
    <path d="M0.5 0.1V0.46" />
  </Glyph>
);
