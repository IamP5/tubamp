/**
 * One 56px catalog row: thumbnail, title, creator line, heart, and a one-click
 * Load that never leaves the list.
 *
 * The row body opens the detail level; Load resolves the tone's files, picks
 * the default (store `t3kLoadTone`), reuses a library copy or downloads it, and
 * loads it into the drawer's target. Its state is derived from the store:
 * Load to progress along the row's bottom edge to Loaded, or Retry.
 */
import { useLayoutEffect, useRef, useState } from "react";
import { motion } from "motion/react";
import { Button, CheckIcon, DownloadIcon, Skeleton, Spinner, Tooltip, cx } from "../../components";
import { BLOCK_INFO } from "../../theme/blocks";
import { noMotion, tween } from "../../theme/motion";
import {
  loadedPathOf,
  loadedTargetsOf,
  selectT3kTarget,
  T3K_TARGET_LABELS,
  tonePaths,
  useStore,
} from "../../store";
import type { T3kTarget, T3kTone } from "../../bridge";
import { HeartButton, RowProgress, Thumb } from "./parts";
import { formatCount, GEAR_LABELS, includesCab } from "./labels";
import s from "./t3k.module.css";

/** The slot a tone of this format loads into, given the drawer's target. */
export function targetFor(tone: T3kTone, target: T3kTarget): T3kTarget {
  if (tone.format === "ir") return "cab";
  return target === "cab" ? "amp" : target;
}

/** A tone's one-click load as the row shows it. `pct` is 0..1 while the file
 *  downloads, null in the phases without a percentage. */
function useRowLoad(toneId: number) {
  const load = useStore((st) => st.t3kLoads[toneId]);
  const progress = useStore((st) =>
    load?.modelId != null ? st.downloads[load.modelId] : undefined,
  );
  const busy = load !== undefined && load.phase !== "failed";
  const pct = load?.phase === "downloading" ? (progress ?? 0) : null;
  return { load, busy, pct };
}

/** One line of text, truncated, with the full text in a tooltip only when cut. */
export function TruncatedText({ text, className }: { text: string; className: string }) {
  const ref = useRef<HTMLSpanElement>(null);
  const [cut, setCut] = useState(false);
  useLayoutEffect(() => {
    const el = ref.current;
    if (el) setCut(el.scrollWidth > el.clientWidth + 1);
  }, [text]);
  return (
    <Tooltip label={text} disabled={!cut} className={s.truncAnchor}>
      <span ref={ref} className={className}>
        {text}
      </span>
    </Tooltip>
  );
}

/** Rows of a new page come in top down, 20 ms apart, the first eight only: a
 *  long page never takes longer to show than a short one. */
const ROW_STAGGER = 0.02;
const ROW_STAGGER_MAX = 8;

