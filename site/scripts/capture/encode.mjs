// Turn screencast frames into mp4 + webm + poster with ffmpeg.
import { mkdirSync, writeFileSync, rmSync, statSync } from "node:fs";
import { join } from "node:path";
import { tmpdir } from "node:os";
import { spawnSync } from "node:child_process";

function ff(args) {
  const r = spawnSync("ffmpeg", ["-hide_banner", "-loglevel", "error", "-y", ...args], { encoding: "utf8" });
  if (r.status !== 0) throw new Error(`ffmpeg failed: ${r.stderr}`);
}

/**
 * @param frames  [{data: Buffer(jpeg), t: seconds}]
 * @param seconds total length of the clip
 * @param opts    { name, outDir, crop: {y, h}|null, outWidth }
 */
export function encode(frames, seconds, { name, outDir, crop, outWidth }) {
  if (frames.length < 2) throw new Error(`${name}: only ${frames.length} frames captured`);
  const work = join(tmpdir(), `tubamp-capture-${name}-${process.pid}`);
  rmSync(work, { recursive: true, force: true });
  mkdirSync(work, { recursive: true });
  // The screencast only delivers frames when the page changes: hold each one until the next.
  const t0 = frames[0].t; // first frame is held from the clip start
  let list = "ffconcat version 1.0\n";
  frames.forEach((f, i) => {
    writeFileSync(join(work, `f${i}.jpg`), f.data);
    const end = i + 1 < frames.length ? frames[i + 1].t : t0 + seconds + 0.25;
    list += `file f${i}.jpg\nduration ${Math.max(0.001, end - f.t).toFixed(4)}\n`;
  });
  list += `file f${frames.length - 1}.jpg\n`;
  writeFileSync(join(work, "list.txt"), list);

  // Frames are 2560x1600 (2x). Crop the editor band at full size, or scale the board.
  const vf = [
    "fps=30",
    crop ? `crop=2560:${crop.h * 2}:0:${crop.y * 2}` : null,
    outWidth ? `scale=${outWidth}:-2:flags=lanczos` : null,
    "scale=in_range=full:out_range=tv", // JPEG frames are full range; deliver limited-range yuv420p
    "format=yuv420p",
  ].filter(Boolean).join(",");
  const input = ["-f", "concat", "-safe", "0", "-i", join(work, "list.txt"), "-t", seconds.toFixed(3), "-vf", vf, "-an"];
  mkdirSync(outDir, { recursive: true });
  const out = (ext) => join(outDir, `${name}.${ext}`);
  ff([...input, "-c:v", "libx264", "-preset", "slow", "-crf", String(crop ? 18 : 24), "-pix_fmt", "yuv420p", "-movflags", "+faststart", "-r", "30", out("mp4")]);
  ff([...input, "-c:v", "libvpx-vp9", "-b:v", "0", "-crf", String(crop ? 30 : 34), "-row-mt", "1", "-deadline", "good", "-cpu-used", "2", "-r", "30", out("webm")]);
  ff(["-i", out("mp4"), "-frames:v", "1", "-q:v", "3", out("jpg")]);
  rmSync(work, { recursive: true, force: true });
  const kb = (e) => Math.round(statSync(out(e)).size / 1024);
  return { frames: frames.length, mp4: kb("mp4"), webm: kb("webm"), jpg: kb("jpg") };
}
