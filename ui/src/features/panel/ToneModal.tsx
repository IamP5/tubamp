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
import {
  Button,
  DownloadIcon,
  Modal,
  CheckIcon,
} from "../../components";
import { useStore } from "../../store";
import { blockCssVars } from "../../theme/blocks";
import s from "./panel.module.css";

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
      title={tone ? `TONE3000 · tone ${tone.toneId}` : "TONE3000"}
      footer={
        <>
          <span className={s.footerNote}>Powered by TONE3000</span>
          <Button onClick={dismissTone}>Close</Button>
        </>
      }
    >
      <div className={s.toneList} style={blockCssVars("amp")} data-block="amp">
        {tone?.models.map((model) => {
          const progress = downloads[model.id];
          const installed = models.find((entry) => entry.name === model.name);
          const isCurrent = installed?.path === currentPath;

          return (
            <div key={model.id} className={s.toneRow}>
              <div className={s.toneInfo}>
                <span className={s.toneName} title={model.name}>
                  {model.name}
                </span>
                <span className={s.meta}>
                  {model.size} · {model.architecture}
                </span>
              </div>

              {progress !== undefined ? (
                <div className={s.toneProgress}>
                  <div className={s.progressTrack}>
                    <div
                      className={s.progressFill}
                      style={{ width: `${Math.round(progress * 100)}%` }}
                    />
                  </div>
                  <span className={s.readout}>
                    {Math.round(progress * 100)}%
                  </span>
                </div>
              ) : installed ? (
                <Button
                  size="sm"
                  variant={isCurrent ? "ghost" : "default"}
                  disabled={isCurrent}
                  icon={isCurrent ? <CheckIcon size={14} /> : undefined}
                  onClick={() => void loadModel(installed.path)}
                >
                  {isCurrent ? "Loaded" : "Load"}
                </Button>
              ) : (
                <Button
                  size="sm"
                  icon={<DownloadIcon size={14} />}
                  onClick={() => void t3kDownloadModel(model)}
                >
                  Download
                </Button>
              )}
            </div>
          );
        })}
      </div>
    </Modal>
  );
}
