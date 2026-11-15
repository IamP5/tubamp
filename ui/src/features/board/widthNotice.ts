/**
 * The mono-instance notice as a toast: shown once when the processor raises
 * `WidthView.notice`, then acknowledged so it stops raising it. The processor
 * decides when (a mono-out instance whose chain just became stereo, once per
 * instance); the local flag only covers the round trip before the cleared
 * notice arrives back.
 */
import { bridge, type WidthView } from "../../bridge";
import { useStore } from "../../store";
import { MONO_INSTANCE_TEXT } from "./wires";

export function watchWidthNotice(): () => void {
  let shown = false;

  const run = (width: WidthView | undefined): void => {
    if (width?.notice !== "monoHost") {
      shown = false;
      return;
    }
    if (shown) return;
    shown = true;
    useStore.getState().toast(MONO_INSTANCE_TEXT.join(" "), "info", 0);
    void bridge.ackNotice("monoHost");
  };

  run(useStore.getState().width);
  return useStore.subscribe((s, prev) => {
    if (s.width !== prev.width) run(s.width);
  });
}
