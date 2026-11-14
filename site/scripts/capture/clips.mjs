// The clip list: what to do on the real editor for each file in public/media/ui.
// `kind: "board"` records the whole 1280x800 window; `"editor"` crops the editor band.
// `setup` runs before recording starts (not in the clip); `run` is what the clip shows.
// Each run ends where it began so the loop does not jump, except chain-build
// (the added blocks stay) and t3k (the loaded capture stays).

const pause = (s, ms) => s.sleep(ms);

/** Step the preset selector forward n times, un-recorded. */
async function gotoPreset(s, n) {
  for (let i = 0; i < n; i++) {
    await s.page.click('button[aria-label="Next preset"]');
    await s.sleep(450);
  }
}
const tile = (token) => `[role=group][data-block="${token}"]`;
const CLEAN_CHORUS = 2; // Next-preset clicks from the first preset

async function pickFromAddMenu(s, label) {
  await s.click('button[aria-label="Add a block to the chain"]', { ms: 550 });
  await pause(s, 350);
  await s.click(`[role=menuitem]:has-text("${label}")`, { ms: 350 });
  await pause(s, 450);
}

/** Editor-band clip: pre-select the block, wiggle two controls, step the type and back. */
const fx = (name, token, { step = "type", knobs, dy = -64 }) => ({
  name: `fx-${name}`,
  kind: "editor",
  async setup(s) {
    await gotoPreset(s, CLEAN_CHORUS);
    await s.page.click(tile(token));
    await s.sleep(700);
    // Bypassed blocks show a dimmed display: switch it on so the picture moves.
    const off = s.page.locator('button[aria-label="Enable this block"]');
    if (await off.count()) { await off.first().click(); await s.sleep(500); }
    await s.moveTo(980, 440, 300);
  },
  async run(s) {
    await pause(s, 500);
    for (const k of knobs) {
      await s.knob(k, dy);
      await pause(s, 300);
    }
    if (step) {
      await s.click(`button[aria-label="Next ${step}"]`, { band: true, ms: 600 });
      await pause(s, 1100);
      await s.click(`button[aria-label="Previous ${step}"]`, { band: true, ms: 400 });
      await pause(s, 700);
    }
  },
});

export const CLIPS = [
  {
    name: "chain-build",
    kind: "board",
    async setup() {},
    async run(s) {
      await pause(s, 500);
      await pickFromAddMenu(s, "Gate");
      await pickFromAddMenu(s, "Delay");
      await pause(s, 400);
      // Drag the last block to the front of the chain.
      const from = await s.box(tile("delay"));
      const to = await s.box(tile("gate"));
      await s.dragBetween(from.x, from.y, to.x - 40, to.y, 800);
      await pause(s, 1000);
    },
  },
  {
    name: "presets",
    kind: "board",
    async setup() {},
    async run(s) {
      await pause(s, 500);
      for (let i = 0; i < 3; i++) {
        await s.clickLabel("Next preset", { ms: i ? 250 : 600 });
        await pause(s, 900);
      }
      for (let i = 0; i < 3; i++) {
        await s.clickLabel("Previous preset", { ms: i ? 250 : 500 });
        await pause(s, i < 2 ? 450 : 500);
      }
    },
  },
  {
    name: "t3k",
    kind: "board",
    async setup(s) { await gotoPreset(s, CLEAN_CHORUS); },
    async run(s) {
      await pause(s, 400);
      await s.clickLabel("TONE3000", { ms: 600 });
      await pause(s, 900);
      await s.wheel(1100, 450, 420, 6, 80);
      await pause(s, 300);
      await s.wheel(1100, 450, -420, 6, 80);
      await pause(s, 300);
      await s.click('button:text-is("Load")', { nth: 2, ms: 500 });
      await pause(s, 900);
      await s.clickLabel("Close browser", { ms: 600 });
      await pause(s, 500);
    },
  },
  {
    name: "amp",
    kind: "board",
    async setup(s) { await gotoPreset(s, CLEAN_CHORUS); },
    async run(s) {
      await pause(s, 400);
      await s.click(tile("amp"), { ms: 600 });
      await pause(s, 700);
      await s.clickLabel("Next capture", { band: true, ms: 600 });
      await pause(s, 1100);
      await s.clickLabel("Previous capture", { band: true, ms: 350 });
      await pause(s, 900);
      await s.knob("Bass", -80, { ms: 450 });
      await pause(s, 300);
      await s.clickLabel("Close panel", { ms: 500 });
      await pause(s, 300);
    },
  },
  {
    name: "split",
    kind: "board",
    // The mock's "Dangling Split" preset has no split block, so the dual-amp
    // block (split + two amps + mixer) is added before recording starts.
    async setup(s) {
      await s.page.click('button[aria-label="Add a block to the chain"]');
      await s.sleep(400);
      await s.page.click('[role=menuitem]:has-text("Dual amp")');
      await s.sleep(1000);
      await s.moveTo(640, 560, 300);
    },
    async run(s) {
      await pause(s, 500);
      await s.click(tile("split"), { ms: 600 });
      await pause(s, 1300);
      await s.click(tile("amp2"), { ms: 500 });
      await pause(s, 1300);
      await s.click(tile("mix"), { ms: 500 });
      await pause(s, 1000);
      await s.clickLabel("Close panel", { ms: 500 });
      await pause(s, 500);
    },
  },
  {
    name: "compare",
    kind: "board",
    async setup(s) {
      await gotoPreset(s, CLEAN_CHORUS);
      await s.page.click(tile("amp"));
      await s.sleep(800);
      await s.moveTo(1000, 330, 300);
    },
    // The mock bridge only tracks which slot is active and holds state; it does
    // not swap parameter values between slots, so no knob is moved (it would
    // not change back). Store into B, then flip A, B, A.
    async run(s) {
      await pause(s, 500);
      await s.click('button[aria-label="Save to A or B"]', { ms: 800 });
      await pause(s, 400);
      await s.click('[role=menuitem]:has-text("Save to B")', { ms: 350 });
      await pause(s, 700);
      await s.click('[role=tab]:has-text("B")', { ms: 450 });
      await pause(s, 800);
      await s.click('[role=tab]:has-text("A")', { ms: 350 });
      await pause(s, 800);
      await s.click('[role=tab]:has-text("B")', { ms: 350 });
      await pause(s, 700);
      await s.click('[role=tab]:has-text("A")', { ms: 350 });
      await pause(s, 700);
    },
  },
  fx("gate", "gate", { step: "mode", knobs: ["Threshold", "Range"] }),
  fx("comp", "comp", { knobs: ["Threshold", "Ratio"] }),
  fx("drive", "drive", { knobs: ["Drive", "Tone"] }),
  fx("cab", "cab", { step: null, knobs: ["Low cut", "High cut", "Mix"] }),
  fx("eq", "eq", { knobs: ["Bass", "Treble"] }),
  fx("mod", "mod", { knobs: ["Rate", "Depth"] }),
  fx("delay", "delay", { knobs: ["Time", "Feedback"] }),
  fx("reverb", "reverb", { knobs: ["Decay", "Mix"] }),
];
