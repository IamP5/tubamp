import { motion } from "motion/react";
import s from "./kit.module.css";
import { cx } from "./cx";
import { spring, tween } from "../theme/motion";

export interface ToggleProps {
  checked: boolean;
  onChange(next: boolean): void;
  disabled?: boolean;
  "aria-label": string;
  className?: string;
}

/** Small switch — settings rows, boolean params that aren't block power. */
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
        className={s.switchKnob}
        animate={{ x: checked ? 14 : 0 }}
        transition={spring.micro}
      />
    </button>
  );
}

export interface PowerPillProps {
  on: boolean;
  onChange(next: boolean): void;
  disabled?: boolean;
  /** Hide the ON/OFF caption, leaving just the LED (compact placements). */
  ledOnly?: boolean;
  title?: string;
  className?: string;
}

/**
 * The block power control: LED + ON/OFF caption, matching the native
 * `makePowerPill`. Colour comes from the nearest `[data-block]` ancestor.
 * The LED state flip is `instant` — bypass gets clicked constantly during A/B
 * and must never feel laggy.
 */
export function PowerPill({
  on,
  onChange,
  disabled,
  ledOnly,
  title,
  className,
}: PowerPillProps) {
  return (
    <motion.button
      type="button"
      role="switch"
      aria-checked={on}
      aria-label={on ? "Bypass this block" : "Enable this block"}
      title={title}
      disabled={disabled}
      onClick={(e) => {
        e.stopPropagation();
        onChange(!on);
      }}
      className={cx(s.powerPill, on && s.powerPillOn, className)}
      transition={tween.instant}
    >
      <span className={s.powerLed} />
      {!ledOnly && <span>{on ? "ON" : "OFF"}</span>}
    </motion.button>
  );
}
