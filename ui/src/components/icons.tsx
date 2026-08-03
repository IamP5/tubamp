/**
 * Chrome icon set (16px box). Block iconography lives in ./blockGlyphs.tsx —
 * `BlockIcon` below only presets its size/stroke.
 *
 * All icons are stroke-based, `currentColor`, no fills — so a parent's colour
 * (block accent, text token) is the only thing that decides how they read.
 */
import type { SVGProps } from "react";
import type { BlockId } from "../bridge/types";
import { BlockGlyph } from "./blockGlyphs";

export interface IconProps extends Omit<SVGProps<SVGSVGElement>, "children"> {
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
      {...rest}
    />
  );
}

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

export const CheckIcon = (p: IconProps) => (
  <Svg {...p}>
    <path d="M3.5 8.5l3 3 6-7" />
  </Svg>
);

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
    <path d="M3.5 6L8 10.5 12.5 6" />
  </Svg>
);

export const MoreIcon = (p: IconProps) => (
  <Svg {...p}>
    <circle cx="3.5" cy="8" r="0.9" fill="currentColor" stroke="none" />
    <circle cx="8" cy="8" r="0.9" fill="currentColor" stroke="none" />
    <circle cx="12.5" cy="8" r="0.9" fill="currentColor" stroke="none" />
  </Svg>
);

export const TrashIcon = (p: IconProps) => (
  <Svg {...p}>
    <path d="M3 4.5h10M6.5 4.5V3h3v1.5M4.5 4.5l.6 8h5.8l.6-8M6.8 7v3.2M9.2 7v3.2" />
  </Svg>
);

export const GearIcon = (p: IconProps) => (
  <Svg {...p}>
    <circle cx="8" cy="8" r="2.2" />
    <path d="M8 1.8v1.6M8 12.6v1.6M14.2 8h-1.6M3.4 8H1.8M12.4 3.6l-1.1 1.1M4.7 11.3l-1.1 1.1M12.4 12.4l-1.1-1.1M4.7 4.7L3.6 3.6" />
  </Svg>
);

export const SaveIcon = (p: IconProps) => (
  <Svg {...p}>
    <path d="M3 3.5h7.5L13 6v6.5H3z" />
    <path d="M5.5 3.5v3h5v-3M5.5 12.5V9h5v3.5" />
  </Svg>
);

export const StarIcon = ({ filled, ...p }: IconProps & { filled?: boolean }) => (
  <Svg {...p}>
    <path
      d="M8 2.5l1.7 3.5 3.8.5-2.8 2.7.7 3.8L8 11.2 4.6 13l.7-3.8L2.5 6.5l3.8-.5z"
      fill={filled ? "currentColor" : "none"}
    />
  </Svg>
);

export const DownloadIcon = (p: IconProps) => (
  <Svg {...p}>
    <path d="M8 2.5v7.5M4.5 7L8 10.5 11.5 7M3 13h10" />
  </Svg>
);

export const AlertIcon = (p: IconProps) => (
  <Svg {...p}>
    <circle cx="8" cy="8" r="5.6" />
    <path d="M8 5v3.6M8 10.8v.6" />
  </Svg>
);

export const InfoIcon = (p: IconProps) => (
  <Svg {...p}>
    <circle cx="8" cy="8" r="5.6" />
    <path d="M8 7.4v3.4M8 5.1v.6" />
  </Svg>
);

export const CheckCircleIcon = (p: IconProps) => (
  <Svg {...p}>
    <circle cx="8" cy="8" r="5.6" />
    <path d="M5.6 8.2l1.7 1.7 3.2-3.6" />
  </Svg>
);

export const InputIcon = (p: IconProps) => (
  <Svg {...p}>
    <path d="M2.5 8h8M7.5 5l3 3-3 3M12.5 3.5v9" />
  </Svg>
);

export const OutputIcon = (p: IconProps) => (
  <Svg {...p}>
    <path d="M5.5 8h8M10.5 5l3 3-3 3M3.5 3.5v9" />
  </Svg>
);

/* ─────────────────────────────── block icons ───────────────────────────── */

/**
 * Chrome-sized block icon. Deliberately a thin preset over `BlockGlyph` (the
 * faithful `BlockIcons.h` port in ./blockGlyphs.tsx) rather than a second,
 * simplified path set — a block must read identically on its board card, in the
 * dock header and in any menu row.
 */
export function BlockIcon({
  block,
  size = 20,
  strokeWidth = 1.6,
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
