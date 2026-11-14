#!/usr/bin/env node
/**
 * Records the landing-page clips from the real plugin editor.
 *
 *   npm run capture -- [--url http://localhost:5173] [--out public/media/ui] [clip ...]
 *
 * The editor is the React SPA in ../ui, served in a browser on its mock bridge
 * (`cd ui && npm run dev`, port 5173). With no clip names all 14 are recorded.
 *
 * Environment:
 *   TUBAMP_UI_URL    UI address (default http://localhost:5173; --url wins)
 *   PLAYWRIGHT_CORE  path to a playwright-core folder (default: first one found in ~/.npm/_npx)
 *   CHROME_PATH      chrome-headless-shell or Chrome binary (default: newest in ~/Library/Caches/ms-playwright)
 *   ffmpeg must be on PATH.
 *
 * Method: 1280x800 window at deviceScaleFactor 2; CDP screencast (JPEG q96, 2560x1600
 * frames with real timestamps) is retimed to 30 fps by ffmpeg. Board clips are scaled
 * to 1920x1200, editor clips keep 2560x592 (the 296 px band above the 36 px status bar).
 * mp4 = H.264 yuv420p +faststart, webm = VP9, no audio, poster = first frame.
 * A cursor overlay is drawn into the page and the "mock bridge" status text is hidden.
 */
import { resolve, dirname } from "node:path";
import { fileURLToPath } from "node:url";
import { openSession, VIEW, STATUS_BAR, EDITOR_BAND } from "./capture/driver.mjs";
import { encode } from "./capture/encode.mjs";
import { CLIPS } from "./capture/clips.mjs";

const here = dirname(fileURLToPath(import.meta.url));
const args = process.argv.slice(2);
const opt = (name) => {
  const i = args.indexOf(name);
  return i >= 0 ? args.splice(i, 2)[1] : undefined;
};
const url = opt("--url") ?? process.env.TUBAMP_UI_URL ?? "http://localhost:5173";
const outDir = resolve(opt("--out") ?? resolve(here, "../public/media/ui"));
const wanted = args.filter((a) => !a.startsWith("--"));

const unknown = wanted.filter((n) => !CLIPS.some((c) => c.name === n));
if (unknown.length) {
  console.error(`unknown clip(s): ${unknown.join(", ")}\navailable: ${CLIPS.map((c) => c.name).join(", ")}`);
  process.exit(1);
}
const todo = CLIPS.filter((c) => !wanted.length || wanted.includes(c.name));

try {
  const r = await fetch(url);
  if (!r.ok) throw new Error(`HTTP ${r.status}`);
} catch (e) {
  console.error(`capture: cannot reach the UI at ${url} (${e.message}). Start it with \`cd ui && npm run dev\` or pass --url.`);
  process.exit(1);
}

const s = await openSession(url);
let failed = 0;
for (const clip of todo) {
  try {
    await s.fresh();
    await clip.setup(s);
    const { frames, seconds } = await s.record(() => clip.run(s));
    const crop = clip.kind === "editor" ? { y: VIEW.height - STATUS_BAR - EDITOR_BAND, h: EDITOR_BAND } : null;
    const r = encode(frames, seconds, { name: clip.name, outDir, crop, outWidth: crop ? null : 1920 });
    console.log(`${clip.name.padEnd(12)} ${seconds.toFixed(1)}s  ${r.frames} frames  mp4 ${r.mp4} KB  webm ${r.webm} KB  jpg ${r.jpg} KB`);
  } catch (e) {
    failed++;
    console.error(`${clip.name}: ${e.message}`);
  }
}
if (s.errors.length) console.warn(`page errors:\n  ${[...new Set(s.errors)].slice(0, 5).join("\n  ")}`);
await s.close();
process.exit(failed ? 1 : 0);
