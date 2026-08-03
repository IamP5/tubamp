# tubamp

A Logic Pro (Audio Unit) amp signal-chain plugin built around **NAM A2** captures —
in the spirit of ToneX, HX Stomp and Kemper — with a TONE3000 browser for
installing models, a cab IR loader, and a fixed effects chain.

```
Input Trim → Gate → Comp → Drive → [NAM MODEL] → Cab IR → EQ → Mod → Delay → Reverb → Output
```

- **NAM engine**: NeuralAmpModelerCore v0.5.4 with the A2 fast path enabled
  (`NAM_ENABLE_A2_FAST`). Loads A1, A2, slimmable-container and LSTM `.nam` files;
  automatic resampling when the host rate differs from the model's native rate;
  optional loudness normalization to −18 dB from model metadata.
- **TONE3000**: browse the catalog via the official Select flow (OAuth 2.0 + PKCE,
  A2-first: `architecture=2`), download models straight into your local library.
  Requires your own TONE3000 publishable API key (Settings gear → paste
  `t3k_pub_…` key; create one at tone3000.com → Settings → API Keys, redirect URI
  `http://127.0.0.1:53682/callback`). Manual `.nam`/IR import works with no setup.
- **Presets**: JSON presets with model + IR references, favorites, tags,
  and true A/B compare slots.

## Build (macOS)

Requires Xcode and CMake ≥ 3.24.

```sh
cmake -B build -DCMAKE_BUILD_TYPE=Release -DCMAKE_OSX_ARCHITECTURES=arm64
cmake --build build --target tubamp_AU -j 8
```

The AU is auto-copied to `~/Library/Audio/Plug-Ins/Components/tubamp.component`.
Then:

```sh
codesign --force --deep --sign - ~/Library/Audio/Plug-Ins/Components/tubamp.component
killall -9 AudioComponentRegistrar   # force Logic to rescan
auval -v aufx Tamp Tuba              # validate
```

Open Logic Pro → insert **Audio Units → Tuba → tubamp** on an audio track.
A Standalone app target (`tubamp_Standalone`) is also available for quick testing.

## Library layout

```
~/Library/Application Support/tubamp/
  models/    *.nam captures
  irs/       *.wav impulse responses
  presets/   *.json presets
```

## Licenses

- JUCE 8 (Starter/AGPL terms apply to distribution)
- NeuralAmpModelerCore — MIT © Steven Atkinson
- TONE3000 API usage subject to the TONE3000 API Terms of Service
  (per-user downloads only, no bulk/scraping, "Powered by TONE3000" attribution).
