/**
 * One 64px catalog row: art, title, creator line, heart — and a one-click Load.
 *
 * The row body opens the detail level; the Load action resolves the tone's
 * files, picks the default (store `t3kLoadTone`), reuses a library copy or
 * downloads it, and loads it into the drawer's target. Its state is derived
 * from the store: idle → resolving → % → Loaded, or Failed + Retry.
 */
import { motion } from "motion/react";
import { CheckIcon, DownloadIcon, Skeleton, Spinner, cx } from "../../components";
import { noMotion, spring, staggerFor, tween } from "../../theme/motion";
import {
  loadedPathOf,
  loadedTargetsOf,
  selectT3kTarget,
  T3K_TARGET_LABELS,
  tonePaths,
  useStore,
} from "../../store";
import type { T3kTarget, T3kTone } from "../../bridge";
import { Avatar, FormatBadge, HeartButton, RetryIcon, ToneArt } from "./parts";
import { formatCount, GEAR_LABELS } from "./labels";
import s from "./t3k.module.css";

/** The slot a tone of this format loads into, given the drawer's target. */
function targetFor(tone: T3kTone, target: T3kTarget): T3kTarget {
  if (tone.format === "ir") return "cab";
  return target === "cab" ? "amp" : target;
}

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
  const drawerTarget = useStore(selectT3kTarget);
  const target = targetFor(tone, drawerTarget);
  const loadedHere = useStore((st) => {
    const path = loadedPathOf(st, target);
    return path !== null && tonePaths(st, tone.id).includes(path);
  });
  const loadedElsewhere = useStore((st) =>
    loadedTargetsOf(st, tone.id).find((t) => t !== target) ?? null,
  );
  const failed = useStore((st) => st.t3kLoads[tone.id]?.phase === "failed");
  const error = useStore((st) => st.t3kLoads[tone.id]?.error ?? null);

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
        className={cx(s.rowHit, loadedHere && s.rowHitLoaded)}
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
        <ToneArt tone={tone} className={s.rowArt}>
          {(loadedHere || loadedElsewhere) && <span className={s.artLoadedDot} />}
        </ToneArt>

        <span className={s.rowBody}>
          <span className={s.rowTitle}>{tone.title}</span>
          {failed && error ? (
            /* The creator stays on the line: the error takes the place of the
               gear and counts, never of the credit. */
            <span className={s.rowMeta}>
              <Avatar tone={tone} />
              <span className={s.rowUser}>{tone.creator.username}</span>
              <span className={s.dot}>·</span>
              <span className={s.rowError} title={error}>
                {error}
              </span>
            </span>
          ) : (
            <span className={s.rowMeta}>
              <Avatar tone={tone} />
              <span className={s.rowUser}>{tone.creator.username}</span>
              <span className={s.dot}>·</span>
              <span className={s.rowGear}>{GEAR_LABELS[tone.gear] ?? tone.gear}</span>
              <FormatBadge tone={tone} />
              {loadedElsewhere ? (
                <span className={s.elsewhereTag}>
                  in {T3K_TARGET_LABELS[loadedElsewhere]}
                </span>
              ) : (
                <span className={s.rowDownloads}>
                  <DownloadIcon size={10} />
                  {formatCount(tone.downloadsCount)}
                </span>
              )}
            </span>
          )}
        </span>

        <span className={s.rowTrail}>
          <HeartButton tone={tone} showCount onToggle={onToggleFavorite} />
          <RowAction tone={tone} target={target} loaded={loadedHere} />
        </span>
      </div>
    </motion.li>
  );
}

/** The row's primary action, every state of it. */
function RowAction({
  tone,
  target,
  loaded,
}: {
  tone: T3kTone;
  target: T3kTarget;
  loaded: boolean;
}) {
  const load = useStore((st) => st.t3kLoads[tone.id]);
  const progress = useStore((st) =>
    load?.modelId != null ? st.downloads[load.modelId] : undefined,
  );
  const installed = useStore((st) => st.t3kInstalled.some((f) => f.toneId === tone.id));
  const loadTone = useStore((st) => st.t3kLoadTone);

  const label = T3K_TARGET_LABELS[target];
  const unavailable = tone.format === "nam" && tone.modelsCount === 0;
  const stop = (e: React.SyntheticEvent): void => e.stopPropagation();

  if (load && load.phase !== "failed") {
    const pct = load.phase === "downloading" ? (progress ?? 0) : null;
    return (
      <span
        className={s.rowAction}
        role="status"
        aria-label={
          pct !== null ? `Downloading ${Math.round(pct * 100)}%` : `Loading into ${label}`
        }
        onClick={stop}
      >
        {pct === null ? (
          <span className={s.actionBusy}>
            <Spinner size={11} />
          </span>
        ) : (
          <span className={s.actionProgress}>
            <motion.span
              className={s.actionProgressFill}
              animate={{ scaleX: Math.max(0.06, pct) }}
              transition={tween.micro}
            />
            <span className={s.actionProgressText}>{Math.round(pct * 100)}%</span>
          </span>
        )}
      </span>
    );
  }

  if (load?.phase === "failed") {
    return (
      <button
        type="button"
        className={cx(s.rowAction, s.actionButton, s.actionRetry)}
        title={load.error ?? "Failed"}
        aria-label={`Retry loading ${tone.title} into ${label}`}
        onClick={(e) => {
          stop(e);
          void loadTone(tone);
        }}
      >
        <RetryIcon />
        Retry
      </button>
    );
  }

  if (loaded) {
    return (
      <span className={cx(s.rowAction, s.actionLoaded)} onClick={stop}>
        <CheckIcon size={11} />
        Loaded
      </span>
    );
  }

  if (unavailable) {
    return (
      <span
        className={cx(s.rowAction, s.actionNone)}
        title="This tone has no A2 captures, which is all this plugin loads."
        onClick={stop}
      >
        No A2
      </span>
    );
  }

  return (
    <button
      type="button"
      className={cx(s.rowAction, s.actionButton)}
      aria-label={`Load ${tone.title} into ${label}`}
      title={installed ? `Load into ${label}` : `Download and load into ${label}`}
      onClick={(e) => {
        stop(e);
        void loadTone(tone);
      }}
    >
      {!installed && <DownloadIcon size={11} />}
      Load
    </button>
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
