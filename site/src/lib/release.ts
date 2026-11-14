import { cacheLife, cacheTag } from "next/cache";
import { fetchRelease } from "./release-fetch";
import type { Release } from "./release-schema";

/**
 * Cached and refreshed in the background, so a new plugin release reaches the
 * site within minutes without a deploy. It throws when the manifest cannot be
 * read, so a failed refresh does not overwrite a good entry: use
 * `getLatestRelease`.
 */
async function loadLatestRelease(): Promise<Release | null> {
  "use cache";
  cacheLife({ stale: 300, revalidate: 300, expire: 86_400 });
  cacheTag("release");
  return fetchRelease();
}

/** The latest release, or null when there is none or it cannot be read now. */
export async function getLatestRelease(): Promise<Release | null> {
  try {
    return await loadLatestRelease();
  } catch {
    return null;
  }
}
