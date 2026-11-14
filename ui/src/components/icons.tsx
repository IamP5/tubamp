/**
 * Chrome icon set. Every icon is drawn on a 16px box with a 1.5px stroke, round
 * caps and joins, `currentColor`, no fills (a dot is a zero-length round-capped
 * line). The parent's text token decides how an icon reads.
 *
 * Block iconography lives in ./blockGlyphs.tsx; `BlockIcon` below only presets
 * its size and stroke.
 */
import type { SVGProps } from "react";
import type { BlockId } from "../bridge/types";
import { BlockGlyph } from "./blockGlyphs";

export interface IconProps extends Omit<SVGProps<SVGSVGElement>, "children"> {
  /** Rendered box in CSS px (default 16). The stroke scales with it. */
  size?: number;
}

function Svg({ size = 16, ...rest }: IconProps & { children?: React.ReactNode }) {
  return (
    <svg
      width={size}
      height={size}
      viewBox="0 0 16 16"
      fill="none"
      stroke="currentColor"
      strokeWidth={1.5}
      strokeLinecap="round"
      strokeLinejoin="round"
      aria-hidden="true"
      focusable="false"
      {...rest}
    />
  );
}

/* ─────────────────────────────── actions ──────────────────────────────── */

export const CloseIcon = (p: IconProps) => (
  <Svg {...p}>
    <path d="M4 4l8 8M12 4l-8 8" />
  </Svg>
);

export const PlusIcon = (p: IconProps) => (
  <Svg {...p}>
    <path d="M8 3.5v9M3.5 8h9" />
  </Svg>
);

export const MinusIcon = (p: IconProps) => (
  <Svg {...p}>
    <path d="M3.5 8h9" />
  </Svg>
);

export const CheckIcon = (p: IconProps) => (
  <Svg {...p}>
    <path d="M3.5 8.5l3 3 6-7" />
  </Svg>
);

export const TrashIcon = (p: IconProps) => (
  <Svg {...p}>
    <path d="M2.5 4.5h11M6 4.5V3a.5.5 0 0 1 .5-.5h3a.5.5 0 0 1 .5.5v1.5M4 4.5l.6 8.1a1 1 0 0 0 1 .9h4.8a1 1 0 0 0 1-.9l.6-8.1M6.75 7v3.75M9.25 7v3.75" />
  </Svg>
);

/** Horizontal dots: the "More" menu trigger. */
export const MoreIcon = (p: IconProps) => (
  <Svg {...p} strokeWidth={2}>
    <path d="M3.5 8h0M8 8h0M12.5 8h0" />
  </Svg>
);

export const SearchIcon = (p: IconProps) => (
  <Svg {...p}>
    <circle cx="7" cy="7" r="4.5" />
    <path d="M10.5 10.5l3 3" />
  </Svg>
);

export const DownloadIcon = (p: IconProps) => (
  <Svg {...p}>
    <path d="M8 2.5v8M4.5 7L8 10.5 11.5 7M3 13.5h10" />
  </Svg>
);

export const SaveIcon = (p: IconProps) => (
  <Svg {...p}>
    <path d="M3.5 2.5h7l3 3v7a1 1 0 0 1-1 1h-9a1 1 0 0 1-1-1v-9a1 1 0 0 1 1-1z" />
    <path d="M5.5 2.5v3h4v-3M5 13.5V10h6v3.5" />
  </Svg>
);

export const UndoIcon = (p: IconProps) => (
  <Svg {...p}>
    <path d="M5.5 3.5l-3 3 3 3" />
    <path d="M2.5 6.5h7a3.5 3.5 0 0 1 0 7H7" />
  </Svg>
);

/** Circular arrow: try again. */
export const RetryIcon = (p: IconProps) => (
  <Svg {...p}>
    <path d="M13.5 8a5.5 5.5 0 1 1-1.6-3.9" />
    <path d="M12.25 1.75v2.75H9.5" />
  </Svg>
);

/** Four corner brackets: zoom to fit. */
export const FitIcon = (p: IconProps) => (
  <Svg {...p}>
    <path d="M2.5 6V3.5a1 1 0 0 1 1-1H6M10 2.5h2.5a1 1 0 0 1 1 1V6M13.5 10v2.5a1 1 0 0 1-1 1H10M6 13.5H3.5a1 1 0 0 1-1-1V10" />
  </Svg>
);

/** Open a link outside the plug-in. */
export const ExternalIcon = (p: IconProps) => (
  <Svg {...p}>
    <path d="M9.5 2.5h4v4M13.5 2.5L8 8" />
    <path d="M12 9.5v3a1 1 0 0 1-1 1H3.5a1 1 0 0 1-1-1V5a1 1 0 0 1 1-1h3" />
  </Svg>
);

export const LinkIcon = (p: IconProps) => (
  <Svg {...p}>
    <path d="M6.75 9.25a3 3 0 0 0 4.25 0l2-2a3 3 0 0 0-4.25-4.25l-.75.75" />
    <path d="M9.25 6.75a3 3 0 0 0-4.25 0l-2 2a3 3 0 0 0 4.25 4.25l.75-.75" />
  </Svg>
);

export const FilterIcon = (p: IconProps) => (
  <Svg {...p}>
    <path d="M2.5 4h11M4.5 8h7M6.5 12h3" />
  </Svg>
);

