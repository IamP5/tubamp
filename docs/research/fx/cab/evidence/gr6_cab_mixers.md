# Guitar Rig 6: multi-microphone cabinets and the IR loader

Binary `GR` as in `gr6_convolution.md`. All three classes derive from `ConvolutionCabBase`: they only
produce an impulse response (`calcIR`), the base class owns the one convolver pair.

## `NI::DSP::ControlRoomCustom8::calcIR` (0xb2cec): eight slots mixed into one IR

For each of 8 slots that is loaded and not muted (solo logic at 0xb305c..0xb3088):

```
b302c  fmov s1, #-1.0 ; b3030 fmov s2, #1.0 ; b3034 fmov s3, #0.5
b303c  mov/movk w13 = 0x3d23d70a      ; 0.04
b3044  mov/movk w14 = 0x43ab999a      ; 343.2
b3050  fmov s5, #-2.5 ; b3054 fmov s6, #1.5
b308c  ldrb w10, [x10, #0xa28]        ; phase-invert flag of the slot
b3094  fcsel s17, s2, s1, eq          ; polarity = +1 or -1
b30c8  ldr  s7, [x15, #0x998]         ; fader
b30cc  fadd s16, s7, s7               ; 2 * fader
b30d0  ldr  s18, [x15, #0x9d8]        ; pan
b30d4  fsub s7, s2, s18               ; 1 - pan
b30e4  fmul s7, s16, s7               ; left gain  = 2*fader*(1-pan)
b30e8  fmul s16, s16, s18             ; right gain = 2*fader*pan
b30fc  ldr  s18, [x10, #0xa08]        ; phase parameter
b3100  fmul s18, s18, s0              ; * IR sample rate
b3108  fmul s18, s18, s19             ; * 0.04
b3110  fdiv s18, s18, s19             ; / 343.2  -> delay in samples
b3114  fcvtms w10, s18                ; integer part
b311c  fsub s18, s18, s19             ; fractional part
```

The slot's IR is then added into the left and right composite buffers at the integer offset; the first
samples use a 4-point cubic with coefficients 0.5, -2.5, 2, 1.5 (0xb316c..0xb31a8), i.e. Catmull-Rom
fractional delay. A slot's room IR, when present, is added with the slot's room level (0xb31e4..);
when the room level is above 0.5 the close IR is scaled by `1 - (2*room - 1)` (0xb30ac..0xb30c4).

After the loop:

```
b35c4  scvtf s0, w25                  ; number of active slots N
b35c8  bl   logf
b35cc  w8 = 0xc10af968                ; -8.6859 = -20/ln(10)
b35d8  fmul s0, s0, s1
b35dc  fmov s1, #-12.0
b35e0  fadd s0, s0, s1                ; gain_dB = -20*log10(N) - 12
...                                   ; dB -> linear through ExpTable, applied to both channels (0xb36b4..)
b36d8  ldr s0, [x20, #0xf80] ; *0.75 + 0.125 -> EQ_Shelf::setGain_1   (bass)
b36f4  ldr s0, [x20, #0xf84] ; *0.75 + 0.125 -> EQ_Shelf::setGain_2   (treble)
b3724  bl   EQ_Shelf::processBuffer   ; the shelving EQ is run over the composite IR, in place
```

So fader, pan, polarity, time offset, room blend, 1/N scaling, a fixed -12 dB and the bass/treble EQ are
all baked into one stereo IR. The audio path runs one convolver whatever the number of microphones.

`resampleIR` (0xb172c) converts each slot IR to the convolver rate with
`SampleRateConverter::init(src, dst, 2, quality 2)` (0xb19ac, 0xb1ac0), `convert`, and
`AudioBlock::amplify(src / dst)` (0xb1a8c `fdiv`, 0xb1a90), and reserves room for the largest phase
offset (same 0.04 / 343.2 constants, 0xb17a0..0xb17bc).

## `NI::RIG::COMP::IRLoaderAU` (user IR loader, 8 slots)

- DSP class is `ControlRoomCustom8` (constructor call at 0xc8400).
- `registerParameters` (0xc09ec) names, read from the string table at 0x4249bca.. and 0x424a282..:
  `Learn` (v), `Gain 1..8` (g1..g8), `Pan 1..8`, `Mute 1..8`, `Solo 1..8`, `Invert 1..8` (pi1..pi8),
  `Phase 1..8` (ph1..ph8), `Room 1..8`, `Room Mute 1..8`, `Air`, `Bass`, `Treble`, `Volume`.
- `getPhaseString` (0xc07dc): displays `param * 40.0` (0x42200000 at 0xc07f0) with format `%5.1fcm`
  (0x424a27a). The DSP applies `param * 0.04 m / 343.2 m/s`. Measured headless: Phase at full scale delays the slot by 5.4 samples at
  48 kHz, i.e. the 0.04 m of the code, so the display shows ten times the applied distance (`gr6_blackbox.tsv`).
- `loadIRFromFile` (0xc1138): opens the file with `AudioStreamFactory::createDecoderStream` (0xc1198),
  allocates one channel of the file's full length (`mov w1,#1; mov x2,x23; bl allocateBuffers` at
  0xc1260..0xc1268), reads it and calls `ControlRoomCustom8::openIR(block, fileRate, slot)` (0xc136c).
  No truncation and no normalisation in this function.

## `NI::DSP::MatchedCabinetPro::calcIR` (0x1d370c): A/B crossfade in the IR domain

```
gainA = levelA * (x > 0.5 ? 2*(1 - x) : 1)        ; x = crossfader, from the Hopper pseudo-code
out   = AudioBlock::mix(irA, gainA) ; AudioBlock::mix(irB, gainB)
```

`openIRs` (0x1d3228) sets the two levels from `ExpTable` entries 1020.62 and 930.92, which are -18.0 dB
and -27.0 dB if the table is `2^((i-1200)/60)` [inferred]. No energy or peak measurement of the IRs.

## `NI::DSP::RigCabinet` (0x2bfce8..): the older filter cabinet

Pole/zero cabinet models (`s_CabFilterPZ` @ 0x2be3a3c, 392 kB; `s_CabFilterMap` @ 0x2be2f4c) converted to
biquads at the session rate by `convertPZ2ab` (0x2c266c). Setters: `setParamCabType`, `setParamMicType`,
`setParamMicPos`, `setParamDistance`, `setParamSize`, `setParamRoom`, `setParamBass`, `setParamTreble`,
`setParamPan`, `setParamPhase`, `setParamVolume`, `learnVolume`, `setAmp` (0x2c1214). Internals not read.

Recheck: `python3 $RE/disasm.py "$GR" --dump 'NI::DSP::ControlRoomCustom8::calcIR' | grep -n 'b303c\|b3044\|b35cc\|b35dc\|b3724'`
