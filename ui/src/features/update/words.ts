/**
 * What the update feature says, in one place, so the Settings row and the
 * toasts word a state the same way. Each phase maps to one status line and at
 * most one action (ui/DESIGN.md §Less); the switches are exhaustive, so a new
 * phase or reason fails typecheck here.
 */
import {
  unreachable,
  type UpdateCheckProblem,
  type UpdateCommand,
  type UpdateFailure,
  type UpdateManualReason,
  type UpdateView,
} from "../../bridge";

export interface UpdateAction {
  label: string;
  command: UpdateCommand;
}

export interface UpdateStatus {
  /** One line, sentence case. Empty when there is nothing to say. */
  text: string;
  /** Why, for a state the user cannot fix from here (one quiet line). */
  detail: string | null;
  problem: boolean;
  action: UpdateAction | null;
  /** 0..1 while downloading. */
  progress: number | null;
  /** A newer version is known and its notes are worth opening. */
  whatsNew: boolean;
}

const RELEASE_PAGE: UpdateAction = {
  label: "Get the installer",
  command: { type: "openReleasePage" },
};

function status(text: string, extra: Partial<UpdateStatus> = {}): UpdateStatus {
  return { text, detail: null, problem: false, action: null, progress: null, whatsNew: false, ...extra };
}

export function checkProblemText(problem: UpdateCheckProblem): string {
  switch (problem) {
    case "unreachable":
      return "Couldn't check for updates";
    case "unverified":
      return "Couldn't verify the update information";
    default:
      return unreachable(problem);
  }
}

export function failureText(reason: UpdateFailure, version: string): string {
  switch (reason) {
    case "network":
      return `Couldn't download ${version}`;
    case "verify":
      return "The download didn't match the release. Nothing was installed.";
    case "noSpace":
      return `Couldn't install ${version}: not enough space`;
    case "notWritable":
      return `Couldn't install ${version}: the plug-in folder is read-only`;
    default:
      return unreachable(reason);
  }
}

/** A retry cannot fix a folder that refuses the write; the installer can. */
export function failureAction(reason: UpdateFailure, version: string): UpdateAction {
  switch (reason) {
    case "network":
    case "verify":
    case "noSpace":
      return { label: "Try again", command: { type: "install", version } };
    case "notWritable":
      return RELEASE_PAGE;
    default:
      return unreachable(reason);
  }
}

export function manualReasonText(reason: UpdateManualReason): string {
  switch (reason) {
    case "systemDomain":
      return "It is installed for all users, in /Library.";
    case "unexpectedPath":
      return "It was moved or renamed after it was installed.";
    case "readOnly":
      return "Its folder is read-only.";
    case "noAtomicSwap":
      return "Its disk cannot replace a plug-in in one step.";
    case "notAnAudioUnit":
      return "The standalone app updates from the installer.";
    default:
      return unreachable(reason);
  }
}

export function installedText(version: string, hostName: string): string {
  return `${version} is installed. Restart ${hostName || "the host"} to use it.`;
}

export function availableText(version: string): string {
  return `tubamp ${version} is available`;
}

export const DISCLOSURE_TEXT = "tubamp checks GitHub once a day for a new version.";
export const SYSTEM_COPY_TEXT = "Another copy of tubamp is installed in /Library.";

export function updateStatus(view: UpdateView): UpdateStatus {
  const { phase } = view;
  switch (phase.kind) {
    case "off":
      return status("Development build");
    case "idle":
      return phase.problem
        ? status(checkProblemText(phase.problem), {
            problem: true,
            action: { label: "Check now", command: { type: "checkNow" } },
          })
        : status("Up to date", { action: { label: "Check now", command: { type: "checkNow" } } });
    case "checking":
      return status("Checking…");
    case "available":
      return status(`${phase.version} available`, {
        action: { label: "Update", command: { type: "install", version: phase.version } },
        whatsNew: true,
      });
    case "downloading": {
      const progress = phase.total > 0 ? Math.min(1, phase.received / phase.total) : 0;
      return status(`Downloading ${phase.version} · ${Math.round(progress * 100)}%`, {
        action: { label: "Cancel", command: { type: "cancel" } },
        progress,
        whatsNew: true,
      });
    }
    case "installing":
      return status(`Installing ${phase.version}…`, { progress: 1, whatsNew: true });
    case "restartPending":
      return status(installedText(phase.version, view.hostName), { whatsNew: true });
    case "failed": {
      const action = failureAction(phase.reason, phase.version);
      return status(failureText(phase.reason, phase.version), {
        problem: true,
        action,
        whatsNew: action !== RELEASE_PAGE,
      });
    }
    case "manual":
      return status(`${phase.version} available. This copy can't update itself.`, {
        detail: manualReasonText(phase.reason),
        action: RELEASE_PAGE,
      });
    default:
      return unreachable(phase);
  }
}

export function newerVersion(view: UpdateView): string | null {
  const { phase } = view;
  switch (phase.kind) {
    case "off":
    case "idle":
    case "checking":
      return null;
    case "available":
    case "downloading":
    case "installing":
    case "restartPending":
    case "failed":
    case "manual":
      return phase.version;
    default:
      return unreachable(phase);
  }
}