export const PlayIcon = (p: IconProps) => (
  <Svg {...p}>
    <path d="M5 3.25v9.5a.5.5 0 0 0 .76.43l7.6-4.75a.5.5 0 0 0 0-.86l-7.6-4.75A.5.5 0 0 0 5 3.25z" />
  </Svg>
);

export const PowerIcon = (p: IconProps) => (
  <Svg {...p}>
    <path d="M11.18 4.6a5 5 0 1 1-6.36 0" />
    <path d="M8 2.5v5" />
  </Svg>
);

const GEAR =
  "M7.12 1.81L8.88 1.81L9.18 3.40L10.42 3.91L11.75 3.00L13.00 4.25L12.09 5.58L12.60 6.82L14.19 7.12L14.19 8.88L12.60 9.18L12.09 10.42L13.00 11.75L11.75 13.00L10.42 12.09L9.18 12.60L8.88 14.19L7.12 14.19L6.82 12.60L5.58 12.09L4.25 13.00L3.00 11.75L3.91 10.42L3.40 9.18L1.81 8.88L1.81 7.12L3.40 6.82L3.91 5.58L3.00 4.25L4.25 3.00L5.58 3.91L6.82 3.40Z";

export const SettingsIcon = (p: IconProps) => (
  <Svg {...p}>
    <path d={GEAR} />
    <circle cx="8" cy="8" r="2" />
  </Svg>
);

/** Older name for `SettingsIcon`. */
export const GearIcon = SettingsIcon;

/* ────────────────────────────── navigation ────────────────────────────── */

export const ChevronLeftIcon = (p: IconProps) => (
  <Svg {...p}>
    <path d="M10 3.5L5.5 8l4.5 4.5" />
  </Svg>
);

export const ChevronRightIcon = (p: IconProps) => (
  <Svg {...p}>
    <path d="M6 3.5L10.5 8 6 12.5" />
  </Svg>
);

export const ChevronDownIcon = (p: IconProps) => (
  <Svg {...p}>
    <path d="M4 6l4 4 4-4" />
  </Svg>
);

export const ArrowLeftIcon = (p: IconProps) => (
  <Svg {...p}>
    <path d="M13 8H3M7 4L3 8l4 4" />
  </Svg>
);

/* ─────────────────────────────── marks ────────────────────────────────── */

const HEART =
  "M8 13.25S2.5 10 2.5 6.25A3 3 0 0 1 5.5 3.25c1.1 0 2 .6 2.5 1.5.5-.9 1.4-1.5 2.5-1.5a3 3 0 0 1 3 3c0 3.75-5.5 7-5.5 7z";

export const HeartIcon = ({ filled, ...p }: IconProps & { filled?: boolean }) => (
  <Svg {...p}>
    <path d={HEART} fill={filled ? "currentColor" : "none"} />
  </Svg>
);

export const HeartFilledIcon = (p: IconProps) => <HeartIcon {...p} filled />;

export const StarIcon = ({ filled, ...p }: IconProps & { filled?: boolean }) => (
  <Svg {...p}>
    <path
      d="M8 2.5l1.7 3.5 3.8.5-2.8 2.7.7 3.8L8 11.2 4.6 13l.7-3.8L2.5 6.5l3.8-.5z"
      fill={filled ? "currentColor" : "none"}
    />
  </Svg>
);

/* ─────────────────────────────── status ───────────────────────────────── */

export const WarningIcon = (p: IconProps) => (
  <Svg {...p}>
    <path d="M7.13 3a1 1 0 0 1 1.74 0l5.2 9a1 1 0 0 1-.87 1.5H2.8a1 1 0 0 1-.87-1.5z" />
    <path d="M8 6.5v3M8 11.5h0" />
  </Svg>
);

/** Circled exclamation: an error. */
export const AlertIcon = (p: IconProps) => (
  <Svg {...p}>
    <circle cx="8" cy="8" r="5.5" />
    <path d="M8 5.25v3.25M8 10.75h0" />
  </Svg>
);

export const InfoIcon = (p: IconProps) => (
  <Svg {...p}>
    <circle cx="8" cy="8" r="5.5" />
    <path d="M8 7.5v3.25M8 5.25h0" />
  </Svg>
);

export const CheckCircleIcon = (p: IconProps) => (
  <Svg {...p}>
    <circle cx="8" cy="8" r="5.5" />
    <path d="M5.75 8.25l1.5 1.5 3-3.25" />
  </Svg>
);

/* ─────────────────────────────── signal ───────────────────────────────── */

export const InputIcon = (p: IconProps) => (
  <Svg {...p}>
    <path d="M2.5 8h8M7.5 5l3 3-3 3M13 3.5v9" />
  </Svg>
);

export const OutputIcon = (p: IconProps) => (
  <Svg {...p}>
    <path d="M5.5 8h8M10.5 5l3 3-3 3M3 3.5v9" />
  </Svg>
);

/* ─────────────────────────────── block icons ───────────────────────────── */

/**
 * Chrome-sized block icon: a size/stroke preset over `BlockGlyph`, so a block
 * reads identically on its board card, in the editor header and in a menu row.
 */
export function BlockIcon({
  block,
  size = 16,
  strokeWidth = 1.5,
  className,
}: {
  block: BlockId;
  size?: number;
  strokeWidth?: number;
  className?: string;
}) {
  return (
    <BlockGlyph
      block={block}
      size={size}
      strokeWidth={strokeWidth}
      className={className}
    />
  );
}
