/**
 * One downloadable file of a tone, 40px: Load to progress to Loaded, or Retry.
 *
 * Download state lives in the store, so leaving the detail level (or closing the
 * drawer) never loses a transfer. A file already in the library (by its
 * sidecar's modelId, else by the name the downloader gives it) loads without a
 * download: re-downloading a capture the user owns would waste a request the
 * TONE3000 rate limit is counting.
 */
import { Button, CheckIcon, Spinner, cx } from "../../components";
import {
  findInstalledPath,
  loadedPathOf,
  t3kTargetLabel,
  useStore,
} from "../../store";
import type { T3kModel, T3kTarget, T3kTone } from "../../bridge";
import { RowProgress } from "./parts";
import { TruncatedText } from "./ToneRow";
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
  const busy = progress !== undefined || (mine !== undefined && mine.phase !== "failed");
  const label = useStore((st) => t3kTargetLabel(target, st.chainOrder));

  const meta = isIr
    ? ["WAV"]
    : [
        SIZE_LABELS[model.size] ?? model.size,
        model.architecture ? (ARCH_LABELS[model.architecture] ?? model.architecture) : null,
      ];
  if (isDefault) meta.push("default");
  if (installedPath !== null && !loaded) meta.push("in library");

  return (
    <li className={s.file}>
      <span className={s.fileMain}>
        <TruncatedText text={model.name} className={s.fileName} />
        <span className={s.fileMeta}>
          {mine?.phase === "failed" ? (
            <span className={s.fileError} title={mine.error ?? undefined}>
              {mine.error ?? "Failed"}
            </span>
          ) : (
            meta.filter(Boolean).join(" · ")
          )}
        </span>
      </span>

      <span className={s.rowAction}>
        {loaded && !busy ? (
          <span className={cx(s.stateText, s.loaded)}>
            <CheckIcon size={14} />
            Loaded
          </span>
        ) : busy ? (
          <span
            className={s.stateText}
            role="status"
            aria-label={
              progress !== undefined
                ? `Downloading ${Math.round(progress * 100)}%`
                : `Loading into ${label}`
            }
          >
            {progress !== undefined ? (
              <span className={s.mono}>{Math.round(progress * 100)}%</span>
            ) : (
              <Spinner size={12} />
            )}
          </span>
        ) : mine?.phase === "failed" ? (
          <button
            type="button"
            className={s.retry}
            title={mine.error ?? undefined}
            aria-label={`Retry loading ${model.name} into ${label}`}
            onClick={() => void loadTone(tone, model)}
          >
            Retry
          </button>
        ) : (
          <Button
            size="sm"
            disabled={busyElsewhere}
            title={`${installedPath ? "Load" : "Download and load"} into ${label}`}
            aria-label={`Load ${model.name} into ${label}`}
            onClick={() => void loadTone(tone, model)}
          >
            Load
          </Button>
        )}
      </span>
      {busy && <RowProgress value={progress ?? null} />}
    </li>
  );
}
