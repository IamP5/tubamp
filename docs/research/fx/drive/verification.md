# Drive report: independent verification

Verifier's scratch: `$RE_STATE/work/drive-verify/` (own model `mymodel.py`, own analysis `ana.py`, `ss.py`, `p1db.py`, `run_adaa.py`, disassembly under `dis/`).
The in-process harness `rigcall` was read, not built and not run. Every number below marked "AU" was re-rendered through the real Audio Unit with `auhost` under a shell timeout.
Tolerance accepted: 0.2 dB for re-rendered AU figures and for simulations (all re-runs landed within 0.1 dB of the report).
Coordinator note on `sig.py thd` (absolute level read 2.37 dB high before the fix): it does not touch this report. The drive evidence uses `meas.py`'s own spectrum scaling, and a -18 dBFS sine through a bypassed chain reads -18.00 dB with it.

## 1. The in-process harness (`evidence/tools/rigcall.cpp`)

**What it loads.** It calls `dlopen(RTLD_NOW | RTLD_LOCAL)` on a plugin's Mach-O inside its `.component` bundle and reads the image slide from dyld. `models.py` points it at two binaries only: Guitar Rig 6 and AmpliTube 5. Loading runs every static initialiser of the vendor binary inside the harness process.

**Which vendor functions it calls, and how it finds them.** It calls functions by hard-coded vm address plus slide. The addresses come from the symbol tables (`nm`/`disasm.py`); there is no `dlsym` and no pattern scan. Guitar Rig 6 classic pedals: class constructor, `EffectBase::init`, `setSampleRate`, the `setParam*` setters, `resume`, `onControlRate`, `processBuffer`. Guitar Rig 6 `ni::effektrig` cores: constructor, `onInit`, `onSetSampleRate`, `onResume`, `setParameter`, `processControlRate`, `processAudio`. AmpliTube 5: `DynamicFactory<Module>::Instance`, `SetMaxBufferSize`, `SetSampleRate`, `Acquire` (with a made-up GUID), the stomp's `OnSetParameter`, `Module::Process`.

**Patching, hooking, licence checks.** It patches no vendor code and installs no hooks. Its writes go only into memory it allocated itself: `poke*` into the object under test, `vt` stores a vendor vtable pointer into its own zeroed object (to stand in for an inlined constructor), and for AmpliTube it builds a fake wrapper object with a dummy no-op vtable so `OnSetParameter` has something to call. It installs its own crash signal handlers. It names and calls no licence or authorisation function, and skips none by patching. But it enters at the DSP class level, so the plugin's normal instantiation path (the AU factory, where NI and IK consult activation state) never runs. In plain terms: it does not defeat a check, it never reaches one. Vendor DSP therefore runs whether or not the product, or a given AmpliTube gear model, is authorised on this Mac. I could not establish which AmpliTube 5 models are licensed here (AmpliTube 5 cannot be instantiated in `auhost`). Neither binary is on the PACE list. This route is not listed in `tools/re/README.md`; the project owner should rule on it.

**Vendor data copied out.** The harness prints fields of live objects (`peek*`) and can print floats at a global address (`gpeekf`). What reached the repo from it: one line with a table size (`1 1999 1998`) and twelve lines of internal sample rates. No table contents, no network weights, no code. Rendered WAVs stayed in scratch and were deleted by the author.

**Fidelity.** For classes whose constructor is inlined (Cat, BigFuzzPsi, FuzzAce, JackHammer, MulhollandDrive, GainBoost, RigTrebleBooster) the object is built from zeroed memory, so members the real constructor sets are missing. The author's own cross-check shows the Skreamer within 1.1 dB on fundamental, H3 and H5, but H2 differs by up to 14 dB and the small-signal response by up to 2.4 dB; the Cat differs by up to 8 dB. AmpliTube 5 numbers have no AU cross-check at all. All harness-only numbers are treated as second-class below.

## 2. Claims table

