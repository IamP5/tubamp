# tubamp

A Logic Pro (Audio Unit) amp signal-chain plugin built around **NAM A2** captures —
in the spirit of ToneX, HX Stomp and Kemper — with a TONE3000 browser for
installing models, a cab IR loader, and a signal chain you build yourself on a
pan/zoom board.

Default order (blocks can be reordered, removed and re-added; chain order is
state, not a parameter):

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

Requires Xcode, CMake ≥ 3.24, and Node ≥ 20 (the editor is a Vite/React SPA
hosted in a `WebBrowserComponent`; the configure step runs `npm ci` +
`npm run build` in `ui/` and bakes the result into the binary).

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

Open Logic Pro → insert **Audio Units → t0audio → tubamp** on an audio track.
A Standalone app target (`tubamp_Standalone`) is also available for quick testing.

### Editor (web UI)

The editor lives in `ui/` — see `docs/REACT-UI.md`.

```sh
cd ui && npm install
npm run dev        # mock bridge in a normal browser, http://localhost:5173
npm run typecheck && npm run lint && npm run build
```

Iterating on the UI against a *running* plugin instead of the mock bridge:

```sh
cmake -B build-dev -DTUBAMP_BUNDLED_UI=OFF   # no Node at build time; loads the dev server
cmake --build build-dev -j
cd ui && npm run dev                          # port 5173 is pinned on both sides
```

A dev build is not copied into `~/Library/Audio/Plug-Ins/Components` — it would
overwrite the installed shipping plugin with one that shows a WebKit error page
whenever Vite is not running. Load `build-dev/.../tubamp.component` explicitly, or
run the Standalone target from that build tree.

`TUBAMP_BUNDLED_UI=ON` (the default) is the shipping path: the built `ui/dist`
is zipped into the binary and served from `juce://juce.backend/`, and the plugin
*is* installed after each build.

The two concerns are separate options. `TUBAMP_BUNDLED_UI` controls whether npm
runs and the bundle is baked in; `TUBAMP_WEBUI_DEV` controls where the editor
loads its HTML from and defaults to the complement of the former. Setting both
off (`-DTUBAMP_BUNDLED_UI=OFF -DTUBAMP_WEBUI_DEV=OFF`) builds without Node and
shows the "UI bundle missing" page instead of pointing at an absent dev server.

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
- Geist Sans / Geist Mono — SIL Open Font License 1.1 © Vercel
  (`ui/src/theme/fonts/OFL.txt`)
- React, Vite, Motion, zustand — MIT
- TONE3000 API usage subject to the TONE3000 API Terms of Service
  (per-user downloads only, no bulk/scraping, "Powered by TONE3000" attribution).
