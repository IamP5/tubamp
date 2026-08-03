/**
 * The drawer's small shared pieces: TONE3000 attribution, tone art, creator and
 * format chrome, the favorite heart.
 *
 * The mark and the creator line are not decoration — the TONE3000 API terms
 * require branding plus creator attribution on every tone list and detail view
 * (docs/research/tone3000-api.md §Design Requirements).
 */
import { useState, type ReactNode } from "react";
import { motion } from "motion/react";
import { cx } from "../../components";
import { spring } from "../../theme/motion";
import type { T3kTone } from "../../bridge";
import { artHues, formatCount } from "./labels";
import s from "./t3k.module.css";

export function HeartIcon({ filled, size = 14 }: { filled?: boolean; size?: number }) {
  return (
    <svg
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

export function SearchIcon({ size = 13 }: { size?: number }) {
  return (
    <svg
      width={size}
      height={size}
      viewBox="0 0 16 16"
      fill="none"
      stroke="currentColor"
      strokeWidth={1.6}
      strokeLinecap="round"
      aria-hidden="true"
    >
      <circle cx="7" cy="7" r="4.2" />
      <path d="M10.3 10.3 13.5 13.5" />
    </svg>
  );
}

/** The persistent TONE3000 attribution mark. */
export function T3kMark({ compact }: { compact?: boolean }) {
  return (
    <span className={cx(s.mark, compact && s.markCompact)}>
      <span className={s.markGlyph} aria-hidden="true">
        <i />
        <i />
        <i />
      </span>
      {compact ? "TONE3000" : "Powered by TONE3000"}
    </span>
  );
}

/**
 * Tone art. The hashed gradient always paints; the catalog image covers it once
 * it loads, and stays covered-up when there is none or the WebView blocks it.
 */
export function ToneArt({
  tone,
  className,
  children,
}: {
  tone: T3kTone;
  className?: string;
  children?: ReactNode;
}) {
  const [failedUrl, setFailedUrl] = useState<string | null>(null);
  const [from, to] = artHues(tone.id);
  const showImage = tone.imageUrl !== null && failedUrl !== tone.imageUrl;

  return (
    <span
      className={cx(s.art, showImage && s.artPhoto, className)}
      style={{
        backgroundImage: `linear-gradient(135deg, hsl(${from} 48% 38%), hsl(${to} 42% 10%))`,
      }}
    >
      {tone.imageUrl !== null && showImage && (
        <img
          className={s.artImage}
          src={tone.imageUrl}
          alt=""
          loading="lazy"
          onError={() => setFailedUrl(tone.imageUrl)}
        />
      )}
      {children}
    </span>
  );
}

export function Avatar({ tone, size = 14 }: { tone: T3kTone; size?: number }) {
  const [hue] = artHues(tone.creator.username.length * 31 + tone.id);
  return (
    <span
      className={s.avatar}
      style={{
        width: size,
        height: size,
        background: tone.creator.avatarUrl
          ? `center / cover url("${tone.creator.avatarUrl}")`
          : `hsl(${hue} 40% 45%)`,
        fontSize: Math.round(size * 0.58),
      }}
      aria-hidden="true"
    >
      {tone.creator.avatarUrl ? "" : tone.creator.username.slice(0, 1).toUpperCase()}
    </span>
  );
}

/** A2 for NAM captures, IR for impulse responses — the two formats this plugin loads. */
export function FormatBadge({ tone }: { tone: T3kTone }) {
  const ir = tone.format === "ir";
  return (
    <span className={cx(s.badge, ir && s.badgeIr)}>{ir ? "IR" : "A2"}</span>
  );
}

export function HeartButton({
  tone,
  showCount,
  onToggle,
}: {
  tone: T3kTone;
  showCount?: boolean;
  onToggle(tone: T3kTone): void;
}) {
  return (
    <motion.button
      type="button"
      className={cx(s.heart, tone.favorited && s.heartOn)}
      aria-label={tone.favorited ? "Remove from favorites" : "Add to favorites"}
      aria-pressed={tone.favorited}
      whileTap={{ scale: 0.86 }}
      transition={spring.micro}
      onClick={(e) => {
        e.stopPropagation();
        onToggle(tone);
      }}
    >
      <HeartIcon filled={tone.favorited} />
      {showCount && (
        <span className={s.heartCount}>{formatCount(tone.favoritesCount)}</span>
      )}
    </motion.button>
  );
}
