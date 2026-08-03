/** One 64px catalog row: art, title, creator line, counts, heart. */
import { motion } from "motion/react";
import { DownloadIcon, Skeleton } from "../../components";
import { noMotion, spring, staggerFor } from "../../theme/motion";
import { useStore } from "../../store";
import type { T3kTone } from "../../bridge";
import { Avatar, FormatBadge, HeartButton, ToneArt } from "./parts";
import { formatCount, GEAR_LABELS } from "./labels";
import s from "./t3k.module.css";

export function ToneRow({
  tone,
  index,
  reduced,
  onOpen,
  onToggleFavorite,
}: {
  tone: T3kTone;
  index: number;
  reduced: boolean;
  onOpen(tone: T3kTone): void;
  onToggleFavorite(tone: T3kTone): void;
}) {
  const inChain = useStore((st) => {
    const paths = st.t3kDownloaded[tone.id];
    if (!paths) return false;
    return paths.some((p) => p === st.model?.path || p === st.ir?.path);
  });

  return (
    <motion.li
      className={s.row}
      initial={reduced ? false : { opacity: 0, x: 14 }}
      animate={{ opacity: 1, x: 0 }}
      transition={
        reduced ? noMotion : { ...spring.reflow, delay: index * staggerFor(12) }
      }
    >
      <div
        className={s.rowHit}
        role="button"
        tabIndex={0}
        onClick={() => onOpen(tone)}
        onKeyDown={(e) => {
          if (e.key === "Enter" || e.key === " ") onOpen(tone);
        }}
      >
        <ToneArt tone={tone} className={s.rowArt}>
          {inChain && <span className={s.artLoadedDot} />}
        </ToneArt>

        <span className={s.rowBody}>
          <span className={s.rowTitle}>{tone.title}</span>
          <span className={s.rowMeta}>
            <Avatar tone={tone} />
            <span className={s.rowUser}>{tone.creator.username}</span>
            <span className={s.dot}>·</span>
            <span className={s.rowGear}>{GEAR_LABELS[tone.gear] ?? tone.gear}</span>
            <FormatBadge tone={tone} />
          </span>
        </span>

        <span className={s.rowTrail}>
          <HeartButton tone={tone} showCount onToggle={onToggleFavorite} />
          <span className={s.rowDownloads}>
            <DownloadIcon size={11} />
            {formatCount(tone.downloadsCount)}
          </span>
        </span>
      </div>
    </motion.li>
  );
}

export function RowSkeleton() {
  return (
    <li className={s.row}>
      <div className={s.rowHit}>
        <Skeleton width={44} height={44} radius="var(--radius-sm)" />
        <span className={s.rowBody}>
          <Skeleton width="72%" height={11} />
          <Skeleton width="46%" height={9} style={{ marginTop: 7 }} />
        </span>
      </div>
    </li>
  );
}
