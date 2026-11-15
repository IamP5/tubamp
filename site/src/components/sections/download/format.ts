import type { Arch } from "../../../lib/release-schema.ts";

const dateFormats = new Map<string, Intl.DateTimeFormat>();

/**
 * "14 Nov 2026" in en-GB. The locale is the page's and the zone is fixed, so
 * the server and every visitor print the same date.
 */
export function releaseDate(iso: string, locale: string): string {
  let format = dateFormats.get(locale);
  if (!format) {
    format = new Intl.DateTimeFormat(locale, { day: "numeric", month: "short", year: "numeric", timeZone: "UTC" });
    dateFormats.set(locale, format);
  }
  return format.format(new Date(iso));
}

export const HASH_PREFIX = 12;

export function shortHash(sha256: string): string {
  return sha256.slice(0, HASH_PREFIX);
}

/** What the page says the plugin needs while there is no release to read it from. */
export const DEFAULT_MACOS = "11.0";

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
export function releaseMeta(
  release: { version: string; minimumMacOS: string } | null,
  copy: { noAccount: string; orLater: string; archs: string },
): string[] {
  return [
    copy.noAccount,
    ...(release ? [`v${release.version}`] : []),
    copy.orLater.replace("{os}", macOS(release?.minimumMacOS ?? DEFAULT_MACOS)),
    copy.archs,
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
