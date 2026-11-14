/**
 * TONE3000 tone picker — the overlay the native `browseClicked` flow raised as
 * a PopupMenu after `onToneSelected`.
 *
 * A tone carries several architecture-2 variants (Standard / Lite / Feather /
 * Nano); the user downloads one at a time. Progress and completion are store
 * state fed by `t3kProgress` / `t3kComplete`, so closing this modal — or
 * switching to a different block entirely — never loses a download.
 *
 * A tone with zero models never reaches this component: the store toasts it
 * (the native UI failed silently there).
 */
import { Button, CheckIcon, Modal } from "../../components";
import { useStore } from "../../store";
import { ARCH_LABELS, SIZE_LABELS } from "../t3k-browser/labels";
import s from "./toneModal.module.css";

export function ToneModal() {
  const tone = useStore((st) => st.tone);
  const downloads = useStore((st) => st.downloads);
  const models = useStore((st) => st.models);
  const currentPath = useStore((st) => st.model?.path ?? null);
  const dismissTone = useStore((st) => st.dismissTone);
  const t3kDownloadModel = useStore((st) => st.t3kDownloadModel);
  const loadModel = useStore((st) => st.loadModel);

  return (
    <Modal
      open={tone !== null}
      onClose={dismissTone}
      title="Choose a capture"
      footer={<span className={s.attribution}>Powered by TONE3000</span>}
    >
      <ul className={s.list} data-block="amp">
        {tone?.models.map((model) => {
          const progress = downloads[model.id];
          const installed = models.find((entry) => entry.name === model.name);
          const isCurrent = installed?.path === currentPath;
          const meta = [
            SIZE_LABELS[model.size] ?? model.size,
            ARCH_LABELS[model.architecture] ?? model.architecture,
          ].filter(Boolean);

          return (
            <li key={model.id} className={s.row}>
              <span className={s.info}>
                <span className={s.name} title={model.name}>
                  {model.name}
                </span>
                <span className={s.meta}>{meta.join(" · ")}</span>
              </span>

              <span className={s.action}>
                {progress !== undefined ? (
                  <span className={s.pct} role="status">
                    {Math.round(progress * 100)}%
                  </span>
                ) : isCurrent ? (
                  <span className={s.loaded}>
                    <CheckIcon size={14} />
                    Loaded
                  </span>
                ) : installed ? (
                  <Button size="sm" onClick={() => void loadModel(installed.path)}>
                    Load
                  </Button>
                ) : (
                  <Button size="sm" onClick={() => void t3kDownloadModel(model)}>
                    Download
                  </Button>
                )}
              </span>

              {progress !== undefined && (
                <span className={s.track} aria-hidden="true">
                  <span
                    className={s.fill}
                    style={{ transform: `scaleX(${Math.max(0.02, progress)})` }}
                  />
                </span>
              )}
            </li>
          );
        })}
      </ul>
    </Modal>
  );
}
