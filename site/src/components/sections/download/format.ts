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

/** The detected Mac first; Apple Silicon first when the Mac cannot be told apart. */
export function archOrder(detected: Arch | null): readonly [Arch, Arch] {
  return detected === "x86_64" ? ["x86_64", "arm64"] : ["arm64", "x86_64"];
}
