// Compares the UI's static parameter table (ui/src/bridge/paramMeta.ts) with the
// plugin's real layout (tests/golden/params-v4.tsv, written by `tubamp_golden --params`).
//
//   cd ui && npx esbuild src/bridge/paramMeta.ts --bundle --format=esm --outfile=../build/uicheck/pm.mjs
//   node tools/check_ui_params.mjs build/uicheck/pm.mjs tests/golden/params-v4.tsv
//
// The UI table drives knob reset values, the mock bridge and PARAM_INDEX, which is only
// right if the order matches the C++. Exit status 1 on any mismatch.
import { readFileSync } from "node:fs";
import { resolve } from "node:path";
import { pathToFileURL } from "node:url";

const [bundle, dump] = process.argv.slice(2);
if (!bundle || !dump) {
  console.error("usage: check_ui_params.mjs <bundled paramMeta.mjs> <params.tsv>");
  process.exit(2);
}

const ui = await import(pathToFileURL(resolve(bundle)).href);
const rows = readFileSync(dump, "utf8")
  .split("\n")
  .filter((line) => line && !line.startsWith("#"))
  .map((line) => {
    const [index, id, name, kind, min, max, step, skew, def, hint, choices] = line.split("\t");
    return { index: Number(index), id, name, kind, min: Number(min), max: Number(max),
             step: Number(step), skew: Number(skew), def: Number(def), hint: Number(hint),
             choices: choices ? choices.split("|") : [] };
  });

const problems = [];
const near = (a, b, tol = 1e-6) => Math.abs(a - b) <= tol * Math.max(1, Math.abs(a), Math.abs(b));
const sliders = new Map(ui.SLIDER_SPECS.map((s) => [s.id, s]));
const toggles = new Map(ui.TOGGLE_SPECS.map((s) => [s.id, s]));
const combos = new Map(ui.COMBO_SPECS.map((s) => [s.id, s]));

for (const row of rows) {
  if (ui.PARAM_INDEX[row.id] !== row.index)
    problems.push(`${row.id}: index ${ui.PARAM_INDEX[row.id]} in the UI, ${row.index} in the plugin`);

  if (row.kind === "bool") {
    const spec = toggles.get(row.id);
    if (!spec) problems.push(`${row.id}: toggle missing from the UI`);
    else if (Number(spec.def) !== row.def)
      problems.push(`${row.id}: toggle default ${spec.def} in the UI, ${row.def} in the plugin`);
  } else if (row.kind === "choice") {
    const spec = combos.get(row.id);
    if (!spec) { problems.push(`${row.id}: combo missing from the UI`); continue; }
    const uiChoices = spec.choices;
    if (uiChoices.join("|") !== row.choices.join("|"))
      problems.push(`${row.id}: choices [${uiChoices.join(", ")}] in the UI, [${row.choices.join(", ")}] in the plugin`);
    if (Number(spec.def) !== row.def)
      problems.push(`${row.id}: choice default ${spec.def} in the UI, ${row.def} in the plugin`);
  } else {
    const spec = sliders.get(row.id);
    if (!spec) { problems.push(`${row.id}: slider missing from the UI`); continue; }
    if (!near(spec.min, row.min) || !near(spec.max, row.max))
      problems.push(`${row.id}: range ${spec.min}..${spec.max} in the UI, ${row.min}..${row.max} in the plugin`);
    if (!near(ui.defaultScaledValue(row.id), row.def, 1e-5))
      problems.push(`${row.id}: default ${ui.defaultScaledValue(row.id)} in the UI, ${row.def} in the plugin`);
    if (spec.interval !== undefined && !near(spec.interval, row.step))
      problems.push(`${row.id}: step ${spec.interval} in the UI, ${row.step} in the plugin`);
    if (!near(ui.skewFor(spec), row.skew, 1e-4))
      problems.push(`${row.id}: skew ${ui.skewFor(spec)} in the UI, ${row.skew} in the plugin`);
  }
}

const known = new Set(rows.map((r) => r.id));
for (const id of [...sliders.keys(), ...toggles.keys(), ...combos.keys()])
  if (!known.has(id)) problems.push(`${id}: in the UI table but not in the plugin`);

for (const problem of problems.slice(0, 60)) console.log(`MISMATCH  ${problem}`);
if (problems.length) {
  console.log(`FAIL: ${problems.length} mismatches between the UI table and the plugin layout (${rows.length} parameters)`);
  process.exit(1);
}
console.log(`PASS: the UI table matches the plugin layout for all ${rows.length} parameters (index, range, step, skew, default, choices)`);
