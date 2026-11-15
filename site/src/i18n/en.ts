/**
 * Every sentence the page says, in English. `pt-br.ts` is the same shape in
 * Portuguese; a test keeps the two in step. Facts come from the repository
 * README and docs; change them there first, then here, then in every locale.
 *
 * Names the plugin itself shows (block names, In, Out, AU) are not here: the
 * plugin is in English, so they stay as they read in the clips beside them.
 */
export const en = {
  /** Locale for dates. en-GB prints "14 Nov 2026". */
  dateLocale: "en-GB",
  meta: {
    title: "tubamp. Your next amp is a file.",
    description:
      "A free Audio Unit for Logic Pro that plays neural captures of real amps in a signal chain you build yourself. Apple Silicon and Intel.",
  },
  hero: {
    line: "An amp plugin for Logic Pro.",
    /** Two lines: the first sets up, the second lands. */
    headline: ["Your next amp", "is a file."],
    what: "tubamp plays captures of real amplifiers in Logic Pro. Build the rest of the rig around them in any order, with room for your own plugins.",
    download: "Download free for Mac",
    tryIt: "Try it in the browser",
  },
  /** The reassurances under a download button. */
  release: {
    noAccount: "No account",
    orLater: "{os} or later",
    archs: "Apple Silicon and Intel",
  },
  nav: {
    label: "Main",
    download: "Download",
    downloadMac: "Download for Mac",
    items: { chain: "Chain", captures: "Captures", effects: "Effects", try: "Try it", faq: "Questions" },
  },
  /** What each block does, by block id. */
  blocks: {
    gate: "Closes on the noise between notes.",
    comp: "Evens out the picking hand.",
    drive: "Pushes the front of the amp.",
    amp: "A neural capture of a real amplifier.",
    cab: "An impulse response of a real speaker and microphone.",
    eq: "Parametric or graphic, after the cab.",
    mod: "Chorus, phaser, tremolo.",
    delay: "Digital, tape or analog repeats, in stereo.",
    reverb: "Plate, hall, room or spring.",
  },
  facts: {
    label: "At a glance",
    /** What a visitor needs to know before the first scroll. Each is true of the plugin today. */
    items: [
      { big: "TONE3000", small: "The capture library, browsed inside the plugin. Powered by TONE3000." },
      { big: "9", small: "Blocks in a chain, in any order." },
      { big: "A2", small: "The newer NAM capture format. A1 and your own .nam files load too." },
      { big: "3", small: "Slots that host your own Audio Unit effects." },
    ],
  },
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
    /** Licence text: stays in English in every locale. */
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
    slots: {
      label: "A chain with three of your own Audio Unit plugins placed between the blocks.",
      yours: "Your plugin",
    },
  },
  demo: {
    kicker: "Try",
    headline: ["This is the editor.", "Go on, touch it."],
    body: "The real interface, running in your browser with a stand-in for the audio engine. Load a preset, drag a block, turn a knob.",
    narrow: "The live editor needs a wider screen.",
    frameTitle: "The tubamp editor, running in the page",
    start: "Start the editor",
    use: "Use the editor",
    live: "Press Esc or click outside the editor to scroll the page.",
    idle: "Click the editor to use it again.",
    missing: "The live editor is not in this build of the page.",
  },
  download: {
    kicker: "Output",
    headline: ["Plug in."],
    body: "Installs as an Audio Unit; open Logic Pro and find it under Audio Units, t0audio, tubamp.",
    free: "Free, with no account.",
    unsignedNote: "The installer is not signed with an Apple Developer ID yet.",
    source: "Read the source on GitHub",
    getOnGitHub: "Get it on GitHub",
    downloadFor: "Download for",
    copyHash: "Copy the full SHA-256 of the {arch} installer",
    copy: "Copy",
    copied: "Copied",
    install: "Install",
    requirements: "Requirements",
    platforms: "{os} or later. Apple Silicon and Intel.",
    hosts: "Logic Pro. Other Audio Unit hosts are untested.",
    whatChanged: "What changed",
    questions: "Questions",
    gatekeeper: {
      q: "macOS says it cannot check the installer. Is that expected?",
      a: "Yes. The installer is not signed with an Apple Developer ID yet, so macOS stops it the first time. Open System Settings, Privacy and Security, and choose Open Anyway.",
    },
    /** `unsigned` replaces the step while the installer has no Apple Developer ID signature. */
    steps: [
      { text: "Download the installer for your Mac.", unsigned: "" },
      {
        text: "Open it and follow the prompts.",
        unsigned:
          "Open it. macOS stops it the first time: go to System Settings, Privacy and Security, and choose Open Anyway. Then follow the prompts.",
      },
      { text: "Restart Logic Pro and add tubamp to an audio track.", unsigned: "" },
    ],
  },
  /** After a download click. */
  guide: {
    started: "Your download has started",
    close: "Close",
  },
  /** What the primary button becomes on a device that cannot install a Mac plugin. */
  share: {
    label: "Send the link to my Mac",
    short: "Send to my Mac",
    copied: "Link copied",
  },
  faq: [
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
  ],
  footer: {
    source: "Source on GitHub",
    releases: "All releases",
    language: "Language",
    credits: [
      "NeuralAmpModelerCore by Steven Atkinson (MIT)",
      "JUCE 8",
      "Geist by Vercel (OFL)",
      "Powered by TONE3000",
    ],
  },
  /** What each clip shows, by its file name in `src/data/media.ts`. */
  media: {
    "chain-build": "Blocks being added to the chain on the board and dragged into a new order.",
    presets: "Stepping through presets; the chain rearranges for each one.",
    t3k: "The TONE3000 browser open inside the plugin, scrolling captures and loading one.",
    amp: "The amp editor stepping through captures in the library.",
    split: "A chain split into two lanes with a second amp and a mixer.",
    compare: "Switching between compare slots A and B.",
    "fx-gate": "The gate editor: the input level over time with a draggable threshold.",
    "fx-comp": "The compressor editor: its transfer curve changing with threshold and ratio.",
    "fx-drive": "The drive editor: the waveshaper curve bending as gain rises.",
    "fx-cab": "The cab editor: low and high cut on the response curve.",
    "fx-eq": "The EQ editor: dragging bands on the response curve.",
    "fx-mod": "The modulation editor: the LFO wave changing with rate and depth.",
    "fx-delay": "The delay editor: taps spreading as time and feedback change.",
    "fx-reverb": "The reverb editor: the tail growing with decay.",
    tubes: "Valve filaments glowing amber in the dark.",
    strings: "Three wound guitar strings lit amber on black, one of them vibrating.",
    "capture-720": "A valve amplifier head breaking up into amber particles that settle into a single waveform line.",
    speaker: "A black speaker cone with an amber rim light, dust lifting off it.",
  },
};

export type Dictionary = typeof en;
