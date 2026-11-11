# TONE3000: IR block

Binary `T3` = `/Library/Audio/Plug-Ins/Components/TONE3000.component/Contents/MacOS/TONE3000` (arm64, JUCE).
The IR block is `juce::dsp::Convolution`. Callers found with a branch-target scan of `__text`
(`python3 evidence/bb/xref.py $RE_STATE/thin/5e8e25b46c515c11-TONE3000 0x62d210 0x62d900 0x62dd44 0x62def0 0x62e22c`).

## Loading: `TONE3000Processor::prepareBlockModelOffThread` (0x68d84)

```
69008  AudioFormatManager ctor ; 69014 registerBasicFormats ; 69020 createReaderFor(file)
6902c  ldr d0, [x23, #0x8]            ; reader->sampleRate (fallback 48000.0 = 0x40e7700000000000)
69044  fmov d2, #10.0 ; fmul ; fcvtzu x24   ; x24 = 10 s of the file = size limit
69050  ldr x9, [x23, #0x18]           ; lengthInSamples ; min with x24
6905c  ... * 48000 / fileRate ; 69088 fcvtas x26   ; x26 = IR length expressed at 48 kHz
69068  ldr w27, [x23, #0x20]          ; numChannels
690c8  mov w8, #0xbb81 (48001) ; cmp w26, w8 ; b.ge 0x693f0
690e0  bl  juce::dsp::Convolution::Convolution()               ; IR up to 1 s: default, uniform, zero latency
693f0  mov w8, #0x2000 ; str w8, [sp,#0xc8]                    ; IR over 1 s:
69408  bl  juce::dsp::Convolution::Convolution(NonUniform{8192})
69418  mov w2, #0x0      ; Stereo::no
6941c  mov w3, #0x1      ; Trim::yes
69420  mov x4, x24       ; size = 10 s
69424  mov w5, #0x0      ; Normalise::no
69428  bl  Convolution::loadImpulseResponse(File, Stereo, Trim, size_t, Normalise)
69434  bl  Convolution::prepare(spec)
69440  bl  $_6(spec.maximumBlockSize, spec.numChannels, convolution)   ; calls processSamples (0x6ab7c)
6944c  cmp w27, #0x2 ; b.lt ...                                 ; file has 2+ channels:
69738  mov w2, #0x1      ; Stereo::yes  (second Convolution object, same Trim::yes / Normalise::no)
69748  bl  Convolution::loadImpulseResponse
```

- Two `juce::dsp::Convolution` objects per IR block when the file is stereo: one mono (first channel), one
  stereo. The mono one is stored at `block+0x10`, the stereo one at `block+0x18`.
- The convolution is built, prepared and run once on a helper thread (`$_6` at 0x6aa8c calls
  `Convolution::processSamples` at 0x6ab7c) before the block is handed to the chain.
- `Normalise::no` in JUCE means the IR is only scaled by `fileRate / sessionRate`.

## Level: `TONE3000Processor::processChainOnBuffer` (0x40490)

```
411a0  ldr w9, [x8, #0x18] ; cmp w9, #0x1      ; block type 1 = IR
411b4  ldrb w8, [x8, #0x5e0]
411bc  fmov s0, #-18.0
411c0  fcsel s0, s0, s10, eq                   ; -18 dB when the flag is clear
411c4  w8 = 0x3d4ccccd (0.05) ; fmul ; bl exp10f    ; dB -> linear
```

UI help string in the binary: `blockOutIr: "block output gain, +-24 dB (IR pre-trimmed -18 dB)."`
(`strings` on the thin slice; the sign glyph is not ASCII).

## Rate: IR at the base rate inside an oversampled chain

```
40d54  bl ChainOversampler::runDownCascade
40d94  bl juce::dsp::Convolution::processSamples
40db0  bl ChainOversampler::runUpCascade
```

taken when the chain's oversampling factor (`[x26]`) is above 1 (0x40c50 `cmp w9,#1; b.gt 0x40d38`);
otherwise `processSamples` is called directly (0x40c88).

## Other

- `TONE3000Processor::refreshIrTailLength` (0x3d2e4) feeds `getTailLengthSeconds` (0x3d2bc).
- Model loudness: `nam::DSP::GetLoudness` (0x40cb4), difference to a target clamped to +-12 dB
  (0x40ce8..0x40cf8), `exp10f(dB * 0.05)`. This is the NAM block, not the IR.
- Gear labels in the embedded UI: `{amp:"Amp Head","amp-cab":"Amp + Cab","full-rig":"Amp + Cab",pedal:"Pedal",outboard:"Outboard",cab:"Cabinet",...,ir:"IR"}`.

Recheck:
`python3 $RE/disasm.py "$T3" --addr 0x690e0 | grep -n ' 6941[0-9a-f]:\| 6942[0-9a-f]:\| 690c[0-9a-f]:\| 693f[0-9a-f]:\| 6973[0-9a-f]:'`
`python3 $RE/disasm.py "$T3" --addr 0x40c88 | grep -n ' 411b[0-9a-f]:\| 411c[0-9a-f]:\| 40d5[0-9a-f]:\| 40d9[0-9a-f]:\| 40db[0-9a-f]:'`
`strings -n 8 "$RE_STATE/thin/5e8e25b46c515c11-TONE3000" | grep -o 'blockOutIr.\{0,80\}'`
