import { getLatestRelease } from "@/lib/release";
import DownloadField from "./download/DownloadField";

export { default as DownloadCta } from "./download/DownloadCta";

// getLatestRelease is "use cache" with a five-minute stale time, which is long
// enough to be part of the prerendered shell, so no Suspense boundary is needed.
export default async function Download() {
  return <DownloadField release={await getLatestRelease()} />;
}