Support: **AU** = I re-rendered through the real Audio Unit. **static** = I read the instructions and literals at the address. **source** = tubamp / JUCE source. **sim** = numerical simulation I re-ran or rewrote. **harness only** = the number exists only in the author's harness output.

| id | verdict | support | what I checked | what I saw | note |
| --- | --- | --- | --- | --- | --- |
| C01 | VERIFIED | source + AU | `FxBlocks.cpp:88-105`; AU renders at 60 Hz, 1 kHz, 7 kHz, -70 and -20.94 dBFS, gain 18 dB, drive-only state | Up-sample, `tanh(x*gain)`, down-sample: nothing ahead of the shaper. Gain drops exactly 1.00 dB at -20.94 dBFS at all three frequencies | Drive On=0 gives a clean output, On=1 gives H3 -24.6 dB: the block is in the chain |
| C02 | VERIFIED | AU + source model | AU render 5 kHz, -6 dBFS, gain 36 dB, tone 4 kHz and 12 kHz; my own time-domain model written from the JUCE source | AU: -21.4 dB and -26.8 dB (alias at 1 kHz = folded 19th harmonic). My model: -21.4 and -26.8 | Exact agreement between AU, my model and the report |
| C03 | VERIFIED | source + own computation + AU | `FxBlocks.cpp:52-53`; `juce_Oversampling.cpp:549-574, 276-283, 647-665`; `juce_FilterDesign.cpp:620-690`; `PluginProcessor.cpp:1724-1741` | Constructor args: factor 1 (2x), `filterHalfBandPolyphaseIIR`, max quality, integer latency off. My design code gives 7 and 6 all-pass coefficients (orders 15 and 13). Measured phase delay of the linear path: 3.137 samples. `ChainLatency` counts only amp, amp2 and FX slots; the AU reports latency 0 with the drive on | The source comment at `FxBlocks.cpp:50-51` shows the omission is deliberate |
| C04 | INCONCLUSIVE | AU for the Skreamer; harness only for Deluxe and AT5 | `meas.py gr6au_skreamer pre` through the AU; AT5 corner from C05 | AU Skreamer: -15.0 dB at 110 Hz, -2.7 dB at 640 Hz (report: -15.1 / -2.7 in-process). Deluxe -16.0 and AT5 -15.9 cannot be re-run | The Skreamer figure is a property of its Bass knob at 0.5, see section 3b |
| C05 | VERIFIED | static | `SetSampleRate` 0xdc7fb4-0xdc8014, `Drive` 0xdc82b0 | Stores -4526.935 and -4171881.5 into the A matrix, +4526.935 and +4171881.5 into B, and -1/((drive*500000+51000)*5.1e-11). `Drive(float)` writes the same field (+0x14) the formula reads | 4526.935/(2*pi) = 720.5 Hz |
| C06 | VERIFIED | static | 0xdc8064-0xdc8078, `Oversample(bool)` | Byte at +0x204 selects 1.0 or 4.0, times the sample rate, passed to `KMethod32::Bilinearize` | |
| C07 | VERIFIED | static | `FillCurve` 0xdc6f84-0xdc707c; factory `Acquire` 0x14bc1f0-0x14bc2c8 | 2.55e-9*(exp(v/0.0455)-1) and the mirrored term with -0.0455; stub 0x2165d18 resolves to `_expf`. `Acquire` allocates 24004 bytes (16 + 1999*12), stores 1999 as the size, then calls `FillCurve` | The author's source for 1999 was the harness; the static reading confirms it |
| C08 | VERIFIED | static | Literals at 0x2bde8e0; `onInit` 0x2f44e8-0x2f4588; `processBuffer` 0x2f515c-0x2f51c8 | The six floats are the Catmull-Rom weights for phases 0.25/0.5/0.75 (I recomputed them). `onInit` copies them to +0x160..+0x19c; `processBuffer` multiplies the 4-sample history by those four rows | The 15 decimation taps sum to 1.0 |
| C09 | VERIFIED | static | `TS9::TS9` 0x316ecc; `processBuffer` 0x2f51cc-0x2f5264 | `mov w1,#0x100` into `TableBase(unsigned)`. Lookup: clamp to [+0x100, +0x110], `fcvtzs`, load (value, slope) pair, multiply-add | |
| C10 | VERIFIED | static | 0x5efb38-0x5efda4; literal 0x3bd8078 | 20 calls to `compute_solver_step`, each followed by `fabs; fcmp 1e-4; b.mi exit` | Exit code adds the stage input back: `fadd d0, d1, d8` at 0x5efdb4 |
| C11 | VERIFIED | static (harness printout agrees) | `onSetSampleRate` 0x1c5d6c-0x1c5dbc; constructor 0x5edcd0; stub names via `otool -Iv` | Factor = exp2(round(log2(ldexp(1, e)*44100/sr))), e = 2 passed by the constructor. That gives 4x at 48 kHz (192 kHz), 4x at 44.1 kHz (176.4 kHz), 2x at 96 kHz | The row's own recheck needs the harness; the code alone gives the same rates |
| C12 | VERIFIED | static | `init_intrat` 0x245bc88-0x245be00; literal 0x412de80 | 427.5641 = I0(8) (numpy: 427.5641). Window argument 8*sqrt(1-(t/8)^2), Abramowitz-Stegun I0 polynomials, loop -8..+7 | `init_intrat` is present in the Dirt and Driver symbol lists; I did not trace HM2 |
| C13 | INCONCLUSIVE | AU for tubamp, Dirt, HG-2, Skreamer; harness only for AT5 and Deluxe | AU alias renders | tubamp -21.4, Dirt -80.7, HG-2 -117.3, Driver -77.2: all match. Skreamer through the AU at the harness's knob values: -26.1 (harness: -25.3). AT5 -40.4 / -19.5 and Deluxe -43.3 cannot be re-run | The middle of the ranking rests on the harness |
| C14 | VERIFIED | sim (author's re-run, then my own) | `alias_schemes.py` re-run; my own time-domain model with recursive all-passes | Author's output reproduced byte for byte. Mine: 2x -26.9, 4x -36.7, 4x+ADAA -71.8, 8x -53.2 dB | The figure is true but the metric flatters it, see section 3d |
| C15 | VERIFIED | sim (author's script) | `ts_proto.py` re-run | Peak 720 Hz, -16.5 dB at 110 Hz, H3 -20.8 dB, worst alias -61.0 dB: reproduced | Not independently re-implemented. The committed script does not print the last block of `ts_proto.tsv` (absolute P1dB), so two Table 7 cells are not reproducible from the repo |
| C16 | VERIFIED | literature | Fetched the PDF, extracted the text | Eq. (23) `Vo = V + Vi`; R1 = 4.7k, Cz = 0.047 uF, Cc = 51 pF, R2 = 51k + D500k; sinh diode term | |
| C17 | VERIFIED | literature | Fetched the PDF, extracted Table 1 | None 12x 46.7 dB, Rectangular 4x 46.3 dB, Triangular 3x 46.6 dB | |
| C18 | INCONCLUSIVE | AU for the Skreamer; static for AT5 TS; harness only for the rest | AU Skreamer 1 kHz -18 dBFS drive 0.5 | H2 -87.4 dB (report -87.5). AT5's tabulated diode pair is odd-symmetric by construction (C07). AT5 RAT, SD-1, Fuzz Face and GR6 HM2 numbers cannot be re-run | Through the AU the Skreamer's H2 rises to -42 dB at low drive and level (110 Hz, -18 dBFS, drive 0.1) |
| C19 | INCONCLUSIVE | AU for tubamp; harness only for AT5 and Deluxe | `meas.py ours dyn` through the AU | tubamp: -6.69 to +0.83 dBFS = 7.5 dB at full gain. 11.9 and 16.0 dB cannot be re-run | The mechanism is in the code of both references (output = clipped term + input: AT5 0xdc7a00-0xdc7a10, Deluxe 0x5efdb4) |
| C20 | VERIFIED | static | `processAudioInternal` 0x5ea460; symbol list | Calls `ml::GRU<redfuzzface...state_network>::process` (0x5eb484), `ml::GRU<boumfuzz...>` (0x5ebca8), `ml::GRU<boumsquare...>` (0x5ec1b0), `ml::OutputNetwork<redfuzzface...>` (0x5eb230) and Eigen matrix-vector products | See section 3c |
| C21 | INCONCLUSIVE | AU half verified; harness half cannot be re-run | `meas.py gr6au_skreamer_xcheck thd alias`, `gr6au_skreamer pre` | Every AU number in `gr6_au_vs_inprocess.txt` reproduced to 0.1 dB; AU pre-clip -15.0 / -2.7 dB. The harness column is taken on trust | "1.1 dB" covers fundamental, H3 and H5 only. The same file shows H2 off by up to 14 dB and the response by up to 2.4 dB |

Counts: **VERIFIED 16, NOT VERIFIED 0, INCONCLUSIVE 5.** No row of `claims.tsv` was contradicted. Four statements in the report body were; they are listed in section 5.

## 3. Points given extra scrutiny

### (a) tubamp itself

- **No filtering before the tanh.** True. Source and AU agree (C01).
- **Oversampling configured.** 2x, one polyphase-IIR half-band stage, maximum quality, non-integer latency (C03).
- **Alias floor -21 dB.** True for the stated signal and metric. My model, written from the JUCE source, and the installed AU both give -21.4 dB (tone 4 kHz) and -26.8 dB (tone 12 kHz). The 5.4 dB between them is the tone filter attenuating the 5 kHz fundamental, not more aliasing.
- **3.1 samples unreported.** True. 3.137 samples of low-frequency delay; `getLatencyInSamples()` would return it but nothing in `PluginProcessor.cpp` asks. The AU reports 0.
- **Not true as written:** Summary line 4 says our alias floor is "the worst of the field". The author's own `alias_all.tsv` has AT5 Distortion+ at -17.8 dB with Oversample on, and AT5 Tube Screamer at -19.5 dB with Oversample off. Both are harness-only numbers, and Table 4 omits the Distortion+.

### (b) The Tube Screamer consensus

- **AmpliTube 5.** 720.5 Hz corner, antiparallel symmetric diode pair, and output = state + input: all three read in the code (C05, C07, 0xdc7a00-0xdc7a10). Static only; no AU.
- **Guitar Rig 6 Skreamer Deluxe.** A trapezoidal one-pole high-pass ahead of the solve and `output = clipped + input` are in the code (0x5efae4-0x5efb04, 0x5efdb4). I did not find its corner constant; the 720 Hz reading rests on the harness.
- **Guitar Rig 6 classic Skreamer: the 720 Hz statement is wrong.** `setParamBass` (0x2f49e8) maps the Bass knob to MIDI pitch 114 - 80*bass, which is 587 Hz at the default 0.5. I tested this through the real AU: the 110 Hz pre-clip figure is -22.8 dB at Bass 0.3, -14.9 dB at 0.5 and -7.6 dB at 0.7. A first-order high-pass at 1480, 587 and 233 Hz predicts -22.6, -14.7 and -7.4 dB. So the corner is a user control, 587 Hz at default, and the -15 dB figure is not evidence of a 720 Hz circuit corner. A true 720 Hz first-order high-pass gives -16.4 dB at 110 Hz, which is what the author's own prototype shows (-16.5).
- **AU small-signal response (knobs 0.5, drive 0.5, -70 dBFS sines).** Peak between 640 and 720 Hz, -10.1 dB at 110 Hz, -11.4 dB at 5 kHz. Linear down to -80 dBFS. Matches the report within 0.3 dB.
- **Symmetry at high level (AU).** 1 kHz at -6 dBFS: H2 -97 / -111 / -104 dB at drive 0.1 / 0.5 / 1.0. Symmetric. At low drive and level H2 rises to -42 to -54 dB, so "H2 below -58 dB" holds only near the quoted operating point.
- **Clean added back.** In the classic Skreamer this is the `Clean` knob (`setParamClean` 0x2f4b24, gain 0.5*dB(40*clean-13)). AU check: raising Clean from 0 to 1 lifts the 1 kHz fundamental from -18.4 to -1.2 dBFS while the harmonics stay at the same absolute level. It is a mix control, not the fixed unity path of the circuit. Summary line 2 tags this "measured: precomp_all.tsv", which cannot show a clean path.
- **"Three independent models".** Two are from one vendor.

### (c) Guitar Rig 6 Fuzz Face as a recurrent network

What the binary shows: the process function calls template instantiations named `ni::effektrig::dsp::ml::GRU<...RedFuzzfacePedalData::state_network>::process`, whose arguments are a 2-element input array and a 12-element state array passed by reference, then `ml::OutputNetwork<...output_network>::process` on a 14-element array (12 state + 2 input), with Eigen matrix-vector products between them. The same pattern exists for `boumfuzz` and `boumsquare` (1 input, 8 states). Static-initialiser symbols `__GLOBAL__sub_I_RedFuzzfacePedalData.cpp` and `BoumFuzzBlackboxData.cpp` hold the data. A gated recurrent unit with a carried state vector is a recurrent network. Verified from symbols and call structure; I did not read the gate arithmetic.

### (d) 4x + first-order ADAA

- The author's simulation reproduces exactly, and my own time-domain version gives the same numbers at the author's point (5 kHz, -6 dBFS, 36 dB): -71.8 dB worst alias below 5 kHz, 4.56 samples (4.433 measured for the half-bands, plus 0.125).
- **The -72 dB is not robust to the metric.** It counts only aliases below the fundamental. ADAA suppresses low-frequency aliases best. The worst inharmonic component anywhere below 20 kHz for the same take is -55.5 dB (at 17 kHz); the sum to 20 kHz is -54.0 dB (the report's Table 6 does print this).
- **Other points, my simulation, 36 dB gain, -6 dBFS** (worst below f0 / worst below 20 kHz / sum to 20 kHz, dB):

| f0 | 2x (ships) | 4x | 4x + ADAA | 8x |
| --- | --- | --- | --- | --- |
| 5000 | -26.9 / -25.7 / -20.7 | -36.7 / -34.9 / -30.0 | -71.8 / -55.5 / -54.0 | -53.2 / -51.5 / -46.7 |
| 6100 | -24.3 / -22.9 / -18.1 | -33.0 / -32.1 / -28.0 | -71.0 / -53.8 / -52.8 | -47.2 / -46.4 / -42.4 |
| 7919 | -22.9 / -21.3 / -18.0 | -30.1 / -29.0 / -26.0 | -59.4 / -55.1 / -53.4 | -41.1 / -40.3 / -36.3 |
| 3830 | -30.1 / -28.0 / -22.4 | -42.0 / -39.4 / -33.5 | -76.7 / -58.5 / -56.3 | -63.6 / -61.9 / -56.4 |

- Honest reading: at full gain 4x + ADAA holds the worst audible alias near -54 to -58 dB across frequencies, about 30 dB better than today and 4 to 15 dB better than plain 8x. "-72 dB" and "beats 8x by 18 dB" are best-case figures.
- At 24 dB gain (5 kHz, -6 dBFS) and at -18 dBFS input (6.1 kHz, 36 dB) plain 8x is cleaner than 4x + ADAA on the sum (-96.0 against -88.5 dB; -94.8 against -79.4 dB). Both are far below audibility there, so the recommendation stands.
- The antiderivative `F(u) = u*v - v^2/2 - k*a*(cosh(v/a) - 1)` is correct (dF/du = v). The Newton table reproduces: 4 iterations leave at most 7.8e-7 V.

## 4. The three top-ranked recommendations

**R1 (typed engine: voicing filter, volt-referenced diode clipper, clean-through).** Follows in part. The need for pre-clip voicing is solid: tubamp flat (AU), Skreamer shaped (AU), AT5 circuit corner (static). It leans on: harness-only pre-clip and dynamics numbers for AT5 and the Deluxe (C04, C19); `V_REF` = 1 V, which is the author's choice; and the sentence that the clean path "explains the dynamics gap", which is inferred. The sentence that the prototype "lands inside the spread of the three references on every measured quantity" is not true for the pre-clip row of Table 7: -16.5 / -10.6 / -3.4 against a spread of -15.1..-16.0 / -9.4..-10.1 / -2.7..-3.1. The miss is 0.3 to 0.5 dB and comes from comparing a 720 Hz circuit with a 587 Hz Skreamer.

**R2 (4x polyphase IIR + first-order ADAA).** Follows from verified claims (C02, C03, C14, C17) and a correct derivation. The headline -72 dB should read about -55 dB for the worst alias below 20 kHz. Its acceptance threshold (-55 dB below 5 kHz for a 5 kHz tone) is met with margin in simulation. The CPU figure is marked as a guess in the report.

**R3 (Screamer type).** Follows from the verified literature values (C16) and from AT5's code using the same circuit (C05). It leans on: the d² taper fitted to harness-only gain steps; the tone-stage expression, which is the author's own nodal analysis and was checked only inside the author's prototype; and Is = 2.52 nA, a = 45.3 mV, which the report takes from the decompiled AmpliTube values (2.55 nA / 45.5 mV at 0xdc6f9c-0xdc6fc0, 2.52 nA at 0xda1528) and not from a cited source. The DAFx-07 paper text does not give those two numbers.

None of the top three depends on the constants the report marks as guesses (Muff tone corners, RAT capacitors and series resistor); those belong to R5 and R6.

## 5. Statements in the report that the evidence contradicts or over-states

1. **Summary 2, section 5 item 2, Table 1 "reading" column:** "a first-order 720 Hz high-pass" for all three Tube Screamers. For the GR6 classic Skreamer it is 587 Hz at the default Bass setting and moves with the knob (AU test above). 720 Hz is read in code for AT5 only.
2. **Summary 2:** "clean input added back at the output" is tagged as measured by `precomp_all.tsv`. That file cannot show it. It is in the code of AT5 and the Deluxe; in the classic Skreamer it is a user knob.
3. **Summary 4:** "worst of the field". Contradicted by the author's own `alias_all.tsv` (AT5 Distortion+ -17.8 dB).
4. **Summary 4:** latency "about 4.6 samples" carries a measured tag; Table 6 and R2 mark it inferred. The value is right (I measured 4.433 + 0.125).
5. **Section 7 header:** "All constants are ours: circuit values from the cited literature, or fits to the measurements." R3's diode constants come from decompiled values, and R5/R6 contain constants the report itself marks as guesses.
6. **R1:** "inside the spread ... on every measured quantity" (see section 4).
7. **R4:** "3 samples for type 0" through JUCE `useIntegerLatency`. JUCE adds a fractional delay to reach the next integer: 3.137 becomes 4, and 4.433 becomes 6 (`juce_Oversampling.cpp:762-770`). The comb null for two lanes sits at 7.38 kHz by my computation, not 7.6 kHz, because the delay grows with frequency.
8. **R5:** the 100 kΩ pot and the 1.5 kΩ / 100 kΩ / 3.3 nF Filter values carry a literature tag. The cited page states the gain formula, about 2300 (67 dB), ±0.65 V, and the 32 kHz and 475 Hz end points; the component values are back-computed from those.
9. **Section 5 item 3:** "H2 below -58 dB" for overdrives. Through the AU the Skreamer reaches -42 dB at low drive and level.

## 6. Citations opened

| URL | exists | says what the report attributes |
| --- | --- | --- |
| ccrma.stanford.edu/~dtyeh/papers/yeh07_dafx_distortion.pdf | yes | Yes. Eq. (23), all TS and tone-stage values, 15.9 / 15.6 Hz, DS-1 values (36 dB, 3 Hz and 600 Hz poles, 250 pF, 2.2k, 320 Hz and 1.16 kHz), "upsample by a factor of eight or ten", aliases masked at eight or above, even-order content not modelled |
| dafx.de/paper-archive/2016/dafxpapers/20-DAFx-16_paper_41-PN.pdf | yes | Yes. Parker, Zavalishin, Le Bivic. Table 1 as quoted, `F0 = log(cosh(x))`, ill-conditioning warning |
| cushychicken.github.io/ltspice-proco-rat | yes | Mostly. Gain formula, about 2300 / 67 dB, ±0.65 V, 32 kHz to 475 Hz, slew-limited LM308. Component values for the pot and the Filter network are not in the text |
| guitarscience.net/papers/bigmuff.pdf | yes | Yes. Input gain stage, two clipping stages, tone stage of one first-order low-pass and one high-pass with a notch between, output stage |

## 7. Fences

- **PACE.** Respected by this study. The harness loads Guitar Rig 6 and AmpliTube 5 only. AU renders touched NI Driver, Dirt, Supercharger, Guitar Rig 6, Plugin Alliance HG-2 and tubamp. A search of `$RE_STATE/work/drive` for uaudio, Soundtoys, Relab, Sonnox, Antares and Lexicon names finds nothing, and the thin-slice cache holds no such binary. (The shared `symbols/` directory contains `uaudio_studio_d_chorus.txt`. That is not from the drive study, but the coordinator should look at it.)
- **Listing length.** Respected. `decompiled_excerpts.txt` holds seven excerpts; the longest is 27 instruction lines. The report pastes no listing.
- **Vendor tables, weights, code.** None stored. `gr6_table_shapes.txt` holds shape statistics of seven Guitar Rig tables (extremes, peak slope, knee distance), no contents. Two things to note: `evidence/tools/gr6au/*.xml` are four factory INIT parameter blocks cut from NI preset files (ids and values, about 1.3 kB each); and R3's diode constants are lifted from decompiled values, as said above. Two textbook scalars are not a table, but they should be re-sourced from a datasheet or paper.
- **Harness.** Not a listed route. It bypasses no check by patching, but it runs vendor DSP without the product's own instantiation path. Owner's call.
- **My own scope.** I wrote only this file and scratch under `$RE_STATE/work/drive-verify/`. A cleanup `rm` of my scratch WAVs was refused by the shell safety check; about 1 MB of small files remains in `drive-verify/wav/`.

## 8. Overall judgment

The static reading is sound: every decompiled claim I checked holds at the named address, and the code path loads the constant the way the claim says. The claims about tubamp are exact: source, my own model and the installed AU agree to 0.1 dB. The literature is quoted correctly. The weak part is measurement of Guitar Rig's newer pedals and of all AmpliTube pedals, which exists only through an unlisted in-process harness; five claims stay inconclusive for that reason, though for the Skreamer the real AU confirms the harness closely. Two conclusions are over-stated: the "720 Hz consensus" is a circuit fact for AmpliTube and the literature, but the classic Guitar Rig Skreamer sits at 587 Hz by a knob default; and the -72 dB alias figure is a best case, with about -55 dB the defensible number. Neither changes the direction of the top recommendations. Pre-clip voicing, a clean-through path, and 4x with ADAA all follow from verified evidence; the specific constants in R3, R5 and R6 need literature or datasheet sources before they are built.
