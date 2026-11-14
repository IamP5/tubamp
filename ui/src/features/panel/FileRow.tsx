/**
 * The media card's library row: ‹ [Captures ▾] › ✕ — the installed files in a
 * menu (the current one checked), previous / next to step through them without
 * opening anything, and a ghost button to empty the slot.
 *
 * The trigger names the library, not the file: the card's title already says
 * what is loaded, and saying it twice is what this row used to do.
 */
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
import m from "./media.module.css";

export interface FileRowProps {
  entries: readonly FileEntry[];
  currentPath: string | null;
  /** The trigger's text, and the noun the tooltips use: "Captures", "IRs". */
  library: string;
  /** Menu text when the library itself is empty. */
  emptyLabel: string;
  onSelect(path: string): void;
  onClear(): void;
  /** Stepping an empty library is a no-op natively; we surface it instead. */
  onEmptyStep(): void;
  labels: { menu: string; prev: string; next: string; clear: string };
}

export function FileRow({
  entries,
  currentPath,
  library,
  emptyLabel,
  onSelect,
  onClear,
  onEmptyStep,
  labels,
}: FileRowProps) {
  const { ref: anchorRef, anchor, open, toggle, close } = useMenu();
  const currentIndex = indexOfPath(entries, currentPath);

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
    <div className={m.files}>
      <Tooltip label={labels.prev}>
        <IconButton aria-label={labels.prev} onClick={() => step(-1)}>
          <ChevronLeftIcon size={14} />
        </IconButton>
      </Tooltip>
      <button
        ref={anchorRef}
        type="button"
        className={m.select}
        onClick={toggle}
        aria-label={labels.menu}
        aria-haspopup="menu"
        aria-expanded={open}
      >
        <span className={m.selectValue}>{library}</span>
        <ChevronDownIcon size={14} />
      </button>
      <Menu anchor={anchor} open={open} onClose={close} entries={menuEntries} minWidth={240} />
      <Tooltip label={labels.next}>
        <IconButton aria-label={labels.next} onClick={() => step(1)}>
          <ChevronRightIcon size={14} />
        </IconButton>
      </Tooltip>
      <Tooltip label={labels.clear}>
        <IconButton aria-label={labels.clear} disabled={currentPath === null} onClick={onClear}>
          <CloseIcon size={14} />
        </IconButton>
      </Tooltip>
    </div>
  );
}
