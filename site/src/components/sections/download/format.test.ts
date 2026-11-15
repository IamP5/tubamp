import assert from "node:assert/strict";
import { test } from "node:test";
import { en } from "../../../i18n/en.ts";
import { ptBR } from "../../../i18n/pt-br.ts";
import { archOrder, ctaHref, installSteps, macOS, releaseDate, releaseMeta, shortHash, unsigned } from "./format.ts";

test("release date is day, short month, year in UTC", () => {
  assert.equal(releaseDate("2026-11-14T09:30:00Z", "en-GB"), "14 Nov 2026");
  assert.equal(releaseDate("2026-11-14T23:59:00-05:00", "en-GB"), "15 Nov 2026");
  assert.equal(releaseDate("2026-11-14T09:30:00Z", "pt-BR"), "14 de nov. de 2026");
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

test("the reassurance line always says no account, with or without a release", () => {
  assert.deepEqual(releaseMeta(null, en.release), ["No account", "macOS 11 or later", "Apple Silicon and Intel"]);
  assert.deepEqual(releaseMeta({ version: "0.1.0", minimumMacOS: "12.0" }, en.release), [
    "No account",
    "v0.1.0",
    "macOS 12 or later",
    "Apple Silicon and Intel",
  ]);
  assert.deepEqual(releaseMeta(null, ptBR.release), ["Sem conta", "macOS 11 ou posterior", "Apple Silicon e Intel"]);
});

test("the Gatekeeper step shows for an unsigned release and for no release at all", () => {
  assert.equal(unsigned(null), true);
  assert.equal(unsigned({ signed: false }), true);
  assert.equal(unsigned({ signed: true }), false);

  const steps = [{ text: "Download." }, { text: "Open it.", unsigned: "Open it, then Open Anyway." }];
  assert.deepEqual(installSteps(steps, true), ["Download.", "Open it, then Open Anyway."]);
  assert.deepEqual(installSteps(steps, false), ["Download.", "Open it."]);
});
