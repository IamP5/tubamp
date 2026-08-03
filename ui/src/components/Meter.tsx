import { useLayoutEffect, useRef, useState } from "react";
import { motion, useTransform } from "motion/react";
import s from "./kit.module.css";
import { cx } from "./cx";
import {
  METER_HIGH_DB,
  METER_MAX_DB,
  METER_MID_DB,
  METER_MIN_DB,
  METER_SEGMENTS,
  type MeterChannel,
} from "../hooks/useMeters";

/** Segment colour comes from the segment's OWN centre dB, not the live level. */
function segmentColour(index: number, segments: number): string {
  const centreDb =
    METER_MIN_DB +
    (METER_MAX_DB - METER_MIN_DB) * ((index + 0.5) / segments);
  if (centreDb >= METER_HIGH_DB) return "var(--meter-high)";
  if (centreDb >= METER_MID_DB) return "var(--meter-mid)";
  return "var(--meter-low)";
}

export interface MeterProps {
  channel: MeterChannel;
  orientation?: "vertical" | "horizontal";
  segments?: number;
  /** Show the peak-hold tick (footer meters do; inline indicators may not). */
  hold?: boolean;
  className?: string;
  "aria-label"?: string;
}

/**
 * Segmented peak meter driven entirely by MotionValues — no React re-render per
 * frame. Unlit segments show their own colour at 10% alpha over the track, lit
 * ones fade to full: one opacity transform per segment, all fed by the shared
 * rAF driver through `useMeters()`.
 */
export function Meter({
  channel,
  orientation = "vertical",
  segments = METER_SEGMENTS,
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
    window.addEventListener("resize", measure);
    return () => window.removeEventListener("resize", measure);
  }, [vertical]);

  const holdOffset = useTransform(channel.hold, (h) =>
    vertical ? (1 - h) * extent : h * extent,
  );
  const holdOpacity = useTransform(channel.hold, (h) => (h > 0.001 ? 0.9 : 0));
  /* Unlike the ladder, the hold tick is coloured by the ACTUAL held level. */
  const holdColour = useTransform(channel.hold, (h) => {
    const db = METER_MIN_DB + (METER_MAX_DB - METER_MIN_DB) * h;
    if (db >= METER_HIGH_DB) return "var(--meter-high)";
    if (db >= METER_MID_DB) return "var(--meter-mid)";
    return "var(--meter-low)";
  });

  return (
    <div
      ref={ref}
      className={cx(
        s.meter,
        vertical ? s.meterVertical : s.meterHorizontal,
        className,
      )}
      role="meter"
      {...rest}
    >
      {Array.from({ length: segments }, (_, i) => (
        <MeterSegment
          key={i}
          index={i}
          segments={segments}
          channel={channel}
        />
      ))}
      {hold && (
        <motion.span
          className={cx(s.meterHold, !vertical && s.meterHoldHorizontal)}
          style={{
            background: holdColour,
            opacity: holdOpacity,
            ...(vertical ? { y: holdOffset } : { x: holdOffset }),
          }}
        />
      )}
    </div>
  );
}

function MeterSegment({
  index,
  segments,
  channel,
}: {
  index: number;
  segments: number;
  channel: MeterChannel;
}) {
  const colour = segmentColour(index, segments);
  const opacity = useTransform(channel.level, (level) =>
    level > index / segments ? 1 : 0.1,
  );
  return (
    <span className={s.meterSegment}>
      <motion.span
        className={s.meterSegmentFill}
        style={{ background: colour, opacity }}
      />
    </span>
  );
}
