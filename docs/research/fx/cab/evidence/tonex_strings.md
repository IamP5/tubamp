# TONEX: strings only (no symbols, no code traced)

Binary `TX` = `/Library/Audio/Plug-Ins/Components/TONEX.component/Contents/MacOS/TONEX`, thin arm64 slice
`$RE_STATE/thin/01f457e004641752-TONEX`. Offsets are file offsets printed by `strings -t x -n 5`.

| offset | string | what it tells us |
| --- | --- | --- |
| 0xb31cb0 | `ToneModeVIR` | the cab mode is called VIR |
| 0xb32c18 | `BlendSli` `CabLabel` `Mic1Labe` `Mic2Labe` | two microphones and one blend slider |
| 0xb6f582 | `N2IK8Products4TXMM27CabModelBypassWidgetManagerE` | a manager for bypassing the cab that is inside a tone model |
| 0xb3a6d0 | `NOISEGATE_ENABLE` `CAB_MODEL_ENABLE` | per-preset cab model enable |
| 0xc4382d | `[CabType] int,` | preset database column |
| 0xc43869.. | `[VIRCabModel] int, [VIRCabMic1Model] int, [VIRCabMic1X] float(24), [VIRCabMic1Z] float(24), [VIRCabMic2Model] int, [VIRCabMic2X] float(24), [VIRCabMic2Z] float(24), [VIRCabMicBlend] float(24), [VIRCabResonance] float(24)` | the whole VIR parameter set: two mics with X and Z each, one blend, one resonance |
| (same table) | `[HWParamA_PwrAmpEqPresence] float(24), [HWParamA_PwrAmpEqDepth] float(24)` | Presence and Depth belong to a power-amp EQ |
| 0xc5503e | `KISResAT5/FlexiCabAT5/AT5/%s%s%s_%02d_%02d_90.0.wav` | the same per-position IR files as AmpliTube 5's FlexiCab |
| 0xc57063 | `Presets containing Custom IRs cannot be uploaded to ToneNET, please use VIR instead.` | custom IR mode exists |
| 0xc5827a | ` 2. Ensure that the audio file is in WAV format, with a sample rate of 44.1 kHz and 16-bit mono.` | custom IR file requirement stated to the user |
| 0xc56df1 | `Amp and Cab` | capture type label |

AU parameters (see `tonex_cab_params.tsv`): `MODEL` Off/On, `CAB TYPE` MODEL / VIR / OFF, `CAB MODEL`,
`MIC 1 MODEL`, `MIC 1 X`, `MIC 1 Z`, `MIC 2 MODEL`, `MIC 2 X`, `MIC 2 Z`, `MIC BLEND`, `CAB RESONANCE`,
`PRESENCE`, `DEPTH`, `MODEL POST FILTER`.

The engine behind VIR is in the AmpliTube 5 binary with symbols: `IK::KIS::FX::ToneX::CabVIRLight`
(`IRPool::LoadIR` 0xfb9198 requests 2048-sample IRs at 44100 Hz; `IRPool::GetIRs` 0xfb8a84 takes four
`IRDesc`), see `at5_cab_notes.txt` section 3 and the gaps list.

Recheck: `strings -t x -n 5 "$RE_STATE/thin/01f457e004641752-TONEX" | grep -E 'VIRCabMic1X|VIRCabResonance|PwrAmpEqPresence|44.1 kHz and 16-bit|ToneModeVIR'`
