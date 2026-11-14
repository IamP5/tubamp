import assert from "node:assert/strict";
import { test } from "node:test";
import { fetchRelease } from "./release-fetch.ts";

const sha = "a".repeat(64);
const manifest = {
  schema: 1,
  repo: "IamP5/tubamp",
  version: "0.2.0",
  tag: "v0.2.0",
  publishedAt: "2026-11-14T16:58:00Z",
  url: "https://github.com/IamP5/tubamp/releases/tag/v0.2.0",
  downloads: {
    arm64: {
      name: "a.pkg",
      url: "https://github.com/IamP5/tubamp/releases/download/v0.2.0/a.pkg",
      size: 10,
      sha256: sha,
    },
    x86_64: {
      name: "b.pkg",
      url: "https://github.com/IamP5/tubamp/releases/download/v0.2.0/b.pkg",
      size: 10,
      sha256: sha,
    },
  },
};
const json = (body: unknown, status = 200) => async () => Response.json(body, { status });

test("a valid manifest becomes a release", async () => {
  assert.equal((await fetchRelease(json(manifest)))?.version, "0.2.0");
});

test("404 means no release yet", async () => {
  assert.equal(await fetchRelease(json({ message: "Not Found" }, 404)), null);
});

test("other failures throw instead of reading as no release", async () => {
  for (const status of [429, 500, 502, 503]) {
    await assert.rejects(fetchRelease(json({}, status)), /manifest/i, String(status));
  }
  await assert.rejects(fetchRelease(async () => Promise.reject(new TypeError("fetch failed"))));
});

test("a 200 that is not a valid manifest throws", async () => {
  await assert.rejects(fetchRelease(json({ schema: 2 })));
  await assert.rejects(fetchRelease(async () => new Response("<html>oops</html>")));
});
