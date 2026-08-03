/**
 * Mock-bridge only: render the app shell at the emulated window's size.
 *
 * In the plugin the WebView is exactly the window, so `--app-w`/`--app-h` stay
 * at 100% and this hook does nothing. In a browser nothing can resize the tab,
 * so the mock keeps a window size of its own (bridge/mock.ts §fake editor
 * window) and the shell is drawn at it, letterboxed inside the viewport — which
 * is the only way the grip, the min/max clamps and the fluid layout can be
 * driven in dev.
 *
 * Written straight to the element's style rather than through the store: this
 * updates on every frame of a grip drag and must not re-render the app.
 */
import { useEffect, type RefObject } from "react";
import { bridge, isMockBridge, type EditorSize } from "../../bridge";

export function useEmulatedWindow(ref: RefObject<HTMLElement | null>): void {
  useEffect(() => {
    if (!isMockBridge) return;
    const el = ref.current;
    if (!el) return;

    const apply = (size: EditorSize): void => {
      el.style.setProperty("--app-w", `${size.width}px`);
      el.style.setProperty("--app-h", `${size.height}px`);
      /* Writing the vars resizes the shell without resizing the browser, so no
         `resize` event fires — and anything that tracks a moving rectangle rather
         than a resizing one (the embed hole, which is centred) would never learn
         it had moved. In the plugin the shell IS the window and the event is real;
         dispatch the equivalent here so the mock exercises the same code path. */
      window.dispatchEvent(new Event("resize"));
    };

    void bridge.getEditorSize().then(apply);
    return bridge.on("editorSizeChanged", apply);
  }, [ref]);
}
