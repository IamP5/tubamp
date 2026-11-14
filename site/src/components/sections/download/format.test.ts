import assert from "node:assert/strict";
import { test } from "node:test";
import { archOrder, ctaHref, macOS, releaseDate, shortHash } from "./format.ts";

test("release date is day, short month, year in UTC", () => {
  assert.equal(releaseDate("2026-11-14T09:30:00Z"), "14 Nov 2026");
  assert.equal(releaseDate("2026-11-14T23:59:00-05:00"), "15 Nov 2026");
});

test("hash is shortened to its first twelve characters", () => {
  const sha = "a3f1c09be47d2e5f8a6b1c3d9e0f7a2b4c6d8e1f3a5b7c9d0e2f4a6b8c1d3e5f";
  assert.equal(shortHash(sha), "a3f1c09be47d");
});

test("minimum macOS drops a trailing .0", () => {
  assert.equal(macOS("11.0"), "macOS 11");
  assert.equal(macOS("13.5"), "macOS 13.5");
});

test("detected Mac goes first, Apple Silicon when unknown", () => {
  assert.deepEqual(archOrder("x86_64"), ["x86_64", "arm64"]);
  assert.deepEqual(archOrder("arm64"), ["arm64", "x86_64"]);
  assert.deepEqual(archOrder(null), ["arm64", "x86_64"]);
});

test("a download button names an installer only for a detected architecture", () => {
  assert.equal(ctaHref("arm64"), "/download/arm64");
  assert.equal(ctaHref("x86_64"), "/download/x86_64");
  assert.equal(ctaHref(null), "#download");
});
