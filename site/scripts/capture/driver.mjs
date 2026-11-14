// Browser, recording and pointer helpers for scripts/capture.mjs.
import { createRequire } from "node:module";
import { homedir } from "node:os";
import { existsSync } from "node:fs";
import { join } from "node:path";

export const VIEW = { width: 1280, height: 800 };
export const STATUS_BAR = 36; // px, bottom of the window
export const EDITOR_BAND = 296; // px, editor panel above the status bar

const sleep = (ms) => new Promise((r) => setTimeout(r, ms));
export { sleep };

function die(msg) {
  console.error(`capture: ${msg}`);
  process.exit(1);
}

/** Resolve playwright-core and a Chromium binary from env vars or common caches. */
export function loadPlaywright() {
  let dir = process.env.PLAYWRIGHT_CORE;
  if (!dir) {
    const npx = join(homedir(), ".npm/_npx");
    try {
      const fs = require_fs();
      for (const d of fs.readdirSync(npx)) {
        const p = join(npx, d, "node_modules/playwright-core");
        if (existsSync(p)) { dir = p; break; }
      }
    } catch {}
  }
  if (!dir || !existsSync(dir)) {
    die(
      "playwright-core not found. Set PLAYWRIGHT_CORE to its folder, e.g.\n" +
        "  PLAYWRIGHT_CORE=$HOME/.npm/_npx/<hash>/node_modules/playwright-core\n" +
        "(get one with `npx playwright-core --version`, or `npm i --prefix /tmp/pw playwright-core`).",
    );
  }
  let exe = process.env.CHROME_PATH;
  if (!exe) {
    const cache = join(homedir(), "Library/Caches/ms-playwright");
    try {
      const fs = require_fs();
      const shells = fs.readdirSync(cache).filter((n) => n.startsWith("chromium_headless_shell")).sort().reverse();
      for (const s of shells) {
        const sub = fs.readdirSync(join(cache, s)).find((n) => n.startsWith("chrome-headless-shell"));
        const p = sub && join(cache, s, sub, "chrome-headless-shell");
        if (p && existsSync(p)) { exe = p; break; }
      }
    } catch {}
  }
  if (!exe || !existsSync(exe)) {
    die("no Chromium binary found. Set CHROME_PATH to a chrome-headless-shell or Chrome executable (`npx playwright-core install chromium-headless-shell` downloads one).");
  }
  const { chromium } = createRequire(import.meta.url)(dir);
  return { chromium, exe };
}
function require_fs() { return createRequire(import.meta.url)("node:fs"); }

/**
 * Open the UI at 2x device scale and return a session with pointer helpers and
 * a screencast recorder. `record(fn)` returns the frames captured while fn ran.
 */
