/**
 * The library picker shared by the amp model row and the cab IR row: a menu of
 * the installed files plus prev / next / clear affordances.
 *
 * The native combo repopulated itself behind an `updatingCombo` reentrancy flag;
 * here the list is just derived from the store, so there is nothing to guard.
 */
import type { ReactNode } from "react";
import {
  ChevronDownIcon,
  ChevronLeftIcon,
  ChevronRightIcon,
  CloseIcon,
  IconButton,
  Menu,
  Tooltip,
  useMenu,
  type MenuEntry,
} from "../../components";
import type { FileEntry } from "../../bridge";
import { indexOfPath, stepIndex } from "./library";
import s from "./panel.module.css";

export interface FileRowProps {
  entries: readonly FileEntry[];
  currentPath: string | null;
  /** Trigger text when nothing is loaded ("Select a model…", "No IR loaded"). */
  placeholder: string;
  /** Menu text when the library itself is empty. */
  emptyLabel: string;
  onSelect(path: string): void;
  onClear(): void;
  /** Stepping an empty library is a no-op natively; we surface it instead. */
  onEmptyStep(): void;
  labels: { prev: string; next: string; clear: string };
  /** Import button (and anything else that belongs on the row). */
  trailing?: ReactNode;
}

export function FileRow({
  entries,
  currentPath,
  placeholder,
  emptyLabel,
  onSelect,
  onClear,
  onEmptyStep,
  labels,
  trailing,
}: FileRowProps) {
  const { ref: anchorRef, anchor, open, toggle, close } = useMenu();
  const currentIndex = indexOfPath(entries, currentPath);
  const currentName = currentIndex >= 0 ? entries[currentIndex].name : null;

  const step = (delta: number): void => {
    const next = stepIndex(currentIndex, delta, entries.length);
    if (next === null) {
      onEmptyStep();
      return;
    }
    onSelect(entries[next].path);
  };

  const menuEntries: MenuEntry[] =
    entries.length === 0
      ? [{ id: "empty", label: emptyLabel, disabled: true }]
      : entries.map((entry) => ({
          id: entry.path,
          label: entry.name,
          selected: entry.path === currentPath,
          onSelect: () => onSelect(entry.path),
        }));

  return (
    <div className={s.fileRow}>
      <button
        ref={anchorRef}
        type="button"
        className={s.select}
        onClick={toggle}
        aria-haspopup="menu"
        aria-expanded={open}
      >
        <span className={currentName ? s.selectValue : s.selectPlaceholder}>
          {currentName ?? placeholder}
        </span>
        <ChevronDownIcon size={14} />
      </button>
      <Menu
        anchor={anchor}
        open={open}
        onClose={close}
        entries={menuEntries}
        minWidth={240}
      />
      <Tooltip label={labels.prev}>
        <IconButton aria-label={labels.prev} plate onClick={() => step(-1)}>
          <ChevronLeftIcon size={14} />
        </IconButton>
      </Tooltip>
      <Tooltip label={labels.next}>
        <IconButton aria-label={labels.next} plate onClick={() => step(1)}>
          <ChevronRightIcon size={14} />
        </IconButton>
      </Tooltip>
      <Tooltip label={labels.clear}>
        <IconButton
          aria-label={labels.clear}
          plate
          disabled={currentIndex < 0 && currentPath === null}
          onClick={onClear}
        >
          <CloseIcon size={14} />
        </IconButton>
      </Tooltip>
      {trailing}
    </div>
  );
}
