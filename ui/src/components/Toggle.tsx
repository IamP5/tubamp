import type { ButtonHTMLAttributes } from "react";
import { motion } from "motion/react";
import s from "./kit.module.css";
import { cx } from "./cx";
import { tween } from "../theme/motion";
import { PowerIcon } from "./icons";

export interface ToggleProps {
  checked: boolean;
  onChange(next: boolean): void;
  disabled?: boolean;
  "aria-label": string;
  className?: string;
}

/** Switch, 28x16; on is accent. Settings rows and boolean parameters. */
export function Toggle({
  checked,
  onChange,
  disabled,
  className,
  ...rest
}: ToggleProps) {
  return (
    <button
      type="button"
      role="switch"
      aria-checked={checked}
      disabled={disabled}
      onClick={() => onChange(!checked)}
      className={cx(s.switch, checked && s.switchOn, className)}
      {...rest}
    >
      <motion.span
        className={s.switchThumb}
        initial={false}
        animate={{ x: checked ? 12 : 0 }}
        transition={tween.micro}
      />
    </button>
  );
}

export interface PowerButtonProps
  extends Omit<ButtonHTMLAttributes<HTMLButtonElement>, "onChange" | "children"> {
  on: boolean;
  onChange(next: boolean): void;
  "aria-label": string;
  /** 24 or 28 square (default 28). */
  size?: "sm" | "md";
}

/**
 * Block bypass: a power icon button. On is primary text, off is disabled text.
 * The click does not bubble, so it can sit inside a clickable card.
 */
export function PowerButton({
  on,
  onChange,
  onClick,
  size = "md",
  className,
  ...rest
}: PowerButtonProps) {
  return (
    <button
      type="button"
      role="switch"
      aria-checked={on}
      onClick={(e) => {
        e.stopPropagation();
        onClick?.(e);
        onChange(!on);
      }}
      className={cx(
        s.iconButton,
        size === "sm" && s.iconButtonSm,
        s.power,
        on && s.powerOn,
        className,
      )}
      {...rest}
    >
      <PowerIcon />
    </button>
  );
}

export interface PowerPillProps {
  on: boolean;
  onChange(next: boolean): void;
  disabled?: boolean;
  /** Accepted for old call sites; the power button has no caption. */
  ledOnly?: boolean;
  title?: string;
  className?: string;
}

/** Older name for the block power control: renders `PowerButton`. */
export function PowerPill({ on, onChange, disabled, title, className }: PowerPillProps) {
  return (
    <PowerButton
      on={on}
      onChange={onChange}
      disabled={disabled}
      title={title}
      className={className}
      aria-label={on ? "Bypass this block" : "Enable this block"}
    />
  );
}
