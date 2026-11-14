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
  line: "A free amp plugin for Logic Pro.",
  what: "A free Audio Unit for Logic Pro that plays neural captures of real amps, in a signal chain you build yourself.",
  platforms: "macOS 11 or later. Apple Silicon and Intel.",
  hosts: "Logic Pro and other Audio Unit hosts.",
} as const;

/** The default chain, in order. Blocks can be reordered, removed and added again. */
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
] as const;

export const CHAPTERS = {
  chain: {
    index: "03",
    kicker: "Chain",
    headline: ["Build the chain.", "Any order."],
    body: "Nine blocks on a board you can pan and zoom. Drag one somewhere else, remove it, add it again, add a second delay. The order is part of the sound and it is saved with your project.",
    swap: {
      title: "Any order.",
      body: "Swap the drive and the reverb, and the whole tail distorts.",
    },
  },
  captures: {
    index: "04",
    kicker: "Captures",
    headline: ["A real amp,", "captured."],
    body: "tubamp plays Neural Amp Modeler captures, including the newer A2 format: a recording of how one real amplifier responds, in a file. Browse the TONE3000 library from inside the plugin and a capture lands in your chain without leaving Logic. Or import any .nam file you already have.",
    facts: [
      "NAM A1, A2, slimmable and LSTM captures",
      "Resamples when the session rate differs from the capture",
      "Optional loudness match across captures",
      "Cab impulse responses, from TONE3000 or your own files",
    ],
    attribution: "Powered by TONE3000",
  },
  effects: {
    index: "05",
    kicker: "Effects",
    headline: ["Every block shows you", "what it is doing."],
    body: "Each editor draws the effect from the same numbers the knobs set: the gate's threshold over your playing, the compressor's curve, the delay's repeats, the reverb's tail. Grab the picture or grab the knob.",
  },
  more: {
    index: "06",
    kicker: "More",
    headline: ["Room for", "your own gear."],
    items: [
      {
        title: "Your plugins, in the chain",
        body: "Three slots host your own Audio Unit effects anywhere in the chain. They are saved with the project, settings included, and kept intact when the project opens on a Mac that does not have the plugin.",
      },
      {
        title: "Two lanes",
        body: "Split the signal, run a second amp on lane B, and bring both back in a mixer with level, pan and phase.",
      },
      {
        title: "Presets and A/B",
        body: "Presets keep the capture, the cab and the chain. Two compare slots hold two versions of a sound and switch between them on one click.",
      },
    ],
  },
  demo: {
    index: "07",
    kicker: "Try",
    headline: ["This is the editor.", "Go on, touch it."],
    body: "The real interface, running in your browser with a stand-in for the audio engine. Load a preset, drag a block, turn a knob.",
  },
  download: {
    index: "08",
    kicker: "Output",
    headline: ["Plug in."],
    body: "Free. Installs as an Audio Unit; open Logic Pro and find it under Audio Units, t0audio, tubamp.",
    unsigned:
      "The installer is not signed with an Apple Developer ID yet. macOS will ask you to confirm the first time: open System Settings, Privacy and Security, and choose Open Anyway.",
    steps: [
      "Download the installer for your Mac.",
      "Open it and follow the prompts.",
      "Restart Logic Pro and add tubamp to an audio track.",
    ],
    archLabels: { arm64: "Apple Silicon", x86_64: "Intel" },
  },
} as const;

/**
 * The two film interludes that bookend the chain: the signal starts at a string
 * and ends at a speaker. `turn` is the end of the line that takes the serif.
 */
export const INTERLUDES = {
  strings: { line: "Every sound starts at a string.", turn: "a string." },
  speaker: { line: "And ends at a speaker.", turn: "a speaker." },
} as const satisfies Record<string, { line: string; turn: string }>;

export const CREDITS = [
  "NeuralAmpModelerCore by Steven Atkinson (MIT)",
  "JUCE 8",
  "Geist by Vercel (OFL)",
  "Powered by TONE3000",
] as const;
