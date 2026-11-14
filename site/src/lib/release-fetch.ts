import { MANIFEST_URL, parseManifest, type Release } from "./release-schema.ts";

/**
 * Null only when GitHub says 404: the repository has no release yet. Every
 * other outcome throws, so a caller that caches this never stores a hiccup as
 * if it were the answer.
 */
export async function fetchRelease(fetchFn: typeof fetch = fetch): Promise<Release | null> {
  const res = await fetchFn(MANIFEST_URL, { redirect: "follow" });
  if (res.status === 404) return null;
  if (!res.ok) throw new Error(`Release manifest request failed: HTTP ${res.status}`);
  const release = parseManifest(await res.json());
  if (!release) throw new Error("Release manifest is not valid");
  return release;
}
