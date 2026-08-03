import { useEffect, useRef, type ReactNode } from "react";
import { createPortal } from "react-dom";
import { AnimatePresence, motion } from "motion/react";
import s from "./kit.module.css";
import { cx } from "./cx";
import { duration, ease, spring } from "../theme/motion";
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

/** Centered dialog: fast dismissal, slightly slower deliberate entrance. */
/** Holds the blocking-overlay registration for exactly as long as it is mounted —
 *  which, inside AnimatePresence, is until the exit animation has finished. */
function BlockingWhileMounted() {
  useBlockingOverlay(true);
  return null;
}

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
          transition={{ duration: duration.base, ease: ease.out }}
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
            initial={{ opacity: 0, scale: 0.96, y: 8 }}
            animate={{ opacity: 1, scale: 1, y: 0 }}
            exit={{
              opacity: 0,
              scale: 0.98,
              y: 4,
              transition: { duration: duration.fast, ease: ease.in },
            }}
            transition={spring.ui}
          >
            {title !== undefined && (
              <header className={s.modalHeader}>
                <span className={s.modalTitle}>{title}</span>
                <IconButton aria-label="Close" onClick={onClose}>
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
