import { useEffect } from "react";
import { AnimatePresence, motion } from "motion/react";
import s from "./kit.module.css";
import { duration, ease } from "../theme/motion";
import { useStore, type Toast as ToastModel, type ToastKind } from "../store";
import { useEmbedHole } from "./overlay";

/** Matches .toastStack's resting inset in kit.module.css. */
const TOAST_GUTTER = 16;
/** Narrowest a toast may get before stepping around the hole beats squeezing. */
const TOAST_MIN_W = 240;
import { AlertIcon, CheckCircleIcon, InfoIcon } from "./icons";

const KIND_COLOUR: Record<ToastKind, string> = {
  info: "var(--text-secondary)",
  success: "var(--success)",
  warning: "var(--warning)",
  error: "var(--error)",
};

function KindIcon({ kind }: { kind: ToastKind }) {
  const style = { color: KIND_COLOUR[kind] };
  if (kind === "success") return <CheckCircleIcon style={style} />;
  if (kind === "info") return <InfoIcon style={style} />;
  return <AlertIcon style={style} />;
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
      exit={{ opacity: 0, transition: { duration: duration.instant } }}
      transition={{ duration: duration.fast, ease: ease.out }}
      onClick={() => dismiss(toast.id)}
      role="status"
    >
      <span className={s.toastIcon}>
        <KindIcon kind={toast.kind} />
      </span>
      <span className={s.toastMessage}>{toast.message}</span>
      {toast.duration > 0 && (
        <motion.span
          className={s.toastProgress}
          style={{ background: KIND_COLOUR[toast.kind] }}
          initial={{ scaleX: 1 }}
          animate={{ scaleX: 0 }}
          transition={{ duration: toast.duration / 1000, ease: "linear" }}
        />
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
  const clearRight =
    hole && hole.x + hole.width > window.innerWidth - TOAST_GUTTER - TOAST_MIN_W
      ? Math.max(TOAST_GUTTER, window.innerWidth - hole.x + TOAST_GUTTER)
      : TOAST_GUTTER;

  return (
    <div className={s.toastStack} style={{ right: clearRight }}>
      <AnimatePresence initial={false}>
        {toasts.map((t) => (
          <ToastRow key={t.id} toast={t} />
        ))}
      </AnimatePresence>
    </div>
  );
}
