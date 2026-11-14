/**
 * One downloadable file of a tone: Get → progress → Loaded.
 *
 * Download state lives in the store, so leaving the detail level (or closing the
 * drawer) never loses a transfer. A file already in the library (by its
 * sidecar's modelId, else by the name the downloader gives it) is offered as a
 * plain Load — re-downloading a capture the user owns would waste a request the
 * TONE3000 rate limit is counting.
 */
import { motion } from "motion/react";
import { Button, CheckIcon, DownloadIcon, cx } from "../../components";
import { tween } from "../../theme/motion";
import {
  findInstalledPath,
  loadedPathOf,
  T3K_TARGET_LABELS,
  useStore,
} from "../../store";
import type { T3kModel, T3kTarget, T3kTone } from "../../bridge";
import { RetryIcon } from "./parts";
import { ARCH_LABELS, SIZE_LABELS } from "./labels";
import s from "./t3k.module.css";

export function ModelRow({
  tone,
  model,
  target,
  isDefault,
}: {
  tone: T3kTone;
  model: T3kModel;
  target: T3kTarget;
  isDefault: boolean;
}) {
  const isIr = model.kind === "wav";
  const progress = useStore((st) => st.downloads[model.id]);
  const installedPath = useStore((st) => findInstalledPath(st, model));
  const loaded = useStore(
    (st) => installedPath !== null && installedPath === loadedPathOf(st, target),
  );
  const rowLoad = useStore((st) => st.t3kLoads[tone.id]);
  const loadTone = useStore((st) => st.t3kLoadTone);

  const mine = rowLoad?.modelId === model.id ? rowLoad : undefined;
  const busyElsewhere = rowLoad !== undefined && rowLoad.phase !== "failed" && !mine;
  const label = T3K_TARGET_LABELS[target];

  return (
    <li className={loaded ? s.fileLoaded : s.file}>
      <span className={s.fileMain}>
        <span className={s.fileName} title={model.name}>
          {model.name}
        </span>
        <span className={s.fileMeta}>
          {isIr ? (
            <span className={cx(s.sizeTag, s.sizeTagIr)}>WAV · IR</span>
          ) : (
            <>
              <span className={s.sizeTag}>{SIZE_LABELS[model.size] ?? model.size}</span>
              {model.architecture && (
                <span className={cx(s.sizeTag, s.archTag)}>
                  {ARCH_LABELS[model.architecture] ?? model.architecture}
                </span>
              )}
            </>
          )}
          {isDefault && <span className={s.fileNote}>default</span>}
          {installedPath !== null && !loaded && (
            <span className={s.fileNote}>in library</span>
          )}
          {mine?.phase === "failed" && (
            <span className={s.fileError} title={mine.error ?? undefined}>
              failed
            </span>
          )}
        </span>
      </span>

      <span className={s.fileAction}>
        {loaded ? (
          <span className={s.loadedTag}>
            <CheckIcon size={12} />
            Loaded
          </span>
        ) : progress !== undefined ? (
          <span className={s.progressWrap}>
            <span className={s.progressTrack}>
              <motion.span
                className={s.progressFill}
                animate={{ scaleX: Math.max(0.04, progress) }}
                transition={tween.micro}
              />
            </span>
            <span className={s.progressPct}>{Math.round(progress * 100)}%</span>
          </span>
        ) : mine?.phase === "failed" ? (
          <Button
            size="sm"
            variant="danger"
            icon={<RetryIcon size={12} />}
            title={mine.error ?? undefined}
            onClick={() => void loadTone(tone, model)}
          >
            Retry
          </Button>
        ) : (
          <Button
            size="sm"
            variant="default"
            icon={installedPath ? undefined : <DownloadIcon size={13} />}
            disabled={busyElsewhere || mine !== undefined}
            title={`${installedPath ? "Load" : "Download and load"} into ${label}`}
            onClick={() => void loadTone(tone, model)}
          >
            {installedPath ? "Load" : "Get"}
          </Button>
        )}
      </span>
    </li>
  );
}
