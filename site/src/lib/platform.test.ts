import assert from "node:assert/strict";
import { test } from "node:test";
import { archFromHints } from "./platform.ts";

test("client-hint architecture decides on Chromium", () => {
  assert.equal(archFromHints({ platform: "macOS", uaArchitecture: "arm" }), "arm64");
  assert.equal(archFromHints({ platform: "macOS", uaArchitecture: "x86" }), "x86_64");
});

test("GPU name decides when there are no client hints", () => {
  assert.equal(archFromHints({ platform: "MacIntel", glRenderer: "ANGLE (Apple, ANGLE Metal Renderer: Apple M2 Pro, Unspecified Version)" }), "arm64");
  assert.equal(archFromHints({ platform: "MacIntel", glRenderer: "Intel(R) Iris(TM) Plus Graphics 655" }), "x86_64");
});

test("Safari's masked GPU name is not a guess", () => {
  assert.equal(archFromHints({ platform: "MacIntel", glRenderer: "Apple GPU" }), null);
});

test("other platforms get no installer suggestion", () => {
  assert.equal(archFromHints({ platform: "Win32", uaArchitecture: "x86" }), null);
  assert.equal(archFromHints({ platform: "Linux x86_64" }), null);
});
