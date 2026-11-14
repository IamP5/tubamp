import { useLayoutEffect, useRef, useState } from "react";
import { motion, useTransform } from "motion/react";
import s from "./kit.module.css";
import { cx } from "./cx";
import { METER_MAX_DB, METER_MIN_DB, type MeterChannel } from "../hooks/useMeters";

/** Ladder positions (0..1) of the colour thresholds: hot above −6 dB, clip at 0 dBFS. */
const HOT_POS = (-6 - METER_MIN_DB) / (METER_MAX_DB - METER_MIN_DB);
const CLIP_POS = (0 - METER_MIN_DB) / (METER_MAX_DB - METER_MIN_DB);

function meterColour(position: number): string {
  if (position >= CLIP_POS) return "var(--ds-meter-clip)";
  if (position >= HOT_POS) return "var(--ds-meter-hot)";
  return "var(--ds-meter)";
}

export interface MeterProps {
  channel: MeterChannel;
  /** Vertical (default) fills bottom-up; horizontal fills left-to-right. */
  orientation?: "vertical" | "horizontal";
  /** Accepted for old call sites; the bar is continuous, not segmented. */
  segments?: number;
  /** Show the 1px peak-hold tick. */
  hold?: boolean;
  className?: string;
  "aria-label"?: string;
}

/**
 * A 4px peak bar: track `meter-track`, fill `meter`, `meter-hot` above −6 dB,
 * `meter-clip` at clip, a 1px peak tick. Driven entirely by MotionValues (the
 * fill is a scale transform), so a meter never re-renders React per frame.
 */
export function Meter({
  channel,
  orientation = "vertical",
  hold = true,
  className,
  ...rest
}: MeterProps) {
  const vertical = orientation === "vertical";
  const ref = useRef<HTMLDivElement>(null);
  const [extent, setExtent] = useState(0);

  useLayoutEffect(() => {
    const el = ref.current;
    if (!el) return;
    const measure = (): void => {
      const r = el.getBoundingClientRect();
      setExtent(vertical ? r.height : r.width);
    };
    measure();
    const observer = new ResizeObserver(measure);
    observer.observe(el);
    return () => observer.disconnect();
  }, [vertical]);

  const fill = useTransform(channel.level, (v) => Math.min(1, Math.max(0, v)));
  const fillColour = useTransform(channel.level, meterColour);

  /* The tick sits at the top (vertical) / right (horizontal) edge of the held
     level, 1px thick, coloured by the level it holds. */
  const holdOffset = useTransform(channel.hold, (h) =>
    vertical ? Math.round((1 - h) * (extent - 1)) : Math.round(h * (extent - 1)),
  );
  const holdOpacity = useTransform(channel.hold, (h) => (h > 0.001 ? 1 : 0));
  const holdColour = useTransform(channel.hold, meterColour);

  return (
    <div
      ref={ref}
      className={cx(s.meter, vertical ? s.meterVertical : s.meterHorizontal, className)}
      role="meter"
      {...rest}
    >
      <motion.span
        className={s.meterFill}
        style={
          vertical
            ? { scaleY: fill, backgroundColor: fillColour }
            : { scaleX: fill, backgroundColor: fillColour }
        }
      />
      {hold && (
        <motion.span
          className={s.meterPeak}
          style={{
            backgroundColor: holdColour,
            opacity: holdOpacity,
            ...(vertical ? { y: holdOffset } : { x: holdOffset }),
          }}
        />
      )}
    </div>
  );
}
