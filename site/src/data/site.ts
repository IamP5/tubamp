/**
 * Every fact and string the page states about the plugin. Sourced from the
 * repository README and docs; change it there first, then here.
 */
export type Family = "gain" | "dynamics" | "tone" | "mod" | "time";

export type Block = {
  id: string;
  name: string;
  family: Family;
  does: string;
  types?: readonly string[];
};

export const SITE = {
  name: "tubamp",
  repoUrl: "https://github.com/IamP5/tubamp",
  tagline: "Your next amp is a file.",
  line: "An amp plugin for Logic Pro.",
  what: "tubamp plays captures of real amplifiers in Logic Pro. Build the rest of the rig around them in any order, with room for your own plugins.",
  platforms: "macOS 11 or later. Apple Silicon and Intel.",
  hosts: "Logic Pro. Other Audio Unit hosts are untested.",
} as const;

/** The blocks in their default order. They can be reordered, removed and added again. */
export const CHAIN: readonly Block[] = [
  { id: "gate", name: "Gate", family: "dynamics", does: "Closes on the noise between notes." },
  { id: "comp", name: "Compressor", family: "dynamics", does: "Evens out the picking hand." },
  { id: "drive", name: "Drive", family: "gain", does: "Pushes the front of the amp.", types: ["Screamer"] },
  { id: "amp", name: "Amp", family: "gain", does: "A neural capture of a real amplifier." },
  { id: "cab", name: "Cab", family: "tone", does: "An impulse response of a real speaker and microphone." },
  { id: "eq", name: "EQ", family: "tone", does: "Parametric or graphic, after the cab." },
  { id: "mod", name: "Modulation", family: "mod", does: "Chorus, phaser, tremolo.", types: ["Chorus", "Phaser", "Tremolo"] },
  { id: "delay", name: "Delay", family: "time", does: "Digital, tape or analog repeats, in stereo.", types: ["Digital", "Tape", "Analog"] },
  { id: "reverb", name: "Reverb", family: "time", does: "Plate, hall, room or spring.", types: ["Plate", "Hall", "Room", "Spring"] },
];

export const NAV = [
  { href: "#chain", label: "Chain" },
  { href: "#captures", label: "Captures" },
  { href: "#effects", label: "Effects" },
  { href: "#try", label: "Try it" },
  { href: "#faq", label: "Questions" },
] as const;

export const CHAPTERS = {
  chain: {
    kicker: "Chain",
    headline: ["Build the chain.", "Any order."],
    body: "Nine blocks on a board. Drag one somewhere else, remove it, add it again, add a second delay. The order is part of the sound and it is saved with your project.",
    board: {
      label: "The chain. Drag a block, or focus one and use the arrow keys, to change the order.",
      waveform: "The signal this order gives.",
      rest: "The default order.",
      tail: "Reverb now feeds the drive, so the whole tail distorts.",
      first: "{block} now runs first.",
      after: "{block} now runs after {other}.",
      position: "{block}, {n} of {total}",
      earlier: "Move {block} earlier",
      later: "Move {block} later",
      reset: "Reset",
    },
  },
  captures: {
    kicker: "Captures",
    headline: ["A real amp,", "captured."],
    body: "A capture is a file that holds how one real amplifier responds to a guitar. tubamp plays Neural Amp Modeler captures, the newer A2 format included. Browse the TONE3000 library inside the plugin with your own TONE3000 API key, or load any .nam file with no setup.",
    facts: [
      "NAM A1, A2, slimmable and LSTM captures",
      "Resamples when the session rate differs from the capture",
      "Optional loudness match across captures",
      "Cab impulse responses, from TONE3000 or your own files",
    ],
    attribution: "Powered by TONE3000",
  },
  effects: {
    kicker: "Effects",
    headline: ["Every block shows you", "what it is doing."],
    body: "Each editor draws the effect from the same numbers the knobs set: the gate's threshold over your playing, the compressor's curve, the delay's repeats, the reverb's tail. Grab the picture or grab the knob.",
  },
  more: {
    kicker: "More",
    headline: ["Room for", "your own gear."],
    items: [
      {
        title: "Your plugins, in the chain",
        body: "Three slots host your own Audio Unit effects anywhere in the chain. Their settings are saved with the project.",
      },
      {
        title: "Two lanes",
        body: "Split the signal, run a second amp on lane B, and blend the two in a mixer with level and pan for each lane and a phase flip.",
      },
      {
        title: "Presets and A/B",
        body: "Presets recall the capture, the cab and the chain. Two compare slots hold two versions of a sound and switch with one click.",
      },
    ],
  },
  demo: {
    kicker: "Try",
    headline: ["This is the editor.", "Go on, touch it."],
    body: "The real interface, running in your browser with a stand-in for the audio engine. Load a preset, drag a block, turn a knob.",
  },
  download: {
    kicker: "Output",
    headline: ["Plug in."],
    body: "Installs as an Audio Unit; open Logic Pro and find it under Audio Units, t0audio, tubamp.",
    free: "Free, with no account.",
    unsignedNote: "The installer is not signed with an Apple Developer ID yet.",
    source: "Read the source on GitHub",
    gatekeeper: {
      q: "macOS says it cannot check the installer. Is that expected?",
      a: "Yes. The installer is not signed with an Apple Developer ID yet, so macOS stops it the first time. Open System Settings, Privacy and Security, and choose Open Anyway.",
    },
    /** `unsigned` replaces the step while the installer has no Apple Developer ID signature. */
    steps: [
      { text: "Download the installer for your Mac." },
      {
        text: "Open it and follow the prompts.",
        unsigned:
          "Open it. macOS stops it the first time: go to System Settings, Privacy and Security, and choose Open Anyway. Then follow the prompts.",
      },
      { text: "Restart Logic Pro and add tubamp to an audio track." },
    ],
    archLabels: { arm64: "Apple Silicon", x86_64: "Intel" },
  },
} as const;

/** What a visitor needs to know before the first scroll. Each is true of the plugin today. */
export const FACTS = [
  { big: "TONE3000", small: "The capture library, browsed inside the plugin. Powered by TONE3000." },
  { big: "9", small: "Blocks in a chain, in any order." },
  { big: "A2", small: "The newer NAM capture format. A1 and your own .nam files load too." },
  { big: "3", small: "Slots that host your own Audio Unit effects." },
] as const;

/** What the primary button becomes on a device that cannot install a Mac plugin. */
export const SHARE = {
  label: "Send the link to my Mac",
  short: "Send to my Mac",
  copied: "Link copied",
} as const;

export const FAQ = [
  {
    q: "Do I need my own captures?",
    a: "No. Paste your own TONE3000 API key into the plugin's settings and it browses the TONE3000 library and loads a capture with one click. Any .nam file you already have loads with no setup. Cab impulse responses work the same way.",
  },
  {
    q: "Is it really free?",
    a: "Yes. There is no account, no email and no trial timer. The button downloads the installer, and the source code is on GitHub.",
  },
  {
    q: "Which Macs does it run on?",
    a: "macOS 11 or later, on Apple Silicon and Intel. Each has its own installer, and the page picks yours when it can tell.",
  },
  {
    q: "Does it work outside Logic Pro?",
    a: "tubamp is an Audio Unit, built and tested in Logic Pro. Other Audio Unit hosts are untested.",
  },
] as const;

export const CREDITS = [
  "NeuralAmpModelerCore by Steven Atkinson (MIT)",
  "JUCE 8",
  "Geist by Vercel (OFL)",
  "Powered by TONE3000",
] as const;
