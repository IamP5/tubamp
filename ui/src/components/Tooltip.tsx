import {
  useCallback,
  useEffect,
  useRef,
  useState,
  type PointerEvent as ReactPointerEvent,
  type ReactNode,
} from "react";
import { createPortal } from "react-dom";
import { AnimatePresence, motion } from "motion/react";
import s from "./kit.module.css";
import { cx } from "./cx";
import { duration, ease } from "../theme/motion";

const SHOW_DELAY_MS = 400;

export interface TooltipProps {
  label: ReactNode;
  /** Wrapped in a tight inline-flex anchor that owns the hover handlers. */
  children: ReactNode;
  className?: string;
  placement?: "top" | "bottom";
  disabled?: boolean;
}

/**
 * 400ms show delay, 0ms hide — Geist behaviour. Never blocks pointer events.
 * The anchor rect is captured from the pointer event itself, so no ref plumbing
 * (and no ref reads during render) is involved.
 */
export function Tooltip({
  label,
  children,
  placement = "top",
  disabled,
  className,
}: TooltipProps) {
  const timer = useRef(0);
  const [pos, setPos] = useState<{ top: number; left: number } | null>(null);

  const hide = useCallback(() => {
    window.clearTimeout(timer.current);
    setPos(null);
  }, []);

  useEffect(() => () => window.clearTimeout(timer.current), []);

  const show = useCallback(
    (element: HTMLElement) => {
      if (disabled) return;
      window.clearTimeout(timer.current);
      timer.current = window.setTimeout(() => {
        const r = element.getBoundingClientRect();
        setPos({
          top: placement === "top" ? r.top - 8 : r.bottom + 8,
          left: r.left + r.width / 2,
        });
      }, SHOW_DELAY_MS);
    },
    [disabled, placement],
  );

  return (
    <>
      <span
        className={cx(s.tooltipAnchor, className)}
        onPointerEnter={(e: ReactPointerEvent<HTMLSpanElement>) =>
          show(e.currentTarget)
        }
        onPointerLeave={hide}
        onPointerDown={hide}
      >
        {children}
      </span>
      {createPortal(
        <AnimatePresence>
          {pos && (
            <motion.div
              role="tooltip"
              className={s.tooltip}
              style={{
                top: pos.top,
                left: pos.left,
                transform: `translate(-50%, ${placement === "top" ? "-100%" : "0"})`,
              }}
              initial={{ opacity: 0 }}
              animate={{ opacity: 1 }}
              exit={{ opacity: 0 }}
              transition={{ duration: duration.instant, ease: ease.out }}
            >
              {label}
            </motion.div>
          )}
        </AnimatePresence>,
        document.body,
      )}
    </>
  );
}
