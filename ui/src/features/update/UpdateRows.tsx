/**
 * The Settings sheet's update rows: the running version with the state in
 * plain words and the one action that state allows, then the automatic-check
 * switch. A pure function of the store's snapshot; every action is a command.
 */
import { useEffect } from "react";
import { Button, Toggle, cx } from "../../components";
import { useStore } from "../../store";
import { SYSTEM_COPY_TEXT, newerVersion, updateStatus } from "./words";
import s from "./update.module.css";

export function UpdateRows({ open }: { open: boolean }) {
  const view = useStore((st) => st.update);
  const updateCommand = useStore((st) => st.updateCommand);
  const markUpdateSeen = useStore((st) => st.markUpdateSeen);

  const newer = newerVersion(view);
  useEffect(() => {
    if (open && newer) markUpdateSeen(newer);
  }, [open, newer, markUpdateSeen]);

  if (!view.running) return null;
  const status = updateStatus(view);
  const off = view.phase.kind === "off";

  return (
    <>
      <div className={s.version} data-update-phase={view.phase.kind}>
        <span className={s.label}>Version</span>
        <div className={s.value}>
          <span className={cx(s.status, status.problem && s.problem)}>
            <span className={s.running}>{view.running}</span>
            {status.text && ` · ${status.text}`}
          </span>
          {status.whatsNew && (
            <button
              type="button"
              className={s.link}
              onClick={() => void updateCommand({ type: "openReleasePage" })}
            >
              What's new
            </button>
          )}
          {status.detail && <span className={s.note}>{status.detail}</span>}
          {view.systemCopy && <span className={s.note}>{SYSTEM_COPY_TEXT}</span>}
        </div>
        {status.action && (
          <Button
            size="sm"
            variant={view.phase.kind === "available" ? "primary" : "default"}
            onClick={() => status.action && void updateCommand(status.action.command)}
          >
            {status.action.label}
          </Button>
        )}
        {status.progress !== null && (
          <span className={s.track} aria-hidden>
            <span className={s.bar} style={{ transform: `scaleX(${status.progress})` }} />
          </span>
        )}
      </div>

      {!off && (
        <div className={s.auto}>
          <span className={s.label}>Check for updates automatically</span>
          <Toggle
            checked={view.autoCheck}
            aria-label="Check for updates automatically"
            onChange={(enabled) => void updateCommand({ type: "setAutoCheck", enabled })}
          />
        </div>
      )}
    </>
  );
}
