/**
 * Gate and compressor meters (docs/FX-ENGINES.md §7.4), fed by the `dynMeters`
 * event through `useDynMeters`. Every moving part is a MotionValue transform or a
 * direct DOM text write, so a meter never re-renders React.
 *
 *  - `GateMeter`: a state lamp (closed / closing / open), the detector level over
 *    -90..0 dBFS with a marker at the threshold and one 6 dB lower (the close
 *    point, Tight and Smooth only), and gain reduction over 0..60 dB.
 *  - `CompMeter`: gain reduction over 0..20 dB for one instance.
 */
import { useCallback, useEffect, useRef } from "react";
import { motion, useMotionValueEvent, useTransform, type MotionValue } from "motion/react";
import {
  COMP_GR_SCALE_DB,
  GATE_GR_SCALE_DB,
  GATE_LEVEL_MAX_DB,
  GATE_LEVEL_MIN_DB,
  useDynMeters,
  useSliderParam,
} from "../../hooks";
import s from "./panel.module.css";

/** Hysteresis between the open and close points in Tight and Smooth, dB. */
const GATE_HYSTERESIS_DB = 6;

const clamp01 = (v: number): number => Math.min(1, Math.max(0, v));

interface DynBarProps {
  label: string;
  /** Smoothed value in dB. */
  value: MotionValue<number>;
  min: number;
  max: number;
  /** Text for the numeric readout. */
  format(db: number): string;
  /** Static tick positions in dB (the gate's threshold and close point). */
  markers?: readonly { db: number; className: string; title: string }[];
}

function DynBar({ label, value, min, max, format, markers }: DynBarProps) {
  const width = useTransform(value, (v) => `${(clamp01((v - min) / (max - min)) * 100).toFixed(2)}%`);
  const readoutRef = useRef<HTMLSpanElement>(null);

  const paint = useCallback(
    (v: number): void => {
      const el = readoutRef.current;
      if (!el) return;
      const text = format(v);
      if (el.textContent !== text) el.textContent = text;
    },
    [format],
  );
  useMotionValueEvent(value, "change", paint);
  useEffect(() => {
    paint(value.get());
  }, [value, paint]);

  return (
    <div className={s.dynBarRow}>
      <span className={s.dynBarLabel}>{label}</span>
      <div className={s.dynBarTrack} role="meter" aria-label={label}>
        <motion.div className={s.dynBarFill} style={{ width }} />
        {markers?.map((m) => (
          <span
            key={m.className}
            className={`${s.dynMarker} ${m.className}`}
            style={{ left: `${(clamp01((m.db - min) / (max - min)) * 100).toFixed(2)}%` }}
            title={m.title}
          />
        ))}
      </div>
      <span ref={readoutRef} className={s.dynBarReadout} />
    </div>
  );
}

/** Gain-reduction readout clamped to the meter's scale: past it the scale value is
 *  shown with a trailing plus ("−60+ dB"), exactly what the bar shows. */
function grFormatter(scaleDb: number): (db: number) => string {
  return (db) => {
    if (db >= scaleDb) return `−${scaleDb}+ dB`;
    return db < 0.05 ? "0.0 dB" : `−${db.toFixed(1)} dB`;
  };
}
const formatGateGr = grFormatter(GATE_GR_SCALE_DB);
const formatCompGr = grFormatter(COMP_GR_SCALE_DB);
const formatLevel = (db: number): string =>
  db <= GATE_LEVEL_MIN_DB ? `< ${GATE_LEVEL_MIN_DB} dB` : `${db.toFixed(0)} dB`;

/** One lamp: lit while `gateState` equals `on`. */
function Lamp({ state, on, className, children }: {
  state: MotionValue<number>;
  on: number;
  className: string;
  children: string;
}) {
  const opacity = useTransform(state, (v) => (v === on ? 1 : 0.22));
  return (
    <motion.span className={`${s.lamp} ${className}`} style={{ opacity }}>
      <span className={s.lampDot} />
      {children}
    </motion.span>
  );
}

/** `classic` hides the close-point marker: Classic has no hysteresis. */
export function GateMeter({ classic }: { classic: boolean }) {
  const meters = useDynMeters();
  const threshold = useSliderParam("gate_threshold").scaled;

  const markers = [
    { db: threshold, className: s.dynMarkerOpen, title: "Open point (threshold)" },
    ...(classic
      ? []
      : [{ db: threshold - GATE_HYSTERESIS_DB, className: s.dynMarkerClose, title: "Close point (6 dB lower)" }]),
  ];

  return (
    <div className={s.meterCard}>
      <div className={s.lampRow}>
        <Lamp state={meters.gateState} on={2} className={s.lampOpen}>OPEN</Lamp>
        <Lamp state={meters.gateState} on={1} className={s.lampClosing}>CLOSING</Lamp>
        <Lamp state={meters.gateState} on={0} className={s.lampClosed}>CLOSED</Lamp>
      </div>
      <DynBar
        label="LEVEL"
        value={meters.gateLevel}
        min={GATE_LEVEL_MIN_DB}
        max={GATE_LEVEL_MAX_DB}
        format={formatLevel}
        markers={markers}
      />
      <DynBar label="GR" value={meters.gateGr} min={0} max={GATE_GR_SCALE_DB} format={formatGateGr} />
    </div>
  );
}

/** Gain reduction of one compressor instance (0 = comp, 1 = comp2, 2 = comp3). */
export function CompMeter({ instance }: { instance: number }) {
  const meters = useDynMeters();
  const value = meters.compGr[instance] ?? meters.compGr[0];
  return (
    <div className={s.compMeter}>
      <DynBar label="GR" value={value} min={0} max={COMP_GR_SCALE_DB} format={formatCompGr} />
    </div>
  );
}
