/**
 * Settings sheet — docs/REACT-UI.md §Settings sheet.
 *
 * A plain modal: rows of label, value and action. The TONE3000 key is built
 * into the plugin, so the only TONE3000 row is the account. Amp calibration
 * lives in the amp block's editor and is deliberately not repeated here.
 */
import { Button, Modal } from "../../components";
import { useStore } from "../../store";
import { UpdateRows } from "../update/UpdateRows";
import s from "./settings.module.css";

export interface SettingsSheetProps {
  open: boolean;
  onClose(): void;
}

export function SettingsSheet({ open, onClose }: SettingsSheetProps) {
  const t3k = useStore((st) => st.t3k);
  const t3kSignOut = useStore((st) => st.t3kSignOut);

  const account = t3k.authenticated ? (t3k.username ?? "Signed in") : "Signed out";

  return (
    <Modal open={open} onClose={onClose} title="Settings" width={460}>
      <div className={s.rows}>
        <div className={s.row}>
          <span className={s.label}>TONE3000 account</span>
          <span className={s.value}>{account}</span>
          <Button
            size="sm"
            variant="ghost"
            disabled={!t3k.authenticated}
            onClick={() => void t3kSignOut()}
          >
            Sign out
          </Button>
        </div>

        <UpdateRows open={open} />
      </div>

      <p className={s.attribution}>Powered by TONE3000</p>
    </Modal>
  );
}
