export const REPO = "IamP5/tubamp";
export const RELEASES_URL = `https://github.com/${REPO}/releases`;
export const MANIFEST_URL = `${RELEASES_URL}/latest/download/manifest.json`;

export const ARCHES = ["arm64", "x86_64"] as const;
export type Arch = (typeof ARCHES)[number];

export type Download = {
  name: string;
  url: string;
  size: number;
  sha256: string;
};

export type Release = {
  version: string;
  tag: string;
  publishedAt: string;
  url: string;
  notes: string;
  downloads: Record<Arch, Download>;
  minimumMacOS: string;
  signed: boolean;
};

export function isArch(value: string): value is Arch {
  return (ARCHES as readonly string[]).includes(value);
}

const str = (v: unknown): v is string => typeof v === "string" && v.length > 0;

const NAME = /^[\w.-]+\.pkg$/;
const MAX_SIZE = 1_000_000_000;

// The site redirects visitors to this URL, so a manifest must never be able to
// point elsewhere. The URL is parsed rather than prefix-matched: a prefix check
// lets "download/../../.." climb out of the repository.
function isAssetUrl(raw: string, tag: string, name: string): boolean {
  let url: URL;
  try {
    url = new URL(raw);
  } catch {
    return false;
  }
  return (
    url.origin === "https://github.com" &&
    url.pathname === `/${REPO}/releases/download/${tag}/${name}` &&
    url.search === "" &&
    url.hash === "" &&
    url.username === "" &&
    url.password === ""
  );
}

function parseDownload(input: unknown, tag: string): Download | null {
  if (typeof input !== "object" || input === null) return null;
  const d = input as Record<string, unknown>;
  if (!str(d.name) || !str(d.url) || !str(d.sha256)) return null;
  if (!NAME.test(d.name) || !isAssetUrl(d.url, tag, d.name)) return null;
  if (!/^[0-9a-f]{64}$/.test(d.sha256)) return null;
  if (typeof d.size !== "number" || !Number.isSafeInteger(d.size) || d.size < 1 || d.size > MAX_SIZE) return null;
  return { name: d.name, url: d.url, size: d.size, sha256: d.sha256 };
}

/** The one place untrusted manifest JSON becomes a Release. Null means "no usable release". */
export function parseManifest(input: unknown): Release | null {
  if (typeof input !== "object" || input === null) return null;
  const m = input as Record<string, unknown>;
  if (m.schema !== 1 || m.repo !== REPO) return null;
  if (!str(m.version) || !/^\d+\.\d+\.\d+$/.test(m.version)) return null;
  if (m.tag !== `v${m.version}`) return null;
  if (!str(m.publishedAt) || Number.isNaN(Date.parse(m.publishedAt))) return null;
  if (m.url !== `${RELEASES_URL}/tag/${m.tag}`) return null;
  if (typeof m.downloads !== "object" || m.downloads === null) return null;
  const downloads = m.downloads as Record<string, unknown>;
  const arm64 = parseDownload(downloads.arm64, m.tag);
  const x86_64 = parseDownload(downloads.x86_64, m.tag);
  if (!arm64 || !x86_64) return null;
  return {
    version: m.version,
    tag: m.tag,
    publishedAt: m.publishedAt,
    url: m.url,
    notes: typeof m.notes === "string" ? m.notes : "",
    downloads: { arm64, x86_64 },
    minimumMacOS: str(m.minimumMacOS) ? m.minimumMacOS : "11.0",
    signed: m.signed === true,
  };
}

export function formatSize(bytes: number): string {
  return `${(bytes / 1_000_000).toFixed(1)} MB`;
}
