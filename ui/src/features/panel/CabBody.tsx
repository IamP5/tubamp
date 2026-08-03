/**
 * Cab body — impulse-response management above the LO CUT / HI CUT knob row
 * (current-ui-inventory.md §CabBody).
 *
 * The native CabBody swallowed every failure: the IR combo did not check the
 * load error at all, and import reported neither an import nor a load failure.
 * Both routes go through the store here, which toasts the error — the
 * inconsistency the inventory doc flags is fixed, not reproduced.
 */
import { Button, PlusIcon, Tooltip } from "../../components";
import { useStore } from "../../store";
import { FileRow } from "./FileRow";
import { KnobRow } from "./KnobRow";
import { KNOB_SPECS } from "./knobSpecs";
import s from "./panel.module.css";

export function CabBody() {
  const ir = useStore((st) => st.ir);
  const irs = useStore((st) => st.irs);
  const loadIr = useStore((st) => st.loadIr);
  const clearIr = useStore((st) => st.clearIr);
  const importIr = useStore((st) => st.importIr);
  const toast = useStore((st) => st.toast);

  return (
    <div className={s.stackBody}>
      <section className={s.section}>
        <span className={s.caption}>Impulse response</span>
        <FileRow
          entries={irs}
          currentPath={ir?.path ?? null}
          placeholder="No IR loaded"
          emptyLabel="No impulse responses in the library"
          onSelect={(path) => void loadIr(path)}
          onClear={() => void clearIr()}
          onEmptyStep={() =>
            toast("No impulse responses in the library yet — import one.", "warning")
          }
          labels={{
            prev: "Previous IR",
            next: "Next IR",
            clear: "Clear the loaded IR",
          }}
          trailing={
            <Tooltip label="Import an impulse response (.wav)">
              <Button
                size="sm"
                icon={<PlusIcon size={14} />}
                onClick={() => void importIr()}
              >
                Import
              </Button>
            </Tooltip>
          }
        />
      </section>

      <KnobRow specs={KNOB_SPECS.cab} />
    </div>
  );
}
