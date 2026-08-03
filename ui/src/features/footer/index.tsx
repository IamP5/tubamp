/**
 * Footer band (56px) — docs/REACT-UI.md §UX structure, current-ui-inventory.md
 * §PluginEditor footer controls.
 *
 * IN TRIM knob + meter, centered info line ("Model SR … Hz · Latency … smp"),
 * meter + OUT LEVEL knob. Meters are motion-value driven (`useMeters`, shared
 * rAF, asymmetric attack/release + time-based peak-hold decay) — this
 * component never re-renders per meter frame.
 */
import {
  useCallback,
  useEffect,
  useMemo,
  useRef,
  useState,
  type ReactNode,
} from "react";
import { AnimatePresence, motion } from "motion/react";
import s from "./footer.module.css";
import { useStore } from "../../store";
import { Knob, Meter } from "../../components";
import { useMeters } from "../../hooks";
import { bridge, type ModelInfo, type SliderParamId } from "../../bridge";
import { formatParamValue } from "../../bridge/paramMeta";
import { duration, ease } from "../../theme/motion";

/**
 * `ModelInfo.latencySamples` is the processor's reported latency, not a
 * model-only figure: `TubampAudioProcessor::updateLatency` reports the NAM
 * engine's latency when an enabled amp block holds a model and 0 otherwise, so
 * "no model" genuinely is "Latency 0 smp" — there is nothing else to surface.
 */
function infoLine(model: ModelInfo | null): string {
  if (!model) return "No model  ·  Latency 0 smp";
  return `Model SR ${Math.round(model.sampleRateHz)} Hz  ·  Latency ${model.latencySamples} smp`;
}

export function Footer() {
  const model = useStore((s) => s.model);
  const meters = useMeters();

  return (
    <div className={s.bar}>
      <TrimKnob id="input_trim" label="In Trim" />
      <div className={s.meterSlot}>
        <Meter channel={meters.input} aria-label="Input level" />
      </div>

      <span className={s.info} data-numeric="true">
        {infoLine(model)}
      </span>

      <div className={s.meterSlot}>
        <Meter channel={meters.output} aria-label="Output level" />
      </div>
      <TrimKnob id="output_level" label="Out Level" />
    </div>
  );
}

/**
 * IN TRIM / OUT LEVEL knob: the native control has no inline text box, only a
 * transient value bubble while hovering/dragging (`setPopupDisplayEnabled`).
 * Reproduced here without touching the shared `Knob` primitive: value text is
 * painted imperatively into a ref (same pattern `Knob` itself uses) so the
 * drag-rate `onValueChanged` stream never triggers a React re-render — only
 * the hover/drag boolean flips state, once per gesture.
 */
function TrimKnob({ id, label }: { id: SliderParamId; label: string }) {
  const state = useMemo(() => bridge.sliderState(id), [id]);
  const hoverRef = useRef(false);
  const dragRef = useRef(false);
  const [visible, setVisible] = useState(false);
  const textRef = useRef<HTMLSpanElement>(null);

  const paint = useCallback(() => {
    if (textRef.current) {
      textRef.current.textContent = formatParamValue(state.getScaledValue(), state.properties);
    }
  }, [state]);

  useEffect(() => {
    const offValue = state.onValueChanged(paint);
    const offProps = state.onPropertiesChanged(paint);
    return () => {
      offValue();
      offProps();
    };
  }, [state, paint]);

  useEffect(() => {
    if (visible) paint();
  }, [visible, paint]);

  const sync = (): void => setVisible(hoverRef.current || dragRef.current);

  return (
    <div
      className={s.knobPopupAnchor}
      onPointerEnter={() => {
        hoverRef.current = true;
        sync();
      }}
      onPointerLeave={() => {
        hoverRef.current = false;
        sync();
      }}
      onPointerDown={() => {
        dragRef.current = true;
        sync();
      }}
      onPointerUp={() => {
        dragRef.current = false;
        sync();
      }}
      onPointerCancel={() => {
        dragRef.current = false;
        sync();
      }}
    >
      <Knob id={id} label={label} size="sm" hideValue />
      <ValuePopup visible={visible} textRef={textRef} />
    </div>
  );
}

function ValuePopup({
  visible,
  textRef,
}: {
  visible: boolean;
  textRef: React.RefObject<HTMLSpanElement | null>;
}): ReactNode {
  return (
    <AnimatePresence>
      {visible && (
        <motion.span
          ref={textRef}
          className={s.knobPopup}
          data-numeric="true"
          // Opacity only, as Tooltip does: animating `y` would make Motion write
          // the whole `transform`, dropping the stylesheet's translateX(-50%)
          // centering and letting the OUT LEVEL bubble run off the right edge.
          initial={{ opacity: 0 }}
          animate={{ opacity: 1 }}
          exit={{ opacity: 0 }}
          transition={{ duration: duration.instant, ease: ease.out }}
        />
      )}
    </AnimatePresence>
  );
}
