import assert from "node:assert/strict";
import { test } from "node:test";
import { CLIPS, FILMS } from "../data/media.ts";
import { fill, localePath, pickLocale } from "./config.ts";
import { en } from "./en.ts";
import { ptBR } from "./pt-br.ts";

test("a visitor from Brazil gets Portuguese, everyone else English", () => {
  assert.equal(pickLocale({ country: "BR" }), "pt-br");
  assert.equal(pickLocale({ country: "br" }), "pt-br");
  assert.equal(pickLocale({ country: "US" }), "en");
  assert.equal(pickLocale({ country: "PT" }), "en");
  assert.equal(pickLocale({ country: null }), "en");
  assert.equal(pickLocale({}), "en");
});

test("a language picked by hand beats the country", () => {
  assert.equal(pickLocale({ choice: "en", country: "BR" }), "en");
  assert.equal(pickLocale({ choice: "pt-br", country: "US" }), "pt-br");
  assert.equal(pickLocale({ choice: "klingon", country: "BR" }), "pt-br");
});

test("English lives at the root, other locales under their own segment", () => {
  assert.equal(localePath("en"), "/");
  assert.equal(localePath("pt-br"), "/pt-br");
});

test("fill replaces every named slot", () => {
  assert.equal(fill("{block}, {n} of {total}", { block: "Gate", n: 1, total: 9 }), "Gate, 1 of 9");
});

/** Every leaf string by its path, with the `{slots}` it carries. */
function leaves(value: unknown, path = ""): Map<string, string[]> {
  if (typeof value === "string") return new Map([[path, [...value.matchAll(/\{(\w+)\}/g)].map((m) => m[1]).sort()]]);
  const out = new Map<string, string[]>();
  for (const [key, child] of Object.entries(value as object)) {
    for (const [p, slots] of leaves(child, `${path}/${key}`)) out.set(p, slots);
  }
  return out;
}

test("Portuguese has every string English has, with the same slots", () => {
  assert.deepEqual([...leaves(ptBR)], [...leaves(en)]);
});

test("no string is left empty in one locale and written in the other", () => {
  const read = (dict: unknown, path: string) =>
    path.split("/").slice(1).reduce((node, key) => (node as Record<string, unknown>)[key], dict) as string;
  for (const path of leaves(en).keys()) {
    assert.equal(read(ptBR, path) === "", read(en, path) === "", path);
  }
});

test("the licence line reads the same in every locale", () => {
  assert.equal(ptBR.captures.attribution, en.captures.attribution);
  assert.ok(ptBR.footer.credits.includes("Powered by TONE3000"));
  assert.match(ptBR.facts.items[0].small, /Powered by TONE3000/);
});

test("every clip has a description", () => {
  for (const clip of [...Object.values(CLIPS), ...Object.values(FILMS)]) {
    assert.ok(en.media[clip.name], clip.name);
    assert.ok(ptBR.media[clip.name], clip.name);
  }
});
