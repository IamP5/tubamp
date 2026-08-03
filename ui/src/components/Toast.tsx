import { useEffect } from "react";
import { AnimatePresence, motion } from "motion/react";
import s from "./kit.module.css";
import { duration, ease } from "../theme/motion";
import { useStore, type Toast as ToastModel, type ToastKind } from "../store";
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

/** Bottom-right toast stack. Mount once, near the app root. */
export function Toaster() {
  const toasts = useStore((st) => st.toasts);
  return (
    <div className={s.toastStack}>
      <AnimatePresence initial={false}>
        {toasts.map((t) => (
          <ToastRow key={t.id} toast={t} />
        ))}
      </AnimatePresence>
    </div>
  );
}
