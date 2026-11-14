import assert from "node:assert/strict";
import { test } from "node:test";
import { archFromHints, notAMac } from "./platform.ts";

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

test("phones, tablets and other desktops cannot install a Mac plugin", () => {
  assert.equal(notAMac({ platform: "iPhone", maxTouchPoints: 5 }), true);
  assert.equal(notAMac({ platform: "Linux armv81", maxTouchPoints: 5 }), true);
  assert.equal(notAMac({ platform: "Android", maxTouchPoints: 5 }), true);
  assert.equal(notAMac({ platform: "Win32", maxTouchPoints: 0 }), true);
});

test("an iPad that reports itself as a Mac is told apart by its touch screen", () => {
  assert.equal(notAMac({ platform: "MacIntel", maxTouchPoints: 5 }), true);
});

test("a Mac, or a platform that says nothing, keeps the download button", () => {
  assert.equal(notAMac({ platform: "MacIntel", maxTouchPoints: 0 }), false);
  assert.equal(notAMac({ platform: "macOS", maxTouchPoints: 0 }), false);
  assert.equal(notAMac({ platform: "", maxTouchPoints: 5 }), false);
});
