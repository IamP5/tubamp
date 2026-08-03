/**
 * Backend detection.
 *
 * Careful: importing the vendored `juce/index.js` *installs a placeholder*
 * `window.__JUCE__` when the real one is missing (check_native_interop.js:44),
 * so "is `window.__JUCE__` defined" alone is not a reliable test once that
 * module has been evaluated. The real backend always publishes non-empty
 * initialisation data (platform + the relay name lists); the placeholder's
 * arrays are empty. Testing that makes detection independent of module
 * evaluation order.
 */
interface InitData {
  __juce__platform?: unknown[];
  __juce__sliders?: unknown[];
  __juce__functions?: unknown[];
}

export function detectNativeBackend(): boolean {
  if (typeof window === "undefined") return false;
  const juce = window.__JUCE__;
  if (!juce || typeof juce.backend?.addEventListener !== "function") return false;
  const data = (juce.initialisationData ?? {}) as InitData;
  return (
    (data.__juce__platform?.length ?? 0) > 0 ||
    (data.__juce__sliders?.length ?? 0) > 0 ||
    (data.__juce__functions?.length ?? 0) > 0
  );
}

/** Evaluated once, at module load, before any UI code runs. */
export const hasNativeBackend = detectNativeBackend();
