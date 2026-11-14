import { cacheLife, cacheTag } from "next/cache";
import { MANIFEST_URL, parseManifest, type Release } from "./release-schema";

/**
 * The latest published release, read from the manifest asset GitHub serves at a
 * stable URL. Cached and refreshed in the background, so a new plugin release
 * reaches the site within minutes without a deploy.
 */
export async function getLatestRelease(): Promise<Release | null> {
  "use cache";
  cacheLife({ stale: 300, revalidate: 300, expire: 86_400 });
  cacheTag("release");
  try {
    const res = await fetch(MANIFEST_URL, { redirect: "follow" });
    if (!res.ok) return null;
    return parseManifest(await res.json());
  } catch {
    return null;
  }
}
