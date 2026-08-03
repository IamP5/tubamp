/**
 * Combo (choice) parameter binding — amp_out_mode, mod_type.
 *
 * `choices` arrives with the async properties push, so the first render can see
 * an empty list; render from `choices` defensively (a Segmented/Menu with zero
 * items rather than assuming three).
 */
import { useCallback, useMemo, useSyncExternalStore } from "react";
import { bridge, type ComboParamId, type ComboProperties } from "../bridge";

export interface ComboParam {
  index: number;
  choices: string[];
  /** Current choice text, or "" before properties arrive. */
  text: string;
  properties: ComboProperties;
  name: string;
  parameterIndex: number;
  setIndex(index: number): void;
}

export function useComboParam(id: ComboParamId): ComboParam {
  const state = useMemo(() => bridge.comboState(id), [id]);

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

  const index = useSyncExternalStore(subscribe, () => state.getChoiceIndex());
  const properties = useSyncExternalStore(subscribe, () => state.properties);

  const setIndex = useCallback(
    (next: number) => state.setChoiceIndex(next),
    [state],
  );

  const choices = properties.choices ?? [];

  return {
    index,
    choices,
    text: choices[index] ?? "",
    properties,
    name: properties.name,
    parameterIndex: properties.parameterIndex,
    setIndex,
  };
}
