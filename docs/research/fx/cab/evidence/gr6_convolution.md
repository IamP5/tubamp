# Guitar Rig 6: convolution engine and convolution cabinets

Binary `GR` = `/Library/Audio/Plug-Ins/Components/Guitar Rig 6.component/Contents/MacOS/Guitar Rig 6` (arm64).
Tools: `python3 $RE/disasm.py "$GR" ...`; Hopper pseudo-code from the shared cache (`hopper.py "$GR" 0xADDR`).
All addresses are vm addresses in the arm64 slice.

## 1. `NI::DSP::Convolver` (the engine)

### Partition schedule: `NI::DSP::Convolver::PartitionArray` @ 0x464b598

283 records of 16 int32 (64 bytes): `{maxIrLength, numStages, (blockSize, count) x 7}`.
`setImpulseResponse` (0x6155f4) clamps the IR length to the last record's `maxIrLength`
(67108864 = 2^26 samples, read at 0x464fc18) and picks the first record whose
`maxIrLength >= length` (loop at the top of the function, 0x11b = 283 records).

Sample records (not the whole table):

```
maxLen  stages  (block,count)...
   384  1  64x4
  1024  1  64x14
  1920  1  64x28                      <- last single-stage record
  2048  2  64x10  256x5
  6912  2  64x10  256x24
  7168  2  64x22  512x11
 12288  2  64x22  512x21
 14336  3  64x10  256x9  1024x11
 51200  3  64x10  256x21 2048x22
139264  4  64x10  256x9  1024x21 8192x14
67108864 7 64x10 256x9 1024x21 8192x21 65536x13 262144x28 1048576x56
```

In every record `maxLen = 128 + sum(block*count)`: 128 samples are not covered by FFT partitions.
Each stage starts at an offset of three times its block size (64x10 = 640, +128 = 768 = 3x256;
768 + 9x256 = 3072 = 3x1024; 3072 + 21x1024 = 24576 = 3x8192).

Recheck: `python3 evidence/bb/readints.py $RE "$GR" 0x464b598 4528 16 | sed -n '1p;25p;26p;45p;46p;283p'`
(row = address, maxLen, stages, then block/count pairs; the address is in `$RE_STATE/symbols/Guitar_Rig_6.txt`).

### Zero-latency head: 128-tap direct FIR

`processBuffer` (0x61714c), per output sample, mono path:

```
619fe4  fmov  s0, w10            ; input sample
619fe8  dup.4s v0, v0[0]
619fec  ldr   x9, [x22, #0x398]  ; head coefficients, 32 vectors of 4
619ff0  ldr   q1, [x9, x11, lsl #4]
619ff4  ldr   w9, [x22, #0x3b0]  ; ring index
619ff8  add   w9, w11, w9
619ffc  and   w9, w9, #0x1f      ; ring of 32 vectors
61a000  ldr   x10, [x22, #0x390] ; accumulator ring
61a008  ldr   q2, [x10, x9]
61a00c  fmul.4s v1, v0, v1       ; x[n] * h[4k..4k+3]
61a010  fadd.4s v1, v1, v2
61a014  str   q1, [x10, x9]
61a018  add   x11, x11, #0x1
61a01c  cmp   x11, #0x20         ; 32 vectors = 128 taps
61a020  b.ne  0x619fec
```

A transposed direct-form FIR of 128 taps runs on every sample; the FFT partition outputs are
added from an overlap buffer afterwards (0x61a074..). The stereo path repeats the loop at 0x61a1b0.
`setImpulseResponse` stores `2 * firstBlockSize` (= 128) at `this+0x310` (0x6156cc).

### FFT

Own split-radix FFT (`SplitRadixFFT` 0x658488, `FastSplitRadixFFT` 0x65bea4, `SplitRadixButterfly`
0x65c438), no vDSP call in `processBuffer` (its only external call is `bzero`). The butterfly work of
the larger stages is done in slices of at most 64 input samples (`cmp w10,#0x40` clamp near the top
of the per-stage loop in `processBuffer`), with per-stage operation tables sized by
`FFTmemoryArray` @ 0x464fc58 (records `{fftSize, ...}` for 64 .. 2097152).

Recheck: `python3 $RE/disasm.py "$GR" --dump 'NI::DSP::Convolver::processBuffer' --max-bytes 200000 | grep -n '61a01c\|61a1b0'`

## 2. `NI::DSP::ConvolutionBase` (IR management, 0x2694a0..0x26af88)

- Two `Convolver` objects, created in `onInit` (0x269718), pointers at `this+0x2a0/0x2a8`, with a
  current/next pair at `+0x2b0/+0x2b8` that `doBackgroundWork` swaps.
- `onSetSampleRate` (0x2699a4):
  - `this+0x10 == 2` (the effect runs 2x oversampled): convolver rate = `0.5 * sampleRate`
    (`fmul s1, s0, s8` at 0x269a40, s8 = 0.5), with two `SampleRateConverter::init(.., 2 ch, quality 2)`
    for down and up (0x269a58, 0x269a74). The convolution runs at the base rate.
  - crossfade length = `floor(sampleRate * 0.02 / blockSize + 0.5)` blocks (0.02 = 0x3ca3d70a at
    0x2699e0 / 0x269a8c), stored at `+0x2fc`; ramp increment `1 / (blocks * blockSize)` at `+0x300`.
  - volume-learn window = `sampleRate * 5 + 0.5` samples (`fmov s1, #5.0` at 0x269b40), at `+0x2f4`.
