/**
 * Hand-written types for the vendored `juce-framework-frontend` module
 * (JUCE 8.0.15, `modules/juce_gui_extra/native/javascript/index.js`).
 *
 * The module ships no types; every field below is read off the implementation
 * (see docs/research/juce-webview.md §3.1 for the line-by-line citations).
 * Keep this file in sync when re-vendoring a newer JUCE.
 */

export interface SliderProperties {
  start: number;
  end: number;
  skew: number;
  name: string;
  label: string;
  numSteps: number;
  interval: number;
  parameterIndex: number;
}

export interface ToggleProperties {
  name: string;
  parameterIndex: number;
}

export interface ComboBoxProperties {
  name: string;
  parameterIndex: number;
  choices: string[];
}

export interface ListenerList {
  addListener(fn: () => void): number;
  removeListener(id: number): void;
}

export interface SliderState {
  name: string;
  identifier: string;
  scaledValue: number;
  properties: SliderProperties;
  valueChangedEvent: ListenerList;
  propertiesChangedEvent: ListenerList;
  /** Value in parameter units (dB / Hz / ms / …). */
  getScaledValue(): number;
  /** Value in 0..1, skew applied. */
  getNormalisedValue(): number;
  setNormalisedValue(newValue: number): void;
  /** Call before the first setNormalisedValue of a gesture (host automation touch). */
  sliderDragStarted(): void;
  /** Call after the last setNormalisedValue of a gesture. */
  sliderDragEnded(): void;
}

export interface ToggleState {
  name: string;
  identifier: string;
  value: boolean;
  properties: ToggleProperties;
  valueChangedEvent: ListenerList;
  propertiesChangedEvent: ListenerList;
  getValue(): boolean;
  setValue(newValue: boolean): void;
}

export interface ComboBoxState {
  name: string;
  identifier: string;
  value: number;
  properties: ComboBoxProperties;
  valueChangedEvent: ListenerList;
  propertiesChangedEvent: ListenerList;
  getChoiceIndex(): number;
  setChoiceIndex(index: number): void;
}

export function getSliderState(name: string): SliderState;
export function getToggleState(name: string): ToggleState;
export function getComboBoxState(name: string): ComboBoxState;

/**
 * Returns a proxy that emits `__juce__invoke` and resolves when C++ calls the
 * completion. Warns (does not throw) when the backend doesn't know the name,
 * which is why the SPA still boots in a plain browser.
 */
export function getNativeFunction(
  name: string,
): (...args: unknown[]) => Promise<unknown>;

/** e.g. "index.html" -> "juce://juce.backend/index.html". */
export function getBackendResourceAddress(path: string): string;

export class ControlParameterIndexUpdater {
  constructor(controlParameterIndexAnnotation: string);
  handleMouseMove(event: MouseEvent): void;
}
