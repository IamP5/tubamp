/**
 * Settings sheet — docs/REACT-UI.md §Settings sheet.
 *
 * A plain modal: rows of label, value and action. The TONE3000 publishable key
 * is entered through the native prompt (`t3kConfigure`), never a web text
 * field: Logic's WebView gets no reliable keyboard. Amp calibration lives in
 * the amp block's editor and is deliberately not repeated here.
 */
import { Button, Modal } from "../../components";
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

  const account = t3k.authenticated ? (t3k.username ?? "Signed in") : "Signed out";

  return (
    <Modal open={open} onClose={onClose} title="Settings" width={400}>
      <div className={s.rows}>
        <div className={s.row}>
          <span className={s.label}>TONE3000 key</span>
          <span className={s.value}>{t3k.configured ? "Configured" : "Not set"}</span>
          <Button size="sm" onClick={() => void t3kConfigure()}>
            {t3k.configured ? "Change key" : "Set key"}
          </Button>
        </div>

        <div className={s.row}>
          <span className={s.label}>Account</span>
          <span className={s.value}>{account}</span>
          <Button
            size="sm"
            variant="ghost"
            disabled={!t3k.configured}
            onClick={() => void t3kSignOut()}
          >
            Sign out
          </Button>
        </div>
      </div>

      <p className={s.attribution}>Powered by TONE3000</p>
    </Modal>
  );
}
