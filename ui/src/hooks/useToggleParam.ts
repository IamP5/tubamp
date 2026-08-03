/**
 * Toggle parameter binding (block enables + amp_cal_input).
 *
 * Writes go through the relay, which wraps them in a proper
 * begin/setValueNotifyingHost/end gesture on the C++ side, so DAW automation and
 * undo see one clean gesture per click.
 */
import { useCallback, useMemo, useSyncExternalStore } from "react";
import { bridge, type ToggleParamId, type ToggleProperties } from "../bridge";

export interface ToggleParam {
  value: boolean;
  properties: ToggleProperties;
  name: string;
  parameterIndex: number;
  setValue(value: boolean): void;
  toggle(): void;
}

export function useToggleParam(id: ToggleParamId): ToggleParam {
  const state = useMemo(() => bridge.toggleState(id), [id]);

  const subscribe = useCallback(
    (onChange: () => void) => {
      const offValue = state.onValueChanged(onChange);
      const offProps = state.onPropertiesChanged(onChange);
      return () => {
        offValue();
        offProps();
      };
    },
    [state],
  );

  const value = useSyncExternalStore(subscribe, () => state.getValue());
  const properties = useSyncExternalStore(subscribe, () => state.properties);

  const setValue = useCallback(
    (next: boolean) => state.setValue(next),
    [state],
  );
  const toggle = useCallback(() => state.setValue(!state.getValue()), [state]);

  return {
    value,
    properties,
    name: properties.name,
    parameterIndex: properties.parameterIndex,
    setValue,
    toggle,
  };
}
