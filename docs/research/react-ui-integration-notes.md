# React UI — integration notes

Final integration pass over the four parallel slices (WebEditor/CMake, ui/
scaffold, board, panel/header/footer/settings). What a reviewer should look at,
what was changed to make the pieces fit, and what is still open.

Everything below was verified on `refactor/react-ui`; nothing is committed.

---

## 1. What a reviewer should look at first

| Area | Files | Why |
| --- | --- | --- |
| App shell + layout | `ui/src/App.tsx`, `ui/src/App.module.css` | The dock became a real overlay; header/footer entrance; app-wide contextmenu + settings ownership live here now. |
| Board ↔ dock geometry | `ui/src/features/board/useStageTransform.ts`, `features/board/layout.ts` (`DOCK_INSET`), `features/board/board.module.css`, `theme/tokens.css` (`--dock-h`/`--dock-gap`/`--dock-inset`) | One number (244px) is duplicated in CSS and JS by necessity. If the dock height ever changes, both must move. |
| Glyph unification | `ui/src/components/blockGlyphs.tsx` (moved from `features/board/glyphs.tsx`), `components/icons.tsx` | There were two competing glyph sets for the same nine blocks. |
| Ship-build safety | `ui/vite.config.ts` (`stripCrossorigin`) | Removes the only sub-resource failure mode that cannot be verified without a real WKWebView. |
| CMake guard | `CMakeLists.txt` (prototype block) | Referenced `src/ui/TubampLookAndFeel.cpp`, which no longer exists. |

---

## 2. Layout: the param dock is now an overlay, not a flex sibling

