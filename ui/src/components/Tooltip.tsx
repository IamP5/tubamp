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
import { embedHole } from "./overlay";
import { duration, ease } from "../theme/motion";

const SHOW_DELAY_MS = 400;

/** Gap between trigger and tip, and the tip's own height (4+11×1.4+4, rounded). */
const TIP_GAP = 8;
const TIP_H = 24;

type Placement = "top" | "bottom";

/**
 * Flip away from the embedded plugin's rectangle when the preferred side would
 * put the tip inside it. That rectangle is a native view drawn ABOVE the page
 * (./overlay.tsx): a tooltip landing there is not dimmed or clipped, it is
 * simply invisible. The tip's width is unknown before it renders, so the test
 * uses a generous fixed half-width — erring towards flipping is free, erring the
 * other way loses the tooltip.
 */
function placementFor(anchor: DOMRect, preferred: Placement): Placement | null {
  const hole = embedHole();
  if (!hole) return preferred;

  const centre = anchor.left + anchor.width / 2;
  const left = centre - 120;
  const right = centre + 120;
  if (left >= hole.x + hole.width || right <= hole.x) return preferred;

  const hits = (side: Placement): boolean => {
    const top =
      side === "top" ? anchor.top - TIP_GAP - TIP_H : anchor.bottom + TIP_GAP;
    return top < hole.y + hole.height && top + TIP_H > hole.y;
  };
  const other: Placement = preferred === "top" ? "bottom" : "top";
  if (!hits(preferred)) return preferred;
  if (!hits(other)) return other;
  /* Both sides land in the hole — a band-tall plugin with the trigger beside it.
     Rendering anyway puts the tip behind the native view, where it is not dimmed
     or clipped but simply absent, and the user is left hovering at nothing. A
     missing tooltip is the better failure. */
  return null;
}

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
  const [pos, setPos] = useState<{
    top: number;
    left: number;
    side: Placement;
  } | null>(null);

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
        const side = placementFor(r, placement);
        // null means every placement lands inside the embedded plugin's rectangle,
        // where nothing the page draws is visible. Stay closed.
        if (side === null) return;
        setPos({
          top: side === "top" ? r.top - TIP_GAP : r.bottom + TIP_GAP,
          left: r.left + r.width / 2,
          side,
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
                transform: `translate(-50%, ${pos.side === "top" ? "-100%" : "0"})`,
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
