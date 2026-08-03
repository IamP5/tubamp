/**
 * `window.__JUCE__` as installed by the vendored `check_native_interop.js`.
 * Present only inside the WebBrowserComponent; absent in a plain browser, which
 * is exactly how `src/bridge/index.ts` picks the mock implementation.
 */
export type JuceEventToken = [string, number];

export interface JuceBackend {
  addEventListener<T>(eventId: string, fn: (payload: T) => void): JuceEventToken;
  removeEventListener(token: JuceEventToken): void;
  emitEvent(eventId: string, payload: unknown): void;
}

export interface JuceGlobal {
  backend: JuceBackend;
  /** withInitialisationData values arrive as arrays, one entry per push. */
  initialisationData: Record<string, unknown[]>;
  postMessage(message: string): void;
}

declare global {
  interface Window {
    __JUCE__?: JuceGlobal;
  }
}

export {};
