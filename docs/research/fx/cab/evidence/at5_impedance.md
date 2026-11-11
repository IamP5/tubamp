# AmpliTube 5: speaker impedance filter between amp and cab

Binary `AT` = `/Library/Audio/Plug-Ins/Components/AmpliTube 5.component/Contents/MacOS/AmpliTube 5` (arm64).
The cabinet itself is documented in `at5_cab_notes.txt` (helper notes, spot-checked: engine choice 0x102bc78,
IR length 0x154a9e0..f8 and block 0x154b768/6c, crop lengths 0x1bde8c..c0, inter-mic delay 0xf120b4..e4).

## Classes

- `IK::MAF::AT3::Impedance` (module: `DefineParameters` 0x16d26b4, `SetParameter` 0x16d1ec4,
  `ProcessBuffer` 0x16d2e6c). `SetParameter` ends in
  `ImpedanceFilter::Module::UpdateImpedance1Left` / `...Right` (call at 0x16d217c, tail call 0x16d2194).
- `IK::Products::AT4::ImpedanceModuleManager::OnAmpAParameter / OnCabAParameter` (0x3c8880, 0x3c8ad8, and
  B, C): the filter is re-set when either the amp or the cab of a chain changes.
- `IK::KIS::FX::AT5::ImpedanceFilter::Module::UpdateImpedance1Left` (0xf1aba8, 460 bytes) and
  `UpdateImpedance2Left` (0xf1af40, 720 bytes).
- Data: `IK::KIS::FX::AT5::ImpedanceFilter::gcImpedanceVector` @ 0x3171fe8, 103 records of 13 x 4 bytes
  (0x31734d4 - 0x3171fe8 = 5356 = 103 x 52). `MAF::AT3::Cabinet` has a `CabModel` parameter with 103
  states (`at5_cab_notes.txt`, parameter table). `gcAmpSettingsVector` @ 0x36f4b80 (bss) holds the amp side.

## What `UpdateImpedance1Left` computes

```
f1aba8  ldr  x8, [x0, #0x28]!        ; state-space matrix storage
f1abac  ldur s3, [x0, #-0x8]         ; amp-side value (output resistance)
f1abb0  ldur x9, [x0, #-0x18]        ; -> impedance record
f1abb4  ldp  s0, s1, [x9, #0x8]      ; record[2], record[3]
f1abb8  ldp  s2, s7, [x9, #0x10]     ; record[4], record[5]
f1abbc  ldp  s16, s17, [x9, #0x1c]   ; record[7], record[8]
f1abc0  ldr  s18, [x9, #0x24]        ; record[9]
f1abc4  ldr  s19, [x9, #0x2c]        ; record[11]
f1abc8  fadd s4, s3, s0              ; Rout + R
...                                  ; entries of the form -R/L, 1/L, 1/C, -1/(R*C), all in double
f1ad70  b    StateSpace64<4,1,1>::Bilinearize(double)
```

A 4x4 A matrix, B, C and D are filled with quotients of sums of resistances over inductances and
capacitances, then `IK::KIS::TK::StateSpace::StateSpace64<4,1,1>::Bilinearize` (0xc65e20) discretises it.
`UpdateImpedance2` is the same with a 6-state model. The model is a lumped electrical circuit of the
loudspeaker driven through the amplifier's output resistance.

## Record contents (first three records, read with `disasm.py --floats 0x3171fe8 40`)

```
type  nominal  R      R      R      R      -  L         L         L        -  C         -
1     16.0     12.5   23.32  20.85  53.09  0  0.000231  0.000916  0.01852  0  0.000305  0
1     16.0     10.2   33.75  12.97  67.15  0  0.000297  0.000536  0.03022  0  0.000236  0
1      8.0      5.3   14.99  12.00   9.96  0  0.000150  0.000457  0.00376  0  0.001118  0
```

Read as ohms, henries and farads: a nominal impedance, a voice-coil resistance of 0.64..0.78 of nominal,
two small inductances with their loss resistors (a two-stage voice-coil inductance), and a large L with a
C and a parallel R that resonate at `1/(2*pi*sqrt(L*C))` = 67.0 Hz, 59.7 Hz and 77.6 Hz. [inferred: the
unit reading is ours; the resonance arithmetic follows from it]

Recheck:
`python3 $RE/disasm.py "$AT" --dump 'ImpedanceFilter::Module::UpdateImpedance1Left' | sed -n 1,20p`
`python3 $RE/disasm.py "$AT" --floats 0x3171fe8 39`
`python3 $RE/disasm.py "$AT" --list 'ImpedanceModuleManager::On(Amp|Cab)AParameter|MAF::AT3::Impedance::'`
