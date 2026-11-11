# Literature opened for the tremolo study

Only pages that were fetched and read are listed. "helper" marks pages opened by the literature
helper agent and not re-read by the orchestrator; they are used for background only.

| # | Source | URL | What it gave |
|---|---|---|---|
| L1 | Strymon, "Amplifier Tremolo Technology White Paper" (2012) | https://www.strymon.net/amplifier-tremolo-technology-white-paper/ | Three amp tremolo types. Harmonic: two bands, opposite LFO phase on each, summed. Bias: LFO on the push-pull power valves' bias, crossover distortion near the minimum, more valve distortion near the maximum, supply sag changes the response. Photocell: neon lamp, lamp off (signal high) for well over half the cycle, close to a square wave. No numbers. |
| L2 | D. Morrin, "Fender Harmonic Vibrato" | https://sites.google.com/site/davidmorrinoldsite/home/trouble/trouble-amps/fender-harmonic-vibrato | High-pass 250 pF + 1 M, "about 636Hz". Low-pass 220 k + 5 nF, "about 144Hz". Single-stage passive RC each, "-6dB/oct". Two LFO signals 180 degrees apart, one per band amplifier. |
| L3 | PerkinElmer Optoelectronics, VTL5C3 / VTL5C4 data sheet, with Silonex NSL-32SR2 sheet appended | https://www.qsl.net/wa1ion/vactrol/vactrol.pdf | VTL5C3: turn-on to 63 % of final R_on 2.5 ms typ., turn-off (decay) to 100 kOhm 35 ms max. VTL5C4: 6.0 ms, 1.5 s. NSL-32SR2: rise 5 ms, decay 80 ms. Response curves differ with drive current (10 mA vs 40 mA). |
| L4 | J. Parker, S. D'Angelo, "A Digital Model of the Buchla Lowpass-Gate", DAFx-13 | https://dafx.de/paper-archive/2013/papers/44.dafx2013_submission_56.pdf | Section 3.2: resistance falls faster under more light than it rises when the light goes. Model: a low-pass built on an integrator whose response time is switched by the sign of the input's derivative, about 12 ms going positive and 250 ms going negative for the VTL5C3/2, sped up further at high output values. Eq. 39: R = A / I^1.4 + B. |
| L5 | R. Robinette, "AA1164 Princeton Reverb" | https://robrobinette.com/AA1164_Princeton_Reverb.htm | The Princeton Reverb uses "power tube bias wiggle tremolo". "if you bias the power tubes too hot you will lose some tremolo intensity". It does not load the preamp signal, unlike the AB763 tremolo. |
| L6 | R. Robinette, "AB763 Modifications" (first 100 k characters) | https://robrobinette.com/AB763_Modifications.htm | AB763 tremolo is injected at the vibrato channel's 220 k mixing resistor. The 50 k reverse-audio intensity pot "places a load on the guitar audio signal". Oscillator capacitors .01, .01 and .02 uF. |
| L7 | M. Blencowe, The Valve Wizard, "Tremolo oscillator" | https://valvewizard.co.uk/trem1.html | LFOs run "between 1 and 10Hz". Example oscillator 6 to 11 Hz, 4.3 to 8 Hz with Fender's capacitor value. |
| L8 | ModularGrid listing of the Seymour Duncan SFX-07 Shape Shifter specifications | https://www.modulargrid.net/p/modules/view/14298 | Rate 1 to 20 per second. Wave continuously sine to triangle to square. Shape 10 % to 90 %. Depth 0 to 28 dB. Ratio 1:1, 2:1, 3:1, 4:1. |
| L9 | J. Weber, "The '59-'63 Fender Concert Vibrato", Premier Guitar | https://www.premierguitar.com/articles/the-59-63-fender-concert-vibrato-1 | Low-pass to V1A, high-pass to V1B, LFO on the bias of both in opposite phase, the two LFO feed-throughs cancel at the mixer, the filters' phase shifts make a moving notch. No values. |
| L10 | Wikipedia, "Space charge" (Child's law) | https://en.wikipedia.org/wiki/Space_charge | Space-charge-limited current "varies directly as the three-halves power of the anode voltage". Used as the device law of the bias voice's own model. |
| L11 (helper) | music-electronics-forum thread on the 6G12-A tremolo | https://music-electronics-forum.com/forum/amplification/guitar-amps/theory-design/10589-brown-concert-6g12a-trem-circuit-some-qns | 250 pF and 0.005 uF grid capacitors, 470 k mixing resistors: agrees with L2. |
| L12 (helper) | Premier Guitar review of the Shape Shifter Stereo | https://www.premierguitar.com/gear/seymour-duncan-shape-shifter-stereo-tap-tremolo-review | Wave, Shape, Ratio, channel phase offset up to 180 degrees. |

Not obtained (fetch failed or no source found): Fender blackface speed range in Hz from a primary source,
LDR time constants of the actual Fender "roach", a published perceptual study of tremolo depth laws,
the Najnudel/Mueller DAFx-23 vactrol paper (PDF returned no text), the Eichas/Zoelzer optocoupler paper.
