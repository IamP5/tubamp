import { useEffect } from "react";
import { AnimatePresence, motion } from "motion/react";
import s from "./kit.module.css";
import { cx } from "./cx";
import { duration, ease } from "../theme/motion";
import { useStore, type Toast as ToastModel, type ToastKind } from "../store";
import { useEmbedHole } from "./overlay";
import { AlertIcon, CheckCircleIcon, InfoIcon, WarningIcon } from "./icons";

/** Matches .toastStack's resting inset in kit.module.css. */
const TOAST_GUTTER = 16;
/** Narrowest a toast may get before stepping around the hole beats squeezing. */
const TOAST_MIN_W = 240;

const KIND_CLASS: Record<ToastKind, string> = {
  info: s.toastIconInfo,
  success: s.toastIconOk,
  warning: s.toastIconWarn,
  error: s.toastIconDanger,
};

function KindIcon({ kind }: { kind: ToastKind }) {
  if (kind === "success") return <CheckCircleIcon />;
  if (kind === "warning") return <WarningIcon />;
  if (kind === "error") return <AlertIcon />;
  return <InfoIcon />;
}

function ToastRow({ toast }: { toast: ToastModel }) {
  const dismiss = useStore((st) => st.dismissToast);

  useEffect(() => {
    if (toast.duration <= 0) return;
    const t = window.setTimeout(() => dismiss(toast.id), toast.duration);
    return () => window.clearTimeout(t);
  }, [toast.id, toast.duration, dismiss]);

  return (
    <motion.div
      layout
      className={s.toast}
      initial={{ opacity: 0, y: 8 }}
      animate={{ opacity: 1, y: 0 }}
      exit={{ opacity: 0, y: 8 }}
      transition={{ duration: duration.slow, ease: ease.standard }}
      onClick={() => dismiss(toast.id)}
      role="status"
    >
      <span className={cx(s.toastIcon, KIND_CLASS[toast.kind])}>
        <KindIcon kind={toast.kind} />
      </span>
      <span className={s.toastMessage}>{toast.message}</span>
      {toast.action && (
        <button
          type="button"
          className={cx(s.button, s.buttonGhost, s.buttonSm, s.toastAction)}
          onClick={(e) => {
            e.stopPropagation();
            toast.action?.onSelect();
            dismiss(toast.id);
          }}
        >
          {toast.action.label}
        </button>
      )}
    </motion.div>
  );
}

/**
 * Bottom-right toast stack. Mount once, near the app root.
 *
 * Hole-aware: an embedded plugin editor is a native view that composites above
 * the page, so a toast inside its rectangle is not merely overlapped, it is
 * invisible — and toasts are where every error is reported, including the ones
 * about embedding. Rather than hide the plugin for each transient message, the
 * stack steps left of the hole when it would otherwise land inside it.
 */
export function Toaster() {
  const toasts = useStore((st) => st.toasts);
  const hole = useEmbedHole();

  /* Only the horizontal axis: the stack is already anchored above the dock, so
     the overlap that actually happens is a wide plugin reaching the right edge. */
  /* Undefined when there is nothing to step around, so the stylesheet's own
     inset applies — which is what moves the stack left of the TONE3000 drawer
     (an inline `right` here would override it). */
  const clearRight =
    hole && hole.x + hole.width > window.innerWidth - TOAST_GUTTER - TOAST_MIN_W
      ? Math.max(TOAST_GUTTER, window.innerWidth - hole.x + TOAST_GUTTER)
      : undefined;

  return (
    <div className={s.toastStack} style={clearRight === undefined ? undefined : { right: clearRight }}>
      <AnimatePresence initial={false}>
        {toasts.map((t) => (
          <ToastRow key={t.id} toast={t} />
        ))}
      </AnimatePresence>
    </div>
  );
}
