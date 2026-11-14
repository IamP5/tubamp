/**
 * The ink sheet the wordmark is cut out of, painted on a canvas that lies over
 * the footage. Glyph positions are read from the DOM wordmark so CSS owns the
 * layout (size, kerning, tracking, line breaks) and the canvas only repaints
 * it: an ink fill with the letters erased, at whatever camera scale the scroll
 * asks for. Vector text repainted per frame stays sharp at any zoom.
 */

type Glyph = { ch: string; x: number; y: number };

type Wordmark = { font: string; glyphs: Glyph[]; ascent: number; descent: number };

/** A point inside one letter, and the scale at which that letter covers the whole canvas. */
type Focus = { x: number; y: number; scale: number };

function readWordmark(el: HTMLElement, origin: DOMRect, ctx: CanvasRenderingContext2D): Wordmark {
  const style = getComputedStyle(el);
  const font = `${style.fontWeight} ${style.fontSize} ${style.fontFamily}`;
  ctx.font = font;
  const metrics = ctx.measureText(el.textContent ?? "");
  const ascent = metrics.fontBoundingBoxAscent;
  const descent = metrics.fontBoundingBoxDescent;

  const glyphs: Glyph[] = [];
  const range = document.createRange();
  const walker = document.createTreeWalker(el, NodeFilter.SHOW_TEXT);
  for (let node = walker.nextNode(); node; node = walker.nextNode()) {
    const text = node.textContent ?? "";
    for (let i = 0; i < text.length; i++) {
      range.setStart(node, i);
      range.setEnd(node, i + 1);
      const r = range.getBoundingClientRect();
      glyphs.push({ ch: text[i], x: r.left - origin.left, y: r.top - origin.top + ascent });
    }
  }
  return { font, glyphs, ascent, descent };
}

function fillGlyphs(ctx: CanvasRenderingContext2D, word: Wordmark, stroke = false) {
  ctx.font = word.font;
  ctx.textBaseline = "alphabetic";
  for (const g of word.glyphs) {
    if (stroke) ctx.strokeText(g.ch, g.x, g.y);
    else ctx.fillText(g.ch, g.x, g.y);
  }
}

/**
 * Finds the letter stem to fly through: the tallest solid vertical run in the
 * wordmark, weighted toward the middle of the word, so the camera ends inside
 * a straight stroke (the b's stem in Geist) instead of grazing a curve.
 */
function findFocus(word: Wordmark, width: number, height: number): Focus {
  const xs = word.glyphs.map((g) => g.x);
  const left = Math.floor(Math.min(...xs));
  const top = Math.floor(Math.min(...word.glyphs.map((g) => g.y)) - word.ascent);
  const bottom = Math.ceil(Math.max(...word.glyphs.map((g) => g.y)) + word.descent);
  const w = Math.ceil(Math.max(...xs) + word.ascent) - left;
  const h = bottom - top;

  const off = document.createElement("canvas");
  off.width = w;
  off.height = h;
  const octx = off.getContext("2d", { willReadFrequently: true })!;
  octx.translate(-left, -top);
  fillGlyphs(octx, word);
  const alpha = octx.getImageData(0, 0, w, h).data;

  const runs = new Array<{ len: number; start: number }>(w);
  for (let x = 0; x < w; x++) {
    let best = { len: 0, start: 0 };
    let start = -1;
    for (let y = 0; y <= h; y++) {
      const solid = y < h && alpha[(y * w + x) * 4 + 3] > 127;
      if (solid && start < 0) start = y;
      if (!solid && start >= 0) {
        if (y - start > best.len) best = { len: y - start, start };
        start = -1;
      }
    }
    runs[x] = best;
  }

  const mid = w / 2;
  let pick = 0;
  let score = -1;
  for (let x = 0; x < w; x++) {
    const s = runs[x].len * (1 - (0.6 * Math.abs(x - mid)) / w);
    if (s > score) {
      score = s;
      pick = x;
    }
  }

  const tall = runs[pick].len * 0.92;
  let l = pick;
  let r = pick;
  while (l > 0 && runs[l - 1].len >= tall) l--;
  while (r < w - 1 && runs[r + 1].len >= tall) r++;
  const stemW = r - l + 1;
  const stemH = runs[pick].len;

  return {
    x: left + (l + r + 1) / 2,
    y: top + runs[pick].start + stemH / 2,
    scale: Math.max(width / stemW, height / stemH) * 1.2,
  };
}

const easeOutCubic = (t: number) => 1 - (1 - t) ** 3;

export type Mask = {
  /** Re-reads layout and size; call after resize or font load. */
  measure(): void;
  /** Paints the camera at t in 0..1 (0 = the wordmark as laid out, 1 = inside the stem). */
  paint(t: number): void;
};

/** `ink` fills the sheet; `edge` draws the cut edge, a hairline that keeps the letters legible where the footage is dark. */
export type MaskColours = { ink: string; edge: string };

export function createMask(
  canvas: HTMLCanvasElement,
  wordEl: HTMLElement,
  { ink, edge }: MaskColours,
  zoom: boolean,
): Mask {
  const ctx = canvas.getContext("2d")!;
  let word: Wordmark | null = null;
  let focus: Focus | null = null;
  let w = 0;
  let h = 0;
  let dpr = 1;
  let t = 0;

  function paint(next: number) {
    t = next;
    if (!word) return;
    const inside = focus !== null && t >= 1;
    canvas.style.visibility = inside ? "hidden" : "";
    if (inside) return;

    ctx.setTransform(1, 0, 0, 1, 0, 0);
    ctx.globalCompositeOperation = "source-over";
    ctx.fillStyle = ink;
    ctx.fillRect(0, 0, canvas.width, canvas.height);

    let s = 1;
    if (focus && t > 0) {
      // Scale grows exponentially so the push reads as constant speed; the aim
      // point drifts to centre early, as a camera turns before it moves in.
      s = focus.scale ** t;
      const e = easeOutCubic(t);
      const sx = focus.x + (w / 2 - focus.x) * e;
      const sy = focus.y + (h / 2 - focus.y) * e;
      ctx.setTransform(dpr * s, 0, 0, dpr * s, dpr * (sx - s * focus.x), dpr * (sy - s * focus.y));
    } else {
      ctx.setTransform(dpr, 0, 0, dpr, 0, 0);
    }
    // The edge is stroked before the letters are erased: the erase removes its
    // inner half and the lines where a glyph's contours overlap, leaving a 1px
    // rim on the ink side only.
    if (edge) {
      ctx.strokeStyle = edge;
      ctx.lineWidth = 2 / s;
      fillGlyphs(ctx, word, true);
    }
    ctx.globalCompositeOperation = "destination-out";
    fillGlyphs(ctx, word);
  }

  function measure() {
    const rect = canvas.getBoundingClientRect();
    w = rect.width;
    h = rect.height;
    dpr = Math.min(window.devicePixelRatio || 1, 2);
    canvas.width = Math.round(w * dpr);
    canvas.height = Math.round(h * dpr);
    word = readWordmark(wordEl, rect, ctx);
    focus = zoom ? findFocus(word, w, h) : null;
    paint(t);
  }

  return { measure, paint };
}
