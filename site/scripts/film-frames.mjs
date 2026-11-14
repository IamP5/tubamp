#!/usr/bin/env node
/**
 * Turns a master video into the numbered AVIF stills a scroll-scrubbed film
 * plays from (`Sequence` in src/data/media.ts).
 *
 *   npm run film:frames -- <master.mp4> [--name capture] [--widths 1920,2560] [--quality 45]
 *
 * Writes public/media/film/<name>/<width>/001.avif onwards, and
 * public/media/film/<name>.jpg from the first frame. Needs `ffmpeg` on PATH;
 * `sharp` comes with Next. Prints the frame count: put it in the Sequence.
 */
import { execFileSync } from "node:child_process";
import { mkdirSync, mkdtempSync, readdirSync, rmSync, statSync } from "node:fs";
import { tmpdir } from "node:os";
import { dirname, join, resolve } from "node:path";
import { fileURLToPath } from "node:url";
import sharp from "sharp";

const args = process.argv.slice(2);
const opt = (name, fallback) => {
  const i = args.indexOf(name);
  return i >= 0 ? args.splice(i, 2)[1] : fallback;
};
const name = opt("--name", "capture");
const widths = opt("--widths", "1920,2560").split(",").map(Number);
const quality = Number(opt("--quality", "45"));
const [master] = args;
if (!master) {
  console.error("film-frames: give the master video's path");
  process.exit(1);
}

const film = resolve(dirname(fileURLToPath(import.meta.url)), "../public/media/film");
const stills = mkdtempSync(join(tmpdir(), "film-frames-"));
try {
  execFileSync("ffmpeg", ["-v", "error", "-i", master, "-fps_mode", "passthrough", join(stills, "%03d.png")], { stdio: "inherit" });
  const frames = readdirSync(stills).sort();
  await sharp(join(stills, frames[0])).resize({ width: widths[0] }).jpeg({ quality: 82, mozjpeg: true }).toFile(join(film, `${name}.jpg`));
  for (const width of widths) {
    const out = join(film, name, String(width));
    rmSync(out, { recursive: true, force: true });
    mkdirSync(out, { recursive: true });
    let bytes = 0;
    for (const frame of frames) {
      const dest = join(out, frame.replace(".png", ".avif"));
      await sharp(join(stills, frame)).resize({ width }).avif({ quality, effort: 5 }).toFile(dest);
      bytes += statSync(dest).size;
    }
    console.log(`${name}/${width}: ${frames.length} frames, ${(bytes / 1e6).toFixed(1)} MB`);
  }
} finally {
  rmSync(stills, { recursive: true, force: true });
}
