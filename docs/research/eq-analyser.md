# EQ spectrum analysers: what the references do

Research for the spectrum behind tubamp's EQ curve (2026-11-14). Two sources:
the published manuals, and a static read of Logic Pro's Channel EQ analyser on
this machine. "Read" means the value was read from the binary's disassembly;
"documented" means the vendor's help page states it; anything else is marked.

## Logic Pro Channel EQ (read from the binary)

`Logic Pro.app/Contents/Frameworks/MADSP.framework/MADSP`, arm64 slice, which
ships C++ symbols: `CChannelEQShell` → `CVariEQBaseShell` → `CSpectrumAnalyzerFX`
→ `CSpectrumAnalyzer`. Addresses are in that slice.

| Parameter | Value | Where |
| --- | --- | --- |
| FFT size | 2048 / 4096 (default) / 8192 points; fixed, not scaled with sample rate | menu at 0x24e364; `ControlChange` 0x2506bc |
| Hop | 1024 samples at every size (overlap 2 / 4 / 8) | 0x2506c8 |
| Window | periodic Hann, scaled by 1/N | `Setup` 0x11cdd0 |
| Magnitude | power, \|X\|² | `Analysis` 0x11ea48 |
| Ballistics | attack instant; release a fixed fall in dB per second | 0x11eac0, `CalcPeakDecay` 0x11d700 |
| Decay | 0 to 30 dB/s, default 10; 0 holds | control at 0x24e518 |
| Mode | Peak (default) or RMS | 0x24e43c |
| Display points | 480, f = 20·10^(3i/479): 20 Hz to 20 kHz, log spaced | `GetResponsePoints`; table at 0x1e4b8c |
| Bin mapping | band edges at the midpoints between neighbouring points; two or more bins in the band → the maximum power; otherwise linear interpolation in dB between the two nearest bins. No fractional-octave smoothing | `GetGraphData` 0x11f428 |
| Range | 60 dB (default) or 120 dB; top −40 to +20 dB, default 0 | 0x24e5b4 |
| Tap | pre or post EQ (default post) | `OnExecute` 0x24b500 / 0x24b888 |
| Runs | only while the analyser is on and the editor is visible | 0x24b4d4 |

Not determined: the exact 0 dB reference of the display, how stereo is
combined, and whether the Swift UI layer smooths again.

### Its Resolution, Decay and hold (second pass)

| Control | What it does | Where |
| --- | --- | --- |
| Resolution | `SetOrder` rebuilds the window in place (buffers are sized for 8192 up front) and calls `Reset()`: the input ring, the held peaks, the average and the display copy are all zeroed. No crossfade: the curve drops to the floor and rebuilds | `SetOrder` 0x11d228 → `Reset` 0x11e1fc; clears at 0x11e250, 0x11e720 |
| Decay ("Analyzer Decay", dB/s, 0 to 30 in steps of 1, default 10) | per hop c = (10^(−D/20))^(hop/sr); held = max(new, old·c²). Attack is instant; there is no attack smoothing anywhere. Nothing decays while the analyser is not running | `ControlChange` 0x2502c4; peak loop 0x11eac0 |
| Hold (D = 0) | c = 1: every bin keeps its running maximum since the last reset. Cleared by a Resolution change, a Mode change and `OnReset`; NOT by turning the analyser off and on. The analyser keeps running with the editor closed while D = 0 | gating 0x24b4d4; `OnReset` 0x24fff8; Mode 0x2504f8 |
| RMS mode | A = c²·A + P, W = c²·W + 1, shown A / W: a normalised exponential mean of power with τ = 4.34 / D seconds (0.43 s at the default) | 0x11e8ac, 0x11f194 |
| Storage | every analyser control is an ordinary plugin controller, saved with the plugin setting; no preference keys | 0x24e71c–0x24e7f4 |

Logic's Multimeter, as a second reference for hold: Return Rate 4 to 50 dB/s,
Peak Hold off / on, Hold Time 2 s / 4 s / 6 s / infinite, with a separate hold
reset (`CMultiMeterShell::OnSetup` 0x279e4c).

No FabFilter product is installed on this machine, so Pro-Q's Resolution, Speed
and Freeze are known from its manual only (below).

## FabFilter Pro-Q 4 (documented)

- Pre, Post and SC/Ext are independent toggles.
- Range 60, 90 (default) or 120 dB, on its own scale, independent of the EQ's
  gain scale (±3, 6, 12 or 30 dB).
- Resolution 1024, 2048, 4096 or 8192 points; higher is more precise in the
  bass and slower to respond.
- Speed changes the release only. No numbers are published.
- Tilt around 1 kHz, default 4.5 dB per octave.
- Freeze holds the maximum. Spectrum Grab lets a peak be dragged.

Source: https://www.fabfilter.com/help/pro-q/using/analyzer

## Others

- Voxengo SPAN: default slope 4.5 dB per octave (documented,
  https://voxengo.com/qna/spectrum-analysis-28); smoothing down to 1/48 octave.
- iZotope Ozone: spectrum types Linear, 1/3 octave, Critical, Full octave, with
  average time, window, overlap and peak hold (documented; no defaults given).
- The JUCE SimpleFFT tutorial is a demo (order 10, no window): not a model.

## What tubamp takes from this

| Choice | Value | Follows |
| --- | --- | --- |
| FFT | 4096 points at 44.1 / 48 kHz, Hann | Logic's default and Pro-Q's "High". tubamp doubles it at 88.2 / 96 kHz so the bass resolution does not halve; Logic does not |
| Refresh | the editor's 30 Hz timer, each frame on the latest 4096 samples | Logic's hop is 1024 samples (about 47 per second at 48 kHz) |
| Display points | 256, log spaced, 20 Hz to 20 kHz | Logic's axis; Logic draws 480, 256 is already under 3 px apart in tubamp's plot |
| Bin mapping | maximum where a point's band holds bins, interpolation in dB where it does not | Logic's rule exactly |
| Ballistics | attack instant, release a constant 20 dB per second, in the UI | Logic's Peak mode (its default is 10, its range 0 to 30); Pro-Q's Speed (release only). 20 is the fall of tubamp's other meters |
| Tilt | +4.5 dB per octave around 1 kHz, display only | Pro-Q's and SPAN's default; Logic has none |
| Range | its own scale across the plot's height, independent of the gain scale | Pro-Q |
| Tap | post as the main shape, pre as a dimmer line | Pro-Q shows both; Logic one at a time |
| Cost | analysed only while an EQ editor is open | Logic |
| Resolution | Low / Medium / High = 2048 / 4096 / 8192 points; a change clears the display, which rebuilds from the floor | Logic's three sizes and its reset; Pro-Q adds 1024 |
| Speed | Slow / Medium / Fast = 10 / 20 / 40 dB per second, release only | Logic's decay law and default (10) as Slow; named steps like Pro-Q; all inside the Multimeter's 4 to 50 range |
| Freeze | a latch: each point holds its maximum, still rising instantly; released, the points fall from where they are. Turning the analyser off or changing resolution releases or clears it | Pro-Q's Freeze; Logic's decay 0 (which Logic keeps across off / on: tubamp does not) |
| Storage | on / resolution / speed saved with the plugin state, not in presets; Freeze not saved | Logic saves them with the plugin setting |
