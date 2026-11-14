import { useEffect, useRef, type ReactNode } from "react";
import { createPortal } from "react-dom";
import { AnimatePresence, motion } from "motion/react";
import s from "./kit.module.css";
import { cx } from "./cx";
import { duration, ease } from "../theme/motion";
import { IconButton } from "./Button";
import { CloseIcon } from "./icons";
import { useBlockingOverlay } from "./overlay";

export interface ModalProps {
  open: boolean;
  onClose(): void;
  title?: ReactNode;
  children: ReactNode;
  /** Right-aligned button row. */
  footer?: ReactNode;
  width?: number;
  className?: string;
}

/** Holds the blocking-overlay registration for exactly as long as it is mounted —
 *  which, inside AnimatePresence, is until the exit animation has finished. */
function BlockingWhileMounted() {
  useBlockingOverlay(true);
  return null;
}

/** Centred dialog: `surface-2`, radius 12, float shadow; fades in with an 8px rise. */
export function Modal({
  open,
  onClose,
  title,
  children,
  footer,
  width,
  className,
}: ModalProps) {
  const panelRef = useRef<HTMLDivElement>(null);

  /* The backdrop is animated (so it cannot be a <Scrim>), but it covers the whole
     page just the same — declare it. Registered from inside the animated subtree
     rather than on `open`, because an embedded plugin is a native view drawn above
     the page: unhiding it the instant `open` flips false pops it back over a
     backdrop that is still fading, and the modal appears to vanish behind it. */

  useEffect(() => {
    if (!open) return;
    const onKey = (e: KeyboardEvent): void => {
      if (e.key === "Escape") onClose();
    };
    window.addEventListener("keydown", onKey);
    panelRef.current?.focus();
    return () => window.removeEventListener("keydown", onKey);
  }, [open, onClose]);

  return createPortal(
    <AnimatePresence>
      {open && (
        <motion.div
          className={s.modalBackdrop}
          initial={{ opacity: 0 }}
          animate={{ opacity: 1 }}
          exit={{ opacity: 0 }}
          transition={{ duration: duration.slow, ease: ease.standard }}
          onPointerDown={(e) => {
            if (e.target === e.currentTarget) onClose();
          }}
        >
          <BlockingWhileMounted />
          <motion.div
            ref={panelRef}
            role="dialog"
            aria-modal="true"
            tabIndex={-1}
            className={cx(s.modalPanel, className)}
            style={width ? { width } : undefined}
            initial={{ opacity: 0, y: 8 }}
            animate={{ opacity: 1, y: 0 }}
            exit={{ opacity: 0, y: 8 }}
            transition={{ duration: duration.slow, ease: ease.standard }}
          >
            {title !== undefined && (
              <header className={s.modalHeader}>
                <span className={s.modalTitle}>{title}</span>
                <IconButton aria-label="Close" size="sm" onClick={onClose}>
                  <CloseIcon />
                </IconButton>
              </header>
            )}
            <div className={s.modalBody}>{children}</div>
            {footer && <footer className={s.modalFooter}>{footer}</footer>}
          </motion.div>
        </motion.div>
      )}
    </AnimatePresence>,
    document.body,
  );
}