export function ToneRow({
  tone,
  index = 0,
  reduced,
  onOpen,
  onToggleFavorite,
}: {
  tone: T3kTone;
  /** Position in the list: rows of a fresh page arrive a beat apart, top down. */
  index?: number;
  reduced: boolean;
  onOpen(tone: T3kTone): void;
  onToggleFavorite(tone: T3kTone): void;
}) {
  const drawerTarget = useStore(selectT3kTarget);
  const target = targetFor(tone, drawerTarget);
  const loadedHere = useStore((st) => {
    const path = loadedPathOf(st, target);
    return path !== null && tonePaths(st, tone.id).includes(path);
  });
  const loadedElsewhere = useStore(
    (st) => loadedTargetsOf(st, tone.id).find((t) => t !== target) ?? null,
  );
  const { load, busy, pct } = useRowLoad(tone.id);
  const error = load?.phase === "failed" ? load.error : null;

  return (
    <motion.li
      className={s.row}
      /* The hue of where this tone goes: a capture into the amp is amber, an
         IR into the cab is teal, so "Loaded" matches the block that has it. */
      data-family={BLOCK_INFO[target].family}
      initial={reduced ? false : { opacity: 0, y: 4 }}
      animate={{ opacity: 1, y: 0 }}
      transition={
        reduced ? noMotion : { ...tween.enter, delay: Math.min(index, ROW_STAGGER_MAX) * ROW_STAGGER }
      }
    >
      <div
        className={s.rowHit}
        role="button"
        tabIndex={0}
        aria-label={`${tone.title} by ${tone.creator.username} — details`}
        onClick={() => onOpen(tone)}
        onKeyDown={(e) => {
          if (e.target !== e.currentTarget) return;
          if (e.key === "Enter" || e.key === " ") {
            e.preventDefault();
            onOpen(tone);
          }
        }}
      >
        <Thumb tone={tone} />

        <span className={s.rowBody}>
          <TruncatedText text={tone.title} className={s.rowTitle} />
          {/* The creator always stays on the line (licence): an error takes the
              place of the gear and count, never of the credit. "includes a
              cab" outranks the download count for the space. */}
          <span className={s.rowMeta}>
            <span className={s.rowCreator}>{tone.creator.username}</span>
            {error ? (
              <span className={s.rowError} title={error}>
                {error}
              </span>
            ) : (
              <>
                <span className={s.rowGear}>
                  {GEAR_LABELS[tone.gear] ?? tone.gear}
                  {includesCab(tone.gear) && " · with cab"}
                </span>
                {includesCab(tone.gear) && !loadedElsewhere ? null : loadedElsewhere ? (
                  <span className={s.rowCount}>in {T3K_TARGET_LABELS[loadedElsewhere]}</span>
                ) : (
                  <span className={s.rowCount} title="Downloads">
                    <DownloadIcon size={12} />
                    <span className={s.mono}>{formatCount(tone.downloadsCount)}</span>
                  </span>
                )}
              </>
            )}
          </span>
        </span>

        <HeartButton
          tone={tone}
          className={cx(s.rowHeart, tone.favorited && s.rowHeartOn)}
          onToggle={onToggleFavorite}
        />
        <span className={s.rowAction}>
          <LoadControl
            tone={tone}
            target={target}
            loaded={loadedHere}
            busy={busy}
            pct={pct}
            failed={load?.phase === "failed"}
            error={error}
          />
        </span>
      </div>
      {busy && <RowProgress value={pct} />}
    </motion.li>
  );
}

/** The row's state control: Load to percent to Loaded, or Retry. */
function LoadControl({
  tone,
  target,
  loaded,
  busy,
  pct,
  failed,
  error,
}: {
  tone: T3kTone;
  target: T3kTarget;
  loaded: boolean;
  busy: boolean;
  pct: number | null;
  failed: boolean;
  error: string | null;
}) {
  const installed = useStore((st) => st.t3kInstalled.some((f) => f.toneId === tone.id));
  const loadTone = useStore((st) => st.t3kLoadTone);
  const label = T3K_TARGET_LABELS[target];
  const unavailable = tone.format === "nam" && tone.modelsCount === 0;
  const stop = (e: React.SyntheticEvent): void => e.stopPropagation();

  if (busy) {
    return (
      <span
        className={s.stateText}
        role="status"
        aria-label={
          pct !== null ? `Downloading ${Math.round(pct * 100)}%` : `Loading into ${label}`
        }
        onClick={stop}
      >
        {pct === null ? (
          <Spinner size={12} />
        ) : (
          <span className={s.mono}>{Math.round(pct * 100)}%</span>
        )}
      </span>
    );
  }

  if (failed) {
    return (
      <button
        type="button"
        className={s.retry}
        title={error ?? "Failed"}
        aria-label={`Retry loading ${tone.title} into ${label}`}
        onClick={(e) => {
          stop(e);
          void loadTone(tone);
        }}
      >
        Retry
      </button>
    );
  }

  if (loaded) {
    return (
      <span className={cx(s.stateText, s.loaded)} onClick={stop}>
        <CheckIcon size={14} />
        Loaded
      </span>
    );
  }

  if (unavailable) {
    return (
      <span
        className={s.stateText}
        title="This tone has no A2 captures, which is all this plugin loads."
        onClick={stop}
      >
        No A2
      </span>
    );
  }

  return (
    <Button
      size="sm"
      aria-label={`Load ${tone.title} into ${label}`}
      title={installed ? `Load into ${label}` : `Download and load into ${label}`}
      onClick={(e) => {
        stop(e);
        void loadTone(tone);
      }}
    >
      Load
    </Button>
  );
}

export function RowSkeleton() {
  return (
    <li className={s.row}>
      <div className={s.rowHit}>
        <Skeleton width={40} height={40} radius="var(--ds-r-2)" />
        <span className={s.rowBody}>
          <Skeleton width="64%" height={12} radius="var(--ds-r-1)" />
          <Skeleton width="40%" height={10} radius="var(--ds-r-1)" />
        </span>
      </div>
    </li>
  );
}
