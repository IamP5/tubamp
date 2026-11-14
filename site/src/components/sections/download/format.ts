import type { Arch } from "../../../lib/release-schema.ts";

const dateFormat = new Intl.DateTimeFormat("en-GB", {
  day: "numeric",
  month: "short",
  year: "numeric",
  timeZone: "UTC",
});

/** "14 Nov 2026". Fixed locale and zone so the server and every visitor print the same date. */
export function releaseDate(iso: string): string {
  return dateFormat.format(new Date(iso));
}

export const HASH_PREFIX = 12;

export function shortHash(sha256: string): string {
  return sha256.slice(0, HASH_PREFIX);
}

/** "11.0" reads as "macOS 11"; "13.5" stays "macOS 13.5". */
export function macOS(version: string): string {
  return `macOS ${version.replace(/\.0$/, "")}`;
}

export function archOrder(detected: Arch | null): readonly [Arch, Arch] {
  return detected === "x86_64" ? ["x86_64", "arm64"] : ["arm64", "x86_64"];
}

/**
 * Where a "Download for Mac" button goes. A wrong-architecture installer
 * installs a plugin that never loads, so an installer is named only when the
 * architecture was detected; otherwise the visitor picks in the download field.
 */
export function ctaHref(detected: Arch | null): string {
  return detected ? `/download/${detected}` : "#download";
}

/** The reassurances that sit under a download button, in the order a doubt comes up. */
export function releaseMeta(release: { version: string; minimumMacOS: string } | null): string[] {
  return [
    "No account",
    ...(release ? [`v${release.version}`] : []),
    `${macOS(release?.minimumMacOS ?? "11.0")} or later`,
    "Apple Silicon and Intel",
  ];
}

/**
 * Whether to tell the visitor about the Gatekeeper prompt. With no release to
 * read it is told anyway: an unneeded step costs a line, a missing one costs
 * the install.
 */
export function unsigned(release: { signed: boolean } | null): boolean {
  return !release?.signed;
}

export function installSteps(steps: readonly { text: string; unsigned?: string }[], gatekeeper: boolean): string[] {
  return steps.map((step) => (gatekeeper && step.unsigned) || step.text);
}
