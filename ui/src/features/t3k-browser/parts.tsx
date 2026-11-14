/**
 * The drawer's small shared pieces: icons the kit does not have, the tone
 * thumbnail, the favourite heart and the load-progress bar.
 *
 * Creator attribution is not decoration: the TONE3000 API terms require
 * branding plus the creator on every tone list and detail view
 * (docs/research/tone3000-api.md §Design Requirements).
 */
import { useState } from "react";
import { motion } from "motion/react";
import { AmpGlyph, CabGlyph, IconButton, cx } from "../../components";
import { tween } from "../../theme/motion";
import type { T3kTone } from "../../bridge";
import s from "./t3k.module.css";

export function HeartIcon({ filled, size = 16 }: { filled?: boolean; size?: number }) {
  return (
    <svg
      className={filled ? s.heartFilled : undefined}
      width={size}
      height={size}
      viewBox="0 0 16 16"
      fill={filled ? "currentColor" : "none"}
      stroke="currentColor"
      strokeWidth={1.5}
      strokeLinejoin="round"
      aria-hidden="true"
    >
      <path d="M8 13.3C8 13.3 2.5 10 2.5 6.4A2.9 2.9 0 0 1 8 5a2.9 2.9 0 0 1 5.5 1.4c0 3.6-5.5 6.9-5.5 6.9z" />
    </svg>
  );
}

export function SearchIcon({ size = 16 }: { size?: number }) {
  return (
    <svg
      width={size}
      height={size}
      viewBox="0 0 16 16"
      fill="none"
      stroke="currentColor"
      strokeWidth={1.5}
      strokeLinecap="round"
      aria-hidden="true"
    >
      <circle cx="7" cy="7" r="4.2" />
      <path d="M10.3 10.3 13.5 13.5" />
    </svg>
  );
}

export function ExternalIcon({ size = 12 }: { size?: number }) {
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
    >
      <path d="M9.5 2.5h4v4M13.5 2.5 7.5 8.5M11.5 9.5v3a1 1 0 0 1-1 1h-7a1 1 0 0 1-1-1v-7a1 1 0 0 1 1-1h3" />
    </svg>
  );
}

/**
 * 40px thumbnail: the tone's catalog image when it has one and the WebView
 * loads it, otherwise a neutral tile with the amp or cab glyph.
 */
export function Thumb({ tone }: { tone: T3kTone }) {
  const [failedUrl, setFailedUrl] = useState<string | null>(null);
  const showImage = tone.imageUrl !== null && failedUrl !== tone.imageUrl;
  const Glyph = tone.format === "ir" ? CabGlyph : AmpGlyph;

  return (
    <span className={s.thumb} aria-hidden="true">
      {showImage && tone.imageUrl !== null ? (
        <img
          className={s.thumbImage}
          src={tone.imageUrl}
          alt=""
          loading="lazy"
          onError={() => setFailedUrl(tone.imageUrl)}
        />
      ) : (
        <Glyph size={20} strokeWidth={1.5} />
      )}
    </span>
  );
}

export function HeartButton({
  tone,
  className,
  onToggle,
}: {
  tone: T3kTone;
  className?: string;
  onToggle(tone: T3kTone): void;
}) {
  return (
    <IconButton
      className={className}
      aria-label={tone.favorited ? "Remove from favorites" : "Add to favorites"}
      aria-pressed={tone.favorited}
      onClick={(e) => {
        e.stopPropagation();
        onToggle(tone);
      }}
    >
      <HeartIcon filled={tone.favorited} />
    </IconButton>
  );
}

/**
 * The 2px accent bar along a row's bottom edge while its file downloads.
 * `value` null = the in-between phases with no percentage (resolving files,
 * loading into the slot): the bar sits full but faint.
 */
export function RowProgress({ value }: { value: number | null }) {
  return (
    <span className={s.progress} aria-hidden="true">
      <motion.span
        className={cx(s.progressFill, value === null && s.progressIdle)}
        initial={false}
        animate={{ scaleX: value === null ? 1 : Math.max(0.02, value) }}
        transition={tween.enter}
      />
    </span>
  );
}
