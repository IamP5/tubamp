import assert from "node:assert/strict";
import { test } from "node:test";
import { formatSize, isArch, parseManifest } from "./release-schema.ts";

const sha = "a".repeat(64);
const good = () => ({
  schema: 1,
  repo: "IamP5/tubamp",
  version: "0.2.0",
  tag: "v0.2.0",
  publishedAt: "2026-11-14T16:58:00Z",
  url: "https://github.com/IamP5/tubamp/releases/tag/v0.2.0",
  notes: "### Features\n- thing",
  downloads: {
    arm64: {
      name: "tubamp-0.2.0-macos-arm64.pkg",
      url: "https://github.com/IamP5/tubamp/releases/download/v0.2.0/tubamp-0.2.0-macos-arm64.pkg",
      size: 12345678,
      sha256: sha,
    },
    x86_64: {
      name: "tubamp-0.2.0-macos-x86_64.pkg",
      url: "https://github.com/IamP5/tubamp/releases/download/v0.2.0/tubamp-0.2.0-macos-x86_64.pkg",
      size: 12345679,
      sha256: sha,
    },
  },
  minimumMacOS: "11.0",
  signed: false,
});

test("a well-formed manifest becomes a release", () => {
  const r = parseManifest(good());
  assert.equal(r?.version, "0.2.0");
  assert.equal(r?.downloads.x86_64.size, 12345679);
  assert.equal(r?.signed, false);
});

test("a manifest that points a download outside this repo's releases is rejected", () => {
  const m = good();
  m.downloads.arm64.url = "https://evil.example/tubamp-0.2.0-macos-arm64.pkg";
  assert.equal(parseManifest(m), null);
});

test("a manifest missing one architecture is rejected", () => {
  const m = good() as Record<string, unknown>;
  m.downloads = { arm64: good().downloads.arm64 };
  assert.equal(parseManifest(m), null);
});

test("wrong schema, wrong repo, bad version, bad hash and non-objects are rejected", () => {
  assert.equal(parseManifest({ ...good(), schema: 2 }), null);
  assert.equal(parseManifest({ ...good(), repo: "someone/else" }), null);
  assert.equal(parseManifest({ ...good(), version: "0.2" }), null);
  assert.equal(parseManifest({ ...good(), tag: "v9.9.9" }), null);
  const m = good();
  m.downloads.x86_64.sha256 = "nope";
  assert.equal(parseManifest(m), null);
  assert.equal(parseManifest(null), null);
  assert.equal(parseManifest("<html>Not Found</html>"), null);
});

test("optional fields fall back", () => {
  const m = good() as Record<string, unknown>;
  delete m.minimumMacOS;
  delete m.notes;
  const r = parseManifest(m);
  assert.equal(r?.minimumMacOS, "11.0");
  assert.equal(r?.notes, "");
});

test("isArch and formatSize", () => {
  assert.equal(isArch("arm64"), true);
  assert.equal(isArch("ppc"), false);
  assert.equal(formatSize(12_345_678), "12.3 MB");
});
