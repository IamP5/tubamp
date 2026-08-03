/**
 * Bridge selection: the real JUCE implementation inside the WebBrowserComponent,
 * the mock everywhere else (`npm run dev` in a browser).
 *
 * Import `bridge` from here — never `../juce/index.js` directly, and never
 * `./mock` directly outside of dev tooling.
 */
// `./detect` must be imported first: it snapshots the backend before the
// vendored JUCE lib (pulled in transitively by ./juce) can install its
// placeholder `window.__JUCE__`.
import { hasNativeBackend } from "./detect";
import { juceBridge } from "./juce";
import { mockBridge } from "./mock";
import type { Bridge } from "./types";

export const bridge: Bridge = hasNativeBackend ? juceBridge : mockBridge;
export const isMockBridge = bridge.kind === "mock";

export * from "./types";
export { hasNativeBackend };
