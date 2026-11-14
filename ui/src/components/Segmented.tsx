import { useId } from "react";
import { motion } from "motion/react";
import s from "./kit.module.css";
import { cx } from "./cx";
import { tween } from "../theme/motion";

export interface SegmentedOption<T extends string> {
  value: T;
  label: string;
  /** Small neutral dot after the label: used for A/B "slot has state". */
  dot?: boolean;
  title?: string;
  disabled?: boolean;
}

export interface SegmentedProps<T extends string> {
  options: SegmentedOption<T>[];
  value: T;
  onChange(value: T, event: React.MouseEvent<HTMLButtonElement>): void;
  className?: string;
  "aria-label"?: string;
}

/**
 * Segmented control: a `surface-1` track with a hairline, the selected segment
 * `surface-3` with primary text, sliding between segments (`layoutId`,
 * transform-only). Used for A/B compare, where shift-click means "capture",
 * hence the raw event in `onChange`.
 */
export function Segmented<T extends string>({
  options,
  value,
  onChange,
  className,
  ...rest
}: SegmentedProps<T>) {
  const groupId = useId();

  return (
    <div className={cx(s.segmented, className)} role="tablist" {...rest}>
      {options.map((option) => {
        const active = option.value === value;
        return (
          <button
            key={option.value}
            type="button"
            role="tab"
            aria-selected={active}
            title={option.title}
            disabled={option.disabled}
            onClick={(e) => onChange(option.value, e)}
            className={cx(s.segment, active && s.segmentActive)}
          >
            {active && (
              <motion.span
                layoutId={`segmented-pill-${groupId}`}
                className={s.segmentPill}
                transition={tween.enter}
              />
            )}
            <span className={s.segmentLabel}>{option.label}</span>
            {option.dot && <span className={s.segmentDot} />}
          </button>
        );
      })}
    </div>
  );
}