export async function openSession(url) {
  const { chromium, exe } = loadPlaywright();
  const browser = await chromium.launch({ headless: true, executablePath: exe });
  const ctx = await browser.newContext({ viewport: VIEW, deviceScaleFactor: 2 });
  const page = await ctx.newPage();
  const errors = [];
  page.on("pageerror", (e) => errors.push(String(e)));
  page.on("console", (m) => m.type() === "error" && errors.push(m.text()));
  await page.goto(url, { waitUntil: "networkidle" });
  const prepare = async () => {
    await page.addStyleTag({ content: "*{caret-color:transparent!important}" });
    await page.evaluate(() => {
      // Hide the bridge label: it names the mock, not the product. Re-applied
      // on a timer because the status bar re-renders.
      const hide = () => {
        for (const el of document.querySelectorAll("*"))
          if (el.children.length === 0 && /mock bridge/i.test(el.textContent || "")) el.style.visibility = "hidden";
      };
      hide();
      setInterval(hide, 100);
      // Pointer overlay, since headless video has no cursor.
      const c = document.createElement("div");
      c.id = "__cap_cursor";
      c.style.cssText =
        "position:fixed;left:0;top:0;width:18px;height:18px;margin:-2px 0 0 -2px;z-index:2147483647;pointer-events:none;transition:transform 60ms linear;transform:translate(-40px,-40px)";
      c.innerHTML =
        '<svg width="18" height="18" viewBox="0 0 18 18"><path d="M2 1l12 8-5.2 1.1L11.6 16 9.2 17 6.5 11.4 2.6 14.6z" fill="#fff" stroke="#000" stroke-width="1.1" stroke-linejoin="round"/></svg>';
      document.body.appendChild(c);
      addEventListener("mousemove", (e) => (c.style.transform = `translate(${e.clientX}px,${e.clientY}px)`), true);
      addEventListener("mousedown", () => (c.firstChild.style.transform = "scale(.86)"), true);
      addEventListener("mouseup", () => (c.firstChild.style.transform = ""), true);
    });
  };
  await prepare();
  await page.waitForTimeout(500);

  const mouse = { x: 640, y: 400 };
  const cdp = await ctx.newCDPSession(page);

  const s = {
    page, errors, mouse,
    sleep,
    async close() { await ctx.close(); await browser.close(); },

    /** Reload the page to a clean mock bridge state. */
    async fresh() {
      await page.reload({ waitUntil: "networkidle" });
      await prepare();
      mouse.x = 640; mouse.y = 400;
      await page.mouse.move(mouse.x, mouse.y);
      await sleep(400);
    },

    /** Centre of an element, optionally restricted to the editor band. */
    async box(sel, { band = false, nth = 0, knob = false } = {}) {
      const loc = page.locator(sel);
      const n = await loc.count();
      const found = [];
      for (let i = 0; i < n; i++) {
        const b = await loc.nth(i).boundingBox();
        if (!b) continue;
        if (knob && b.width < 28) continue; // display handles share the knob's label
        if (band && (b.y < VIEW.height - STATUS_BAR - EDITOR_BAND || b.y > VIEW.height - STATUS_BAR)) continue;
        found.push(b);
      }
      if (!found[nth]) throw new Error(`target not found: ${sel}${band ? " (band)" : ""}`);
      const b = found[nth];
      return { x: b.x + b.width / 2, y: b.y + b.height / 2, w: b.width, h: b.height };
    },

    /** Ease the pointer to (x, y) over `ms`. */
    async moveTo(x, y, ms = 650) {
      const x0 = mouse.x, y0 = mouse.y;
      const t0 = Date.now();
      for (;;) {
        const t = Math.min(1, (Date.now() - t0) / ms);
        const e = t < 0.5 ? 4 * t * t * t : 1 - Math.pow(-2 * t + 2, 3) / 2;
        mouse.x = x0 + (x - x0) * e; mouse.y = y0 + (y - y0) * e;
        await page.mouse.move(mouse.x, mouse.y);
        if (t >= 1) break;
        await sleep(8);
      }
    },
    async hover(sel, opts = {}) {
      const b = await s.box(sel, opts);
      await s.moveTo(b.x, b.y, opts.ms ?? 650);
      return b;
    },
    async click(sel, opts = {}) {
      const b = await s.hover(sel, opts);
      await sleep(120);
      await page.mouse.down(); await sleep(70); await page.mouse.up();
      return b;
    },
    /** Press on `from`, ease to `to`, release. */
    async dragBetween(fx, fy, tx, ty, ms = 900) {
      await s.moveTo(fx, fy, 600);
      await sleep(100);
      await page.mouse.down(); await sleep(120);
      await s.moveTo(tx, ty, ms);
      await sleep(150);
      await page.mouse.up();
    },
    /** Vertical drag on a knob (negative dy turns it up). With `back`, returns to the start in the same press. */
    async knob(label, dy, { ms = 550, back = true } = {}) {
      const b = await s.hover(`[aria-label="${label}"]`, { band: true, knob: true, ms: 450 });
      await sleep(80);
      await page.mouse.down(); await sleep(60);
      await s.moveTo(b.x, b.y + dy, ms);
      await sleep(back ? 120 : 60);
      if (back) await s.moveTo(b.x, b.y, ms);
      await sleep(60);
      await page.mouse.up();
    },
    async wheel(x, y, dy, steps = 6, gap = 90) {
      await s.moveTo(x, y, 500);
      for (let i = 0; i < steps; i++) { await page.mouse.wheel(0, dy / steps); await sleep(gap); }
    },
    async clickLabel(label, opts = {}) { return s.click(`button[aria-label="${label}"]`, opts); },

    /**
     * Record while `fn` runs by grabbing 2x screenshots back to back (about 30 per
     * second). The CDP screencast only delivers CSS-pixel frames, so it is not used.
     * Returns { frames: [{data: Buffer, t: seconds}], seconds }.
     */
    async record(fn) {
      const frames = [];
      let running = true;
      const loop = (async () => {
        while (running) {
          const t = Date.now() / 1000;
          const r = await cdp.send("Page.captureScreenshot", { format: "jpeg", quality: 95, optimizeForSpeed: true, clip: { x: 0, y: 0, width: VIEW.width, height: VIEW.height, scale: 2 } });
          frames.push({ data: Buffer.from(r.data, "base64"), t });
        }
      })();
      await sleep(200);
      const t0 = Date.now();
      let seconds;
      try {
        await fn();
        seconds = (Date.now() - t0) / 1000;
        await sleep(200);
      } finally {
        running = false;
        await loop.catch(() => {});
      }
      // Drop the lead-in so the clip starts when fn starts.
      const start = t0 / 1000 - 0.0;
      const kept = frames.filter((f, i) => (frames[i + 1]?.t ?? Infinity) > start);
      return { frames: kept, seconds };
    },
  };
  return s;
}
