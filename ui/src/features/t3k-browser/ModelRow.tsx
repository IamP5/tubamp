/**
 * One downloadable file of a tone: Get → progress → Loaded.
 *
 * Download state lives in the store, so leaving the detail level (or closing the
 * drawer) never loses a transfer. A file already in the library is offered as a
 * plain Load — re-downloading a capture the user owns would waste a request the
 * TONE3000 rate limit is counting.
 */
import { motion } from "motion/react";
import { Button, CheckIcon, DownloadIcon } from "../../components";
import { tween } from "../../theme/motion";
import { useStore } from "../../store";
import type { T3kModel, T3kTone } from "../../bridge";
import { SIZE_LABELS } from "./labels";
import s from "./t3k.module.css";

export function ModelRow({ tone, model }: { tone: T3kTone; model: T3kModel }) {
  const isIr = model.kind === "wav";
  const progress = useStore((st) => st.downloads[model.id]);
  const installed = useStore((st) =>
    (isIr ? st.irs : st.models).find((f) => f.name === model.name),
  );
  const currentPath = useStore((st) => (isIr ? st.ir?.path : st.model?.path) ?? null);
  const download = useStore((st) => st.t3kDownloadModel);
  const loadModel = useStore((st) => st.loadModel);
  const loadIr = useStore((st) => st.loadIr);

  const loaded = installed !== undefined && installed.path === currentPath;

  return (
    <li className={loaded ? s.fileLoaded : s.file}>
      <span className={s.fileMain}>
        <span className={s.fileName}>{model.name}</span>
        <span className={s.fileMeta}>
          <span className={s.sizeTag}>{SIZE_LABELS[model.size] ?? model.size}</span>
          <span className={s.fileKind}>
            {isIr ? "WAV · IR" : model.architecture ? `NAM · A${model.architecture}` : "NAM"}
          </span>
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
        ) : installed ? (
          <Button
            size="sm"
            onClick={() =>
              void (isIr ? loadIr(installed.path) : loadModel(installed.path))
            }
          >
            Load
          </Button>
        ) : (
          <Button
            size="sm"
            variant="primary"
            icon={<DownloadIcon size={13} />}
            onClick={() => void download(model, tone.id)}
          >
            Get
          </Button>
        )}
      </span>
    </li>
  );
}
