import { frameSrc, type Sequence } from "@/data/media";
import { loadOrder } from "./plan";

/** Decoded frames kept either side of the one on screen. A 2560x1440 bitmap is about 15 MB. */
const KEEP = 5;
/** Frames decoded ahead of the scroll, in the direction it is moving. */
const AHEAD = 3;
const MAX_DECODES = 4;
const MAX_FETCHES = 6;

/**
 * Plays a `Sequence` on a canvas by frame index. Frames stay compressed in
 * memory; only the few around the scroll position are decoded, off the main
 * thread. If the wanted frame is not decoded yet, the nearest one that is goes
 * up now and the wanted one replaces it as soon as it lands.
 */
export function createPlayer(canvas: HTMLCanvasElement, seq: Sequence, width: number) {
  const ctx = canvas.getContext("2d")!;
  const blobs: (Blob | undefined)[] = new Array(seq.count);
  const bitmaps = new Map<number, ImageBitmap>();
  const decoding = new Set<number>();
  const queue = loadOrder(seq.count);
  let fetching = 0;
  let want = 0;
  let shown = -1;
  let dir = 1;
  let live = true;

  const nearest = (has: (i: number) => boolean) => {
    for (let d = 0; d < seq.count; d++) {
      if (has(want + d)) return want + d;
      if (has(want - d)) return want - d;
    }
    return -1;
  };

  const paint = () => {
    const i = nearest((k) => bitmaps.has(k));
    if (i < 0 || i === shown) return;
    const frame = bitmaps.get(i)!;
    // object-fit: cover, by hand.
    const scale = Math.max(canvas.width / frame.width, canvas.height / frame.height);
    const sw = canvas.width / scale;
    const sh = canvas.height / scale;
    ctx.drawImage(frame, (frame.width - sw) / 2, (frame.height - sh) / 2, sw, sh, 0, 0, canvas.width, canvas.height);
    shown = i;
    // `shown` and `target` are read by the scrub benchmark; nothing in the page depends on them.
    canvas.dataset.shown = String(i / (seq.count - 1));
  };

  const decode = (i: number) => {
    const blob = blobs[i];
    if (!blob || bitmaps.has(i) || decoding.has(i) || decoding.size >= MAX_DECODES) return;
    decoding.add(i);
    createImageBitmap(blob).then(
      (bitmap) => {
        decoding.delete(i);
        if (!live || Math.abs(i - want) > KEEP) bitmap.close();
        else {
          bitmaps.set(i, bitmap);
          paint();
        }
        if (live) pump();
      },
      () => decoding.delete(i),
    );
  };

  const pump = () => {
    const first = nearest((k) => blobs[k] !== undefined);
    if (first < 0) return;
    decode(first);
    for (let k = 1; k <= AHEAD; k++) decode(want + dir * k);
    decode(want - dir);
    for (const [i, bitmap] of bitmaps) {
      if (Math.abs(i - want) > KEEP) {
        bitmap.close();
        bitmaps.delete(i);
      }
    }
  };

  const fetchNext = () => {
    while (live && fetching < MAX_FETCHES && queue.length) {
      const i = queue.shift()!;
      fetching++;
      fetch(frameSrc(seq, width, i))
        .then((res) => (res.ok ? res.blob() : undefined))
        .catch(() => undefined)
        .then((blob) => {
          fetching--;
          if (!live) return;
          blobs[i] = blob;
          if (blob && Math.abs(i - want) <= Math.max(AHEAD, Math.abs(shown - want))) pump();
          fetchNext();
        });
    }
  };

  return {
    load: fetchNext,
    seek(progress: number) {
      const next = Math.round(Math.min(1, Math.max(0, progress)) * (seq.count - 1));
      canvas.dataset.target = String(progress);
      if (next === want && shown === want) return;
      if (next !== want) dir = next > want ? 1 : -1;
      want = next;
      paint();
      pump();
    },
    resize(w: number, h: number) {
      canvas.width = w;
      canvas.height = h;
      ctx.imageSmoothingQuality = "high";
      shown = -1;
      paint();
    },
    dispose() {
      live = false;
      for (const bitmap of bitmaps.values()) bitmap.close();
      bitmaps.clear();
    },
  };
}