- `doBackgroundWork` (0x26a3b4): calls the virtual `calcIR` (vtable +0x60) to get the IR block, its
  sample rate and a stereo flag; if the IR rate differs from the convolver rate it runs
  `SampleRateConverter::init(.., 2, quality 2)` (0x26a5a0), `convert` (0x26a664) and
  `AudioBlock::amplify(irRate / convolverRate)` (0x26a674 `fdiv`, 0x26a67c); then
  `Convolver::setImpulseResponse` on the idle convolver, and swaps the pair.
- `NI::AB::SampleRateConverter::InterpolCoefficients` ctor (0x245e4ec): quality 0 = 4 points, order 3;
  quality 1 = 6 points, order 3; quality 2 = 8 points, order 5 (int pairs at 0x37fcd68, 0x412dea8,
  0x412de88). Integer-ratio fast paths: `process_intratUP/DOWN/DOUBLE/HALF`.

## 3. `NI::DSP::ConvolutionCabBase::processBuffer` (0x26bc10)

Order of operations (Hopper pseudo-code of 0x26bc10, checked against the disassembly):

1. if bypass fade is pending, keep a copy of the dry input;
2. if not stereo (`this+0x2d8 == 0`): input = `(L + R) * 0.5`;
3. mode 2: downsample by 2;
4. during an IR change: the old convolver runs on the input and the new one too; the two outputs are
   mixed with a per-sample linear ramp (`this+0x3c0` state, increment `+0x780`);
5. mode 2: upsample;
6. `Air_GR::processBuffer` (0x25c420) on the output;
7. volume learning: while active, `peak = max|out * volume|`; if `peak > 0.50118` (0x3f004dce at
   0x26c068) then `volume *= 0.50118 / peak`, floor 0.1 (0x3dcccccd at 0x26c110); the window is the 5 s
   set above; at the end the volume parameter is written back;
8. volume with per-sample smoothing; output peak meters;
9. bypass crossfade against the dry copy.

`Air_GR::onSetSampleRate` (0x25c0fc): delay ring sized for `0.045 s` (0x3d386572 at 0x25c16c); tap
delays `0.015 s * 1.17^k` (0x3c75c28f at 0x25c254, 0x3f95c28f at 0x25c268), eight taps
(15.0, 17.6, 20.5, 24.0, 28.1, 32.9, 38.5, 45.0 ms); two one-pole coefficients `2*pi*f/fs`
(6.2832 = 0x40c90e56 at 0x25c1cc) with `f` read from `NI::AB::ExpTable` entries 1665.88 and 1790.88,
which are 218 Hz and 922 Hz if the table is `2^((i-1200)/60)` [inferred from the dB lookups
`dB*9.9658 + 1200` used throughout the class].

## 4. Amp impedance hand-off

- `ConvolutionCabBase::tellAmpImpedance` (0x26b5f8) walks a list of linked effects and calls the virtual
  at vtable +0x120 with `(int this+0x418, const float* this+0x420)`;
  `resetAmpImpedance` (0x26b7a8) sends `(0x1b, nullptr)`.
- Receivers: `NI::RIG::COMP::ComponentAudioAmpStereo<Amp>::receiveImpedance(int, float const*)` and
  `NI::DSP::<Amp>::setCabinetImpedance(int, float const*)` for 15 amp classes (RigPlexi 0x2db578,
  RigFender 0x2c6010, RigAC30 0x2bb8bc, ...).
- `getImpedanceType()`: 2 for `CabinetHeadAudio`, `ControlRoomAU`, `ControlRoomCustom8AU`, `IRLoaderAU`,
  `OriginalCabinetAU`, `MatchedCabinetProAU` (0xa5210, 0xc32dc, 0xc346c, 0xc3574, 0x13b1e4, 0x1d4cf8);
  1 for `ComponentAudioAmpBase` (0x20cfb8); 0 for the base class (0x281d8).
- `NI::DSP::RigCabinet::s_CabImpedancePZ` @ 0x2c43644: 28 records of 24 float32. Record layout as read
  by `RigPlexi::setCabinetImpedance` (0x2db5ec..0x2db8d4): two sections of 9 floats
  `{z1.re, z1.im, z2.re, z2.im, p.re, p.im, p*.re, p*.im, gain}`, one first-order section
  `{z.re, z.im, p.re, p.im, gain}`, one scale. Units are Hz (the bilinear constant is `fs / pi`,
  0x3ea2f983 at 0x2db5c4). Each root `r` is mapped with `(k + r) / (k - r)` (std::complex divide at
  0x2db650..), then expanded to biquad coefficients. Record 27 is all zero with gain 1; id 27 (0x1b)
  short-circuits to a plain gain (0x2db5b0).
- Shape of the 27 real records (summary, not the table): section 1 = zero at 0 and at -100 kHz, pole
  pair at 30..120 Hz with Q 2.6..15 (a band-pass resonance); section 2 = the same form at 18..630 Hz
  (one outlier 2.1 kHz); section 3 = zero 0.7..1.5 kHz, pole 1.6..3.9 kHz, gain 0.86..3.06 (three
  records use 6.1..7.0 kHz / 7.4..15.2 kHz).

Recheck: `python3 $RE/disasm.py "$GR" --floats 0x2c43644 48` and
`python3 $RE/disasm.py "$GR" --dump 'NI::DSP::RigPlexi::setCabinetImpedance' | sed -n 1,60p`.
