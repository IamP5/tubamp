/**
 * The update feature's toasts, driven by the store's snapshot rather than by
 * events, so hydration and `updateChanged` take the same path.
 *
 *  - The first-run disclosure, once ever (`disclose`, then `markDisclosed`).
 *  - "tubamp 0.4.0 is available" once per version (`announce`, then
 *    `markAnnounced`). The flags are cleared C++-side for every editor; the
 *    local memory only covers the round trip before that event lands.
 *  - An install started from that toast reports through toasts until it ends,
 *    because the user who clicked it is not looking at Settings.
 */
import { useStore } from "../../store";
import type { UpdateView } from "../../bridge";
import {
  DISCLOSURE_TEXT,
  availableText,
  failureAction,
  failureText,
  installedText,
} from "./words";

export function watchUpdateNotices(): () => void {
  let disclosed = false;
  const announced = new Set<string>();
  let follow: { version: string; toastId: number } | null = null;

  const store = () => useStore.getState();

  const setToastMessage = (id: number, message: string): void =>
    useStore.setState((s) => ({
      toasts: s.toasts.map((t) => (t.id === id && t.message !== message ? { ...t, message } : t)),
    }));

  const install = (version: string): void => {
    void store().updateCommand({ type: "install", version });
    const toastId = store().toast(`Downloading ${version}`, "info", 0, {
      label: "Cancel",
      onSelect: () => void store().updateCommand({ type: "cancel" }),
    });
    follow = { version, toastId };
  };

  const endFollow = (): void => {
    if (follow) store().dismissToast(follow.toastId);
    follow = null;
  };

  const track = (view: UpdateView): void => {
    if (!follow) return;
    const { phase } = view;
    if (phase.kind === "downloading" && phase.version === follow.version) {
      const pct = phase.total > 0 ? Math.round((phase.received / phase.total) * 100) : 0;
      setToastMessage(follow.toastId, `Downloading ${phase.version} · ${pct}%`);
    } else if (phase.kind === "installing" && phase.version === follow.version) {
      setToastMessage(follow.toastId, `Installing ${phase.version}…`);
    } else if (phase.kind === "restartPending") {
      endFollow();
      store().toast(installedText(phase.version, view.hostName), "success", 10000);
    } else if (phase.kind === "failed") {
      endFollow();
      const action = failureAction(phase.reason, phase.version);
      const { command } = action;
      store().toast(failureText(phase.reason, phase.version), "error", 10000, {
        label: action.label,
        onSelect: () =>
          command.type === "install" ? install(command.version) : void store().updateCommand(command),
      });
    } else {
      endFollow();
    }
  };

  const run = (view: UpdateView): void => {
    if (view.disclose && !disclosed) {
      disclosed = true;
      store().toast(DISCLOSURE_TEXT, "info", 10000, {
        label: "Turn off",
        onSelect: () => void store().updateCommand({ type: "setAutoCheck", enabled: false }),
      });
      void store().updateCommand({ type: "markDisclosed" });
    }

    const version = view.announce;
    if (version && !announced.has(version)) {
      announced.add(version);
      const { phase } = view;
      if (phase.kind === "available" && phase.version === version) {
        store().toast(availableText(version), "info", 12000, {
          label: "Update",
          onSelect: () => install(version),
        });
      } else if (phase.kind === "manual" && phase.version === version) {
        store().toast(availableText(version), "info", 12000, {
          label: "Get the installer",
          onSelect: () => void store().updateCommand({ type: "openReleasePage" }),
        });
      }
      void store().updateCommand({ type: "markAnnounced", version });
    }

    track(view);
  };

  run(store().update);
  return useStore.subscribe((s, prev) => {
    if (s.update !== prev.update) run(s.update);
  });
}
