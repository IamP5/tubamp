/**
 * The panel header's trash for SPLIT / MIX: a region has three ways out, not
 * one, so the trash opens the same "Remove split" menu the cards' kebabs show
 * (keep lane A, keep lane B, chain A → B — see features/board/splitActions).
 */
import { IconButton, Menu, Tooltip, TrashIcon, useMenu } from "../../components";
import { useStore } from "../../store";
import { useLaneNames } from "../board/lanes";
import { splitRemoveEntries } from "../board/splitActions";

export function SplitRemoveButton() {
  const order = useStore((st) => st.chainOrder);
  const { names } = useLaneNames();
  const { ref, anchor, open, toggle, close } = useMenu<HTMLSpanElement>();
  return (
    <>
      <Tooltip label="Remove the split…">
        <span ref={ref} style={{ display: "inline-flex" }}>
          <IconButton
            aria-label="Remove the split"
            aria-haspopup="menu"
            aria-expanded={open}
            destructive
            onClick={toggle}
          >
            <TrashIcon />
          </IconButton>
        </span>
      </Tooltip>
      <Menu
        anchor={anchor}
        open={open}
        onClose={close}
        entries={splitRemoveEntries(order, names)}
        align="end"
        minWidth={230}
      />
    </>
  );
}
