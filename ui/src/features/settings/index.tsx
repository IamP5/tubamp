/**
 * Settings sheet — current-ui-inventory.md §SettingsPanel, docs/REACT-UI.md
 * §Settings sheet.
 *
 * TONE3000 publishable key is entered through the native AlertWindow prompt
 * (`t3kConfigure`) — never a web text field, per the bridge contract (Logic/AS
 * WebViews get no reliable keyboard). This sheet only ever shows *status* plus
 * the two action buttons.
 *
 * The native SettingsPanel also hosted amp calibration (`amp_cal_input` /
 * `amp_cal_level`). Those now live in the amp block's dock body, next to the
 * IN/OUT levels they calibrate (docs/REACT-UI.md §UX structure lists the
 * settings sheet as "t3k key via native prompt, sign out"). They are
 * deliberately NOT duplicated here — one owner per control.
 */
import { useCallback, useState } from "react";
import { Button, Modal, cx } from "../../components";
import { useStore } from "../../store";
import s from "./settings.module.css";

export interface SettingsSheetProps {
  open: boolean;
  onClose(): void;
}

export function SettingsSheet({ open, onClose }: SettingsSheetProps) {
  const t3k = useStore((st) => st.t3k);
  const t3kConfigure = useStore((st) => st.t3kConfigure);
  const t3kSignOut = useStore((st) => st.t3kSignOut);
  const [status, setStatus] = useState("");

  const handleConfigure = useCallback(async () => {
    setStatus("");
    await t3kConfigure();
  }, [t3kConfigure]);

  const handleSignOut = useCallback(async () => {
    await t3kSignOut();
    setStatus("Signed out.");
  }, [t3kSignOut]);

  return (
    <Modal open={open} onClose={onClose} title="Settings" width={380}>
      <div className={s.section}>
        <h3 className={s.sectionTitle}>TONE3000</h3>

        <div className={s.row}>
          <span className={s.label}>Publishable key</span>
          <span className={cx(s.status, t3k.configured && s.statusOk)}>
            {t3k.configured
              ? t3k.authenticated
                ? "Configured · signed in"
                : "Configured"
              : "Not configured"}
          </span>
        </div>

        <div className={s.buttonRow}>
          <Button onClick={() => void handleConfigure()}>
            {t3k.configured ? "Change key…" : "Set key…"}
          </Button>
          <Button variant="ghost" disabled={!t3k.configured} onClick={() => void handleSignOut()}>
            Sign out
          </Button>
        </div>

        <p className={s.statusLine}>{status}</p>
        <p className={s.attribution}>Powered by TONE3000</p>
      </div>

      <p className={s.footnote}>
        Amp calibration (input calibration and level) lives in the AMP block’s
        panel.
      </p>
    </Modal>
  );
}
