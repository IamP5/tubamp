import type { CSSProperties, ReactNode } from "react";
import s from "./kit.module.css";
import { cx } from "./cx";

/* ─────────────────────────────── Kbd chip ──────────────────────────────── */

export function Kbd({ children }: { children: ReactNode }) {
  return <kbd className={s.kbd}>{children}</kbd>;
}

/* ────────────────────────────── Empty state ────────────────────────────── */

export interface EmptyStateProps {
  icon?: ReactNode;
  message: string;
  hint?: string;
  action?: ReactNode;
  className?: string;
}

export function EmptyState({
  icon,
  message,
  hint,
  action,
  className,
}: EmptyStateProps) {
  return (
    <div className={cx(s.emptyState, className)}>
      {icon && <span className={s.emptyIcon}>{icon}</span>}
      <span className={s.emptyMessage}>{message}</span>
      {hint && <span className={s.emptyHint}>{hint}</span>}
      {action && <span className={s.emptyAction}>{action}</span>}
    </div>
  );
}

/* ──────────────────────────────── Loading ──────────────────────────────── */

export function Spinner({
  size = 16,
  className,
}: {
  size?: number;
  className?: string;
}) {
  const r = size / 2 - 1;
  const c = 2 * Math.PI * r;
  return (
    <svg
      className={cx(s.spinner, className)}
      width={size}
      height={size}
      viewBox={`0 0 ${size} ${size}`}
      role="status"
      aria-label="Loading"
    >
      {/* ~270° arc, matching the rotary knob's sweep. */}
      <circle
        cx={size / 2}
        cy={size / 2}
        r={r}
        fill="none"
        stroke="currentColor"
        strokeWidth={2}
        strokeLinecap="round"
        strokeDasharray={`${c * 0.75} ${c}`}
      />
    </svg>
  );
}

export interface SkeletonProps {
  width?: number | string;
  height?: number | string;
  radius?: string;
  className?: string;
  style?: CSSProperties;
}

export function Skeleton({
  width = "100%",
  height = 12,
  radius = "var(--radius-sm)",
  className,
  style,
}: SkeletonProps) {
  return (
    <div
      className={cx(s.skeleton, className)}
      style={{ width, height, borderRadius: radius, ...style }}
    />
  );
}