`docs/REACT-UI.md` §UX structure calls for "param dock (bottom, **slides over**
board bottom edge)". It was previously a fixed-height flex row below the board,
with the *shell* owning the card surface — so with nothing selected the Panel
rendered nothing and an empty 232px rounded box stayed on screen (flagged in the
panel agent's handoff).

Now:

- `App.module.css .board` is `position: absolute; inset: 0` — the board paints
  **full-bleed**, so the dot grid runs behind and around the floating dock.
- `App.module.css .dock` is a positioning box only: absolute, `--dock-gap`
  inset, `--dock-h` tall, `pointer-events: none`. The surface (background,
  radius, `--shadow-dock`) moved into `panel.module.css .dock`, which re-enables
  pointer events. Nothing selected ⇒ nothing painted.
- The board's *geometry* still respects the dock. `board.module.css .stage` is
  `inset: 0 0 var(--dock-cover) 0`, so its centre — the transform origin — is the
  centre of the visible band, and `useStageTransform` takes a third
  `insetBottom` argument used by `panLimit`, `zoomAt` (the zoom-to-cursor centre
  MUST match the transform origin) and `fit`. The chain therefore never settles
  underneath the dock, and the resulting board band is 352px of 596px ≈ the
  "~60% height" the spec asks for.
- `.emptyOverlay` and the zoom bar are pinned to the same band.

`DOCK_INSET = 244` in `features/board/layout.ts` and `--dock-inset` in
`theme/tokens.css` are the same number in two languages; both carry a comment
pointing at the other.

## 3. App entrance choreography (motion-design.md §2.1)

- Canvas chrome: `opacity` only, `duration.fast`, `ease.out`.
- Header: `y: -12 → 0`, `spring.ui`, `delay 0.02`. Footer mirrors it at `+12`.
- Param dock: the Panel's own `AnimatePresence` runs `initial={false}`, so first
  mount is deliberately silent there. `App.module.css .dockInner` supplies the
  one-shot arrival — `opacity` + `y: 12`, `duration.slow`, `ease.out`,
  `delay 0.28` (board stagger done + a beat), which is the spec's "panel fades in
  last".
- Everything is gated on `useReducedMotion()` (`initial={false}` collapses each
  to a straight cut).

## 4. Cross-feature issues resolved

1. **Two settings sheets.** App.tsx rendered a `SettingsSheet` nothing could
   open while Header rendered its own. App now owns `settingsOpen` and the
   sheet; `Header` takes `onOpenSettings`. One instance, one source of truth.
2. **Empty dock box with no selection.** Fixed by §2.
3. **Duplicated `amp_cal_input` / `amp_cal_level`.** They were in both the amp
   dock body and the settings sheet. Removed from the settings sheet (which
   §Repository layout scopes to "t3k key via native prompt, sign out"); the amp
   panel keeps them next to the IN/OUT levels they calibrate, and the sheet
   carries a one-line pointer so the native SettingsPanel's users can find them.
4. **Two glyph sets for the same nine blocks.** `features/board/glyphs.tsx` (the
   faithful `BlockIcons.h` port) moved to `components/blockGlyphs.tsx` and is now
   the single source; `components/icons.tsx` `BlockIcon` is a thin size/stroke
   preset over `BlockGlyph`, and its simplified 16px path table is deleted. Board
   card, dock header and menu rows now show the same glyph.
5. **`contextmenu` suppression was board-local.** Moved to `App.tsx` so it holds
   while the board is unmounted (during hydration, behind a modal). `index.html`
   keeps its own pre-React listener for the frames before mount; the board keeps
   its inline `onContextMenu` on the stage.
6. **Unused `store.moveBlock`.** Deleted. The reorder drag deliberately commits
   the whole resulting order through `setChainOrder`, so a gesture is exactly one
   write to C++; a second mutation path would make that guarantee depend on the
   caller. A comment in the store says so.
7. **Footer "Latency 0 smp" with no model.** Not a bridge gap after all:
   `TubampAudioProcessor::updateLatency` reports the NAM engine's latency only
   when an enabled amp block holds a model, and 0 otherwise, so `latencySamples`
   on `ModelInfo` *is* the processor latency. The line now reads
   `No model  ·  Latency 0 smp`; the reasoning is in a comment.
8. **"NOT RUNNING" chip.** `features/panel/library.ts` derives it from
   `sampleRateHz <= 0`. Checked against C++: `loadedModelPath` is assigned only
   after `namEngine.loadModel` returns no error (`PluginProcessor.cpp:580-591`),
   so "path set but engine down" cannot occur and `modelVar()` returns JSON null.
   The chip is unreachable defensive code — left in place, not a bug, but nobody
   should spend time trying to trigger it.
9. **Prototype CMake block referenced deleted sources.** `prototype/t3k-browser`
   links `src/ui/TubampLookAndFeel.cpp`, deleted with the rest of `src/ui/`. The
   guard now requires both halves, so a checkout that still has the prototype
   configures instead of failing on a missing source file.
10. **README build instructions were wrong.** A default `cmake -B build` now
    needs Node; documented, together with the `-DTUBAMP_BUNDLED_UI=OFF` dev-server
    workflow and the Geist/React/Vite/Motion license lines.

## 5. `crossorigin` stripped from the built `index.html`

Vite stamps `crossorigin` on the emitted module script and stylesheet, which puts
those sub-resource requests in CORS mode. In a ship build the page's origin is
the custom scheme `juce://juce.backend`, and JUCE's macOS resource provider emits
`Access-Control-Allow-Origin` only when an `allowedOrigin` is configured — which
ship builds deliberately do not do (`docs/research/juce-webview.md` §5.4). The
requests are same-origin so they *should* skip the CORS check entirely, but a
blank WebView is an unrecoverable failure in a plugin and the attribute buys
nothing here. `ui/vite.config.ts` now carries a `transformIndexHtml` plugin
(`stripCrossorigin`, build-only) that removes it. Verified absent from the zip
that is baked into the binary.

## 6. Verified

UI gates (in `ui/`):

```
npm run typecheck     green
npm run lint          green
npm run build         green — 468 kB js / 139 kB gzip, 34 kB css, 7 woff2
```

Native, bundled (the previously unexercised path):

```
cmake -S . -B build -DTUBAMP_BUNDLED_UI=ON      configure OK
cmake --build build -j                          all targets green
```

- `build/tubamp_webui.zip` = 12 entries under `dist/`, matching the resource
  provider's `TUBAMP_WEBUI_ZIP_PREFIX` + relative-path resolution.
- `webui::tubamp_webui_zipSize == 488055`; the zip's entry names appear in both
  `tubamp.component/Contents/MacOS/tubamp` and the Standalone binary.
- All emitted asset URLs are relative (`./assets/…`, and font `url(./…)` inside
  the CSS), i.e. they resolve under `juce://juce.backend/`.
- Incremental wiring works both ways: touching `ui/src/App.tsx` re-runs
  `Building tubamp web UI (vite)` → `Zipping` → binary data → relink; touching
  nothing skips npm entirely.

Tests / validation:

```
tubamp_smoke <Deluxe Reverb.nam>    exit 0 (model changes signal; 10/10 chain-order checks)
auval -v aufx Tamp Tuba             exit 0 — AU VALIDATION SUCCEEDED
```

Dev escape hatch:

```
cmake -S . -B build-dev -DTUBAMP_BUNDLED_UI=OFF   configure OK (no Node used)
cmake --build build-dev --target tubamp -j        green — compiles the TUBAMP_WEBUI_DEV=1 branch
```

CMake's configure-time `file(COPY)` of the JUCE JS frontend into `ui/src/juce/`
leaves the hand-written `index.d.ts` / `global.d.ts` intact and writes
byte-identical `index.js`, `check_native_interop.js`, `package.json` (diffed
against `build/_deps/juce-src/...`), so it produces no git churn.

Browser (mock bridge, `vite preview`, 1120×700): full chain, board/dock overlay,
accent-tinted dock, meters, footer info line — one console message, the expected
`window.__JUCE__ is undefined` notice from the vendored lib. Removing all nine
blocks leaves the empty-chain state with no residual dock box and the dot grid
running to the footer.

Standalone launch: `tubamp.app` starts, stays up, `vmmap` shows 29 WebKit/WebCore
mappings in the process (the `WebBrowserComponent` really instantiated), no
crash report.

## 7. Still open

1. **No pixel confirmation of the WKWebView itself.** `screencapture` needs
   Screen Recording permission that this environment does not have, so the
   bundled UI has been proven to be *present and addressable* (zip entries in the
   binary, correct MIME table, relative URLs) but not *seen rendering*. Open the
   Standalone once by hand before shipping.
2. **`pluginval` was not run.** `auval -v` queries the Cocoa view class but does
   not repeatedly open and close the editor, which is exactly the churn that
   exposes JUCE#1415 (WKWebView stuck on `about:blank`) flagged in
   `juce-webview.md` §5.1. Run `pluginval --strictness-level 10` plus a manual
   Logic open/close/reopen cycle.
3. **No push notification for model/IR changes.** `WebEditor::pollModelAndIr`
   polls `getLoadedModelPath`/`getLoadedIrPath`/`getLatencySamples` at 5 Hz. If
   the processor ever gains `onModelChanged`/`onIrChanged`, delete the poll and
   subscribe.
4. **TONE3000 downloads have no global indicator.** Progress is store-held and
   survives switching blocks, but is only visible while the amp panel is open.
5. **`bridge/paramMeta.ts` duplicates `src/Parameters.cpp`** (ranges, defaults,
   labels, log skew) for knob reset and formatting, because the relay carries no
   default. A parameter change in C++ that is not mirrored there drifts silently.
   Worth a generated header or a `getParamMeta()` native function later.
6. **Bundle is one 468 kB chunk**, mostly the motion library, and the mock bridge
   ships inside it (runtime-guarded, allowed by the spec). Nothing is code-split.
7. **Deliberate deviation from motion-design.md §2.6/§3.1** on the board: the
   selection ring is a per-card pre-baked layer cross-faded by opacity, not a
   `layoutId` shared element, and the lane does not use the `layout` prop —
   Motion's layout projection does not divide out the plain CSS `scale` on the
   pan/zoom stage, so both would mis-travel at zoom ≠ 1. Expect the diff.
8. **Board trackpad heuristic** (plain wheel zooms, pan needs shift or a
   horizontal component) is untested on a real trackpad inside WKWebView; it is
   one branch in `useStageTransform`'s `onWheel` if it needs flipping.
