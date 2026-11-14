#!/usr/bin/env node
/**
 * Builds the plugin editor for the web and copies it to public/demo/, where the
 * site serves it at /demo/index.html (run on its mock bridge, no plugin needed).
 *
 *   npm run build:demo [-- --ui <dir>] [-- --skip-install]
 *
 * Steps: `npm ci` in the repo's ui/ folder, `vite build` into a temporary folder
 * (ui/dist is left alone: the plugin build embeds it), then copy to public/demo/.
 * Vite's config uses relative asset paths, so the bundle works from a subfolder.
 *
 *   --ui <dir>       build another copy of the UI (default: ../ui)
 *   --skip-install   do not run `npm ci` (dependencies already present)
 */
import { cpSync, existsSync, mkdtempSync, readFileSync, rmSync } from "node:fs";
import { tmpdir } from "node:os";
import { dirname, join, resolve } from "node:path";
import { fileURLToPath } from "node:url";
import { spawnSync } from "node:child_process";

const here = dirname(fileURLToPath(import.meta.url));
const args = process.argv.slice(2);
const opt = (name) => {
  const i = args.indexOf(name);
  return i >= 0 ? args.splice(i, 2)[1] : undefined;
};
const skipInstall = args.includes("--skip-install");
const uiDir = resolve(opt("--ui") ?? join(here, "../../ui"));
const dest = resolve(here, "../public/demo");

if (!existsSync(join(uiDir, "package.json"))) {
  console.error(`build-demo: no package.json in ${uiDir}`);
  process.exit(1);
}
const run = (cmd, cmdArgs) => {
  console.log(`> ${cmd} ${cmdArgs.join(" ")}   (in ${uiDir})`);
  const r = spawnSync(cmd, cmdArgs, { cwd: uiDir, stdio: "inherit" });
  if (r.status !== 0) {
    console.error(`build-demo: \`${cmd} ${cmdArgs.join(" ")}\` failed`);
    process.exit(r.status ?? 1);
  }
};

if (!skipInstall) run("npm", ["ci"]);
const tmp = mkdtempSync(join(tmpdir(), "tubamp-demo-"));
try {
  run("npx", ["vite", "build", "--outDir", tmp, "--emptyOutDir"]);
  if (!existsSync(join(tmp, "index.html"))) {
    console.error("build-demo: the build produced no index.html");
    process.exit(1);
  }
  rmSync(dest, { recursive: true, force: true });
  cpSync(tmp, dest, { recursive: true });
} finally {
  rmSync(tmp, { recursive: true, force: true });
}
const html = readFileSync(join(dest, "index.html"), "utf8");
if (/(src|href)="\//.test(html)) console.warn("build-demo: index.html has absolute asset paths; /demo/ will not load them");
console.log(`build-demo: copied to ${dest}`);
