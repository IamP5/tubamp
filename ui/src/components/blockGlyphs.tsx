/**
 * Block glyphs: the single source of block iconography. Board cards, the add
 * picker, the editor header and every menu row render these (`BlockIcon` in
 * ./icons.tsx is a size/stroke preset over `BlockGlyph`). Do not add a second
 * glyph set: the same block must look identical everywhere.
 *
 * One family with the chrome icons: drawn on a 16px grid, stroked (never
 * filled), round caps and joins, `currentColor`, no per-block colour. The
 * stroke is given in CSS px and converted to viewBox units, so a glyph keeps
 * the same line weight at any size.
 */
import type { ReactNode } from "react";
import { blockRecord, kindOf, type BaseBlockId, type BlockId } from "../bridge/types";

export interface GlyphProps {
  /** Rendered box in CSS px (default 16). */
  size?: number;
  /** Stroke weight in CSS px (default 1.5), constant across sizes. */
  strokeWidth?: number;
  className?: string;
}

function Glyph({
  size = 16,
  strokeWidth = 1.5,
  className,
  children,
}: GlyphProps & { children: ReactNode }) {
  return (
    <svg
      width={size}
      height={size}
      viewBox="0 0 16 16"
      fill="none"
      stroke="currentColor"
      strokeWidth={(strokeWidth * 16) / size}
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

/** Two posts with the threshold between them. */
export const GateGlyph = (p: GlyphProps) => (
  <Glyph {...p}>
    <path d="M4 2.5v11M12 2.5v11M6.75 8h2.5" />
  </Glyph>
);

/** Two arrows pressing on a line. */
export const CompGlyph = (p: GlyphProps) => (
  <Glyph {...p}>
    <path d="M2.5 8h11M8 2v3.5M6 3.75l2 2 2-2M8 14v-3.5M6 12.25l2-2 2 2" />
  </Glyph>
);

/** A hard-clipped wave. */
export const DriveGlyph = (p: GlyphProps) => (
  <Glyph {...p}>
    <path d="M1.75 8.5L4.25 4H6.5l3 8h2.25l2.5-4.5" />
  </Glyph>
);

/** Amp head: body, handle, two controls and the panel line. */
export const AmpGlyph = (p: GlyphProps) => (
  <Glyph {...p}>
    <rect x="2" y="5" width="12" height="8" rx="1.5" />
    <path d="M6 5V3.5h4V5M5 8.25h0M7.5 8.25h0M4.5 10.75h7" />
  </Glyph>
);

/** Cabinet with a speaker cone. */
export const CabGlyph = (p: GlyphProps) => (
  <Glyph {...p}>
    <rect x="2" y="2" width="12" height="12" rx="1.5" />
    <circle cx="8" cy="8" r="3.5" />
    <path d="M8 8h0" />
  </Glyph>
);

/** Three faders at different positions. */
export const EqGlyph = (p: GlyphProps) => (
  <Glyph {...p}>
    <path d="M4 2.5v11M8 2.5v11M12 2.5v11M2.5 6h3M6.5 10h3M10.5 5h3" />
  </Glyph>
);

/** Two sine cycles. */
export const ModGlyph = (p: GlyphProps) => (
  <Glyph {...p}>
    <path d="M1.5 8Q3.125 2 4.75 8T8 8T11.25 8T14.5 8" />
  </Glyph>
);

/** Three diminishing taps. */
export const DelayGlyph = (p: GlyphProps) => (
  <Glyph {...p}>
    <path d="M3.5 2.5v11M8 4.5v7M12.5 6.5v3" />
  </Glyph>
);

/** A source and three wavefronts spreading from it. */
export const ReverbGlyph = (p: GlyphProps) => (
  <Glyph {...p}>
    <path d="M2.5 8h0" />
    <path d="M5.78 5.7a4 4 0 0 1 0 4.6" />
    <path d="M8.64 3.7a7.5 7.5 0 0 1 0 8.6" />
    <path d="M11.5 1.69a11 11 0 0 1 0 12.62" />
  </Glyph>
);

/**
 * A hosted plugin: a module with pins on both sides. One glyph for all three
 * slots; what is in the slot is named in text.
 */
export const FxGlyph = (p: GlyphProps) => (
  <Glyph {...p}>
    <rect x="4.5" y="3.5" width="7" height="9" rx="1.5" />
    <path d="M4.5 6.25H2M4.5 9.75H2M11.5 6.25H14M11.5 9.75H14" />
  </Glyph>
);

/** Split: one line forking into two. */
export const SplitGlyph = (p: GlyphProps) => (
  <Glyph {...p}>
    <path d="M2 8h3.5l4-4H14M5.5 8l4 4H14" />
  </Glyph>
);

/** Mixer: two lines joining into one. */
export const MixGlyph = (p: GlyphProps) => (
  <Glyph {...p}>
    <path d="M2 4h4.5l4 4H14M2 12h4.5l4-4" />
  </Glyph>
);

const BASE_GLYPH: Record<BaseBlockId, (p: GlyphProps) => ReactNode> = {
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
  split: SplitGlyph,
  mix: MixGlyph,
  // Furniture, never drawn; the Record must be total.
  lane2: SplitGlyph,
};

/** Every instance draws its kind's glyph: a second delay is a delay. Which
 *  instance it is comes from the caption, not the icon. */
export const BLOCK_GLYPH: Record<BlockId, (p: GlyphProps) => ReactNode> =
  blockRecord((id) => BASE_GLYPH[kindOf(id)]);

export function BlockGlyph({ block, ...rest }: GlyphProps & { block: BlockId }) {
  const Component = BLOCK_GLYPH[block];
  return <Component {...rest} />;
}

/* ────────────────────────────── endpoint glyphs ────────────────────────── */

/** Input: an instrument jack and its cable. */
export const InputGlyph = (p: GlyphProps) => (
  <Glyph {...p}>
    <circle cx="5.5" cy="8" r="3.5" />
    <path d="M9 8h5" />
  </Glyph>
);

/** Output: a speaker. */
export const OutputGlyph = (p: GlyphProps) => (
  <Glyph {...p}>
    <path d="M2.5 6h2.25L8.5 3v10L4.75 10H2.5z" />
    <path d="M11 5.5a3.5 3.5 0 0 1 0 5" />
  </Glyph>
);

/** The power symbol: broken ring and stem. */
export const PowerGlyph = (p: GlyphProps) => (
  <Glyph {...p}>
    <path d="M11.18 4.6a5 5 0 1 1-6.36 0" />
    <path d="M8 2.5v5" />
  </Glyph>
);
