import {
  useCallback,
  useEffect,
  useLayoutEffect,
  useRef,
  useState,
  type ReactNode,
} from "react";
import { createPortal } from "react-dom";
import { AnimatePresence, motion } from "motion/react";
import s from "./kit.module.css";
import { cx } from "./cx";
import { duration, ease } from "../theme/motion";

export interface MenuItem {
  id: string;
  label: string;
  icon?: ReactNode;
  /** Right-aligned hint, e.g. a shortcut or a value. */
  hint?: string;
  selected?: boolean;
  disabled?: boolean;
  destructive?: boolean;
  onSelect?(): void;
}

export type MenuEntry =
  | ({ kind?: "item" } & MenuItem)
  | { kind: "separator"; id: string }
  | { kind: "section"; id: string; label: string };

export interface MenuProps {
  /** Anchor element the menu positions itself against. */
  anchor: HTMLElement | null;
  open: boolean;
  onClose(): void;
  entries: MenuEntry[];
  /** Horizontal alignment relative to the anchor. */
  align?: "start" | "end" | "center";
  minWidth?: number;
}

/**
 * Vercel-style dropdown, portalled to `document.body` and positioned against an
 * anchor element. Mouse-first (the plugin WebView gets no reliable keyboard in
 * Logic) but Escape/arrow keys still work when focus is available.
 */
export function Menu({
  anchor,
  open,
  onClose,
  entries,
  align = "start",
  minWidth,
}: MenuProps) {
  const menuRef = useRef<HTMLDivElement>(null);
  const [pos, setPos] = useState({ top: 0, left: 0 });

  const place = useCallback(() => {
    if (!anchor || !menuRef.current) return;
    const a = anchor.getBoundingClientRect();
    const m = menuRef.current.getBoundingClientRect();
    let left = a.left;
    if (align === "end") left = a.right - m.width;
    if (align === "center") left = a.left + (a.width - m.width) / 2;
    let top = a.bottom + 6;
    if (top + m.height > window.innerHeight - 8) {
      top = Math.max(8, a.top - m.height - 6);
    }
    left = Math.min(Math.max(8, left), window.innerWidth - m.width - 8);
    setPos({ top, left });
  }, [anchor, align]);

  useLayoutEffect(() => {
    if (open) place();
  }, [open, place, entries.length]);

  useEffect(() => {
    if (!open) return;
    const onKey = (e: KeyboardEvent): void => {
      if (e.key === "Escape") onClose();
    };
    window.addEventListener("keydown", onKey);
    window.addEventListener("resize", place);
    return () => {
      window.removeEventListener("keydown", onKey);
      window.removeEventListener("resize", place);
    };
  }, [open, onClose, place]);

  return createPortal(
    <AnimatePresence>
      {open && (
        <>
          <div className={s.menuScrim} onPointerDown={onClose} />
          <motion.div
            ref={menuRef}
            role="menu"
            className={s.menu}
            style={{ top: pos.top, left: pos.left, minWidth }}
            initial={{ opacity: 0, y: -4 }}
            animate={{ opacity: 1, y: 0 }}
            exit={{ opacity: 0, y: -4 }}
            transition={{ duration: duration.instant, ease: ease.out }}
          >
            {entries.map((entry) => {
              if (entry.kind === "separator") {
                return <div key={entry.id} className={s.menuSeparator} />;
              }
              if (entry.kind === "section") {
                return (
                  <div key={entry.id} className={s.menuSectionLabel}>
                    {entry.label}
                  </div>
                );
              }
              return (
                <button
                  key={entry.id}
                  type="button"
                  role="menuitem"
                  disabled={entry.disabled}
                  className={cx(
                    s.menuItem,
                    entry.selected && s.menuItemSelected,
                    entry.destructive && s.menuItemDestructive,
                  )}
                  onClick={() => {
                    entry.onSelect?.();
                    onClose();
                  }}
                >
                  {entry.icon}
                  <span className={s.menuItemLabel}>{entry.label}</span>
                  {entry.hint && (
                    <span className={s.menuItemHint}>{entry.hint}</span>
                  )}
                </button>
              );
            })}
          </motion.div>
        </>
      )}
    </AnimatePresence>,
    document.body,
  );
}

/**
 * Convenience: anchor + open state for the common trigger-button case.
 *
 * `ref` is a callback ref that stores the element in state, so the anchor is
 * available during render (a plain ref's `.current` is not readable then).
 *
 *     const menu = useMenu();
 *     <Button ref={menu.ref} onClick={menu.toggle}>…</Button>
 *     <Menu anchor={menu.anchor} open={menu.open} onClose={menu.close} … />
 */
export function useMenu<T extends HTMLElement = HTMLButtonElement>() {
  const [anchor, setAnchor] = useState<T | null>(null);
  const [open, setOpen] = useState(false);
  return {
    ref: setAnchor,
    anchor,
    open,
    toggle: () => setOpen((v) => !v),
    show: () => setOpen(true),
    close: () => setOpen(false),
  };
}
