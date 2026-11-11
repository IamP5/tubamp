/**
 * Boolean-parameter switch with a micro-label to its left (`comp_auto_makeup`,
 * `mod_color`) — the toggle sibling of ParamMenu, in the same field style.
 */
import { Segmented, Toggle } from "../../components";
import { useToggleParam } from "../../hooks";
import type { ToggleParamId } from "../../bridge";
import s from "./panel.module.css";

export interface ParamToggleProps {
  id: ToggleParamId;
  label: string;
  /** Dim and lock: the engine ignores this switch right now. */
  disabled?: boolean;
}

export function ParamToggle({ id, label, disabled }: ParamToggleProps) {
  const param = useToggleParam(id);
  return (
    <div
      className={disabled ? `${s.field} ${s.fieldDisabled}` : s.field}
      data-param-index={param.parameterIndex}
      title={param.name}
    >
      <span className={s.fieldLabelFit}>{label}</span>
      <Toggle checked={param.value} onChange={param.setValue} disabled={disabled} aria-label={label} />
    </div>
  );
}

export interface ParamSegmentedToggleProps {
  id: ToggleParamId;
  label: string;
  /** Text for the off and on positions. */
  off: string;
  on: string;
}

/** A boolean parameter shown as a two-option segmented control (off | on), for
 *  switches whose on-state would not say which position it is (Phase 90's
 *  Script / Block). */
export function ParamSegmentedToggle({ id, label, off, on }: ParamSegmentedToggleProps) {
  const param = useToggleParam(id);
  return (
    <div className={s.field} data-param-index={param.parameterIndex} title={param.name}>
      <span className={s.fieldLabelFit}>{label}</span>
      <Segmented<"off" | "on">
        aria-label={label}
        value={param.value ? "on" : "off"}
        options={[
          { value: "off", label: off },
          { value: "on", label: on },
        ]}
        onChange={(v) => param.setValue(v === "on")}
      />
    </div>
  );
}
