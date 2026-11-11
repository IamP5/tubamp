# Chorus literature notes (BBD choruses and digital chorus design)

Conventions: `[literature: URL]` = I opened and read that page/PDF in this session.
`[derived]` = my own arithmetic from sourced numbers (formula stated). `[unverified memory]` = no opened source.
`[snippet only]` = a search-result summary mentioned it but I could not open the page; treat as unverified.
Could NOT open: electrosmash.com (DNS failure on every attempt), web.archive.org (blocked), Roland support, diystompboxes (403), freestompboxes (403), anasounds (403), scribd.
Local working copies (text extractions) are in the same directory (ce1-1.png = CE-1 schematic render, dattorro.txt, raffel.txt, holters.txt, deip.txt).

## 1. Boss CE-1 (1976)

- BBD is the Matsushita MN3002, a single 512-stage chip, listed range 0.32-25.6 ms (chip capability, not CE-1 operating range). [literature: https://djjondent.blogspot.com/2017/10/boss-ce-1-chorus-ensemble-panasonic-bbd.html] The same chip is named in [literature: https://catalinbread.com/blogs/kulas-cabinet/boss-ce-1-chorus-ensemble].
- Chorus and vibrato modes share one BBD circuit and cannot be used at the same time; stereo outputs; derived from the JC-120 circuit (1975). [literature: https://www.tonehome.de/boss/ce-1-chorus-ens/]
- Operating delay range 1.3 to 4 ms for the CE-1 (stated without derivation). [literature: https://electricdruid.net/weird-wiggles-bbd-clock-modulation/]
- Service schematic ET-10C (image-only PDF, I rendered and read it): IC-2 = MN3002; the scope-reference strip at the bottom lists waveform G (BBD clock) as "5 us ~ 16 us" [literature: https://www.tonehome.de/app/download/8747046/boss_ce1_chorus.pdf]. Reading this as the clock period range gives clock = 62.5-200 kHz and delay = 512 * T / 2 = 1.28 ms (T=5 us) to 4.10 ms (T=16 us) [derived], which matches the 1.3-4 ms figure above. The interpretation of "5us~16us" as period is mine; the figure itself is on the schematic.
- Same schematic, LFO: the strip shows a triangle at "CHORUS MIN" (points I, K: 11 Vp-p and 23 Vp-p) and a sine-like wave at "VIB. MAX" (point L: 6.5 Vp-p). So chorus = triangle LFO, vibrato = sine-like LFO, from the scope picture only. [literature: https://www.tonehome.de/app/download/8747046/boss_ce1_chorus.pdf] The schematic gives no LFO Hz; the rate network (R64 3.9k, C35 4.7 uF, C34 10 uF, VR4 100k C-taper, R59 220k, S4 chorus/vibrato) is there but I did not compute a rate; **LFO Hz for the CE-1: not found.**
- Same schematic, preamp: input with HIGH/LOW switch; LOW path is jack to level pot (VR11 50k A) to IC-1 (TA7136P op-amp, R11 470k feedback, R8 10k input as I read it, so voltage gain about 47 = 33.5 dB [derived from my reading of the image, moderate confidence]); HIGH path adds a one-transistor "MIC AMP" (Q1). Scope strip: point A 8 mVp-p at 440 Hz, B 80 mVp-p, C and D 4 Vp-p, so about 50x between B and C. After IC-1 sits a passive RC low-pass (R13 22k, R14/R16 10k, C12 .0047, C11 .001 as read) into Q2 and then the BBD. [literature: https://www.tonehome.de/app/download/8747046/boss_ce1_chorus.pdf] Level control is 50k and a transistor "mic amp" in high mode gives asymmetrical clipping. [literature: https://articles.boss.info/the-legend-of-the-ce-1-and-re-201-preamps/]
- Post-BBD: Q7/Q10 emitter-follower stages with a multi-RC low-pass (R41 22k, R42 6.8k, R43/R44 15k, C26 .0047, C28/C29 .001, C27 470p as read); scope strip point E 2 Vp-p showing the clock-feedthrough spikes, F 750 mVp-p. No compander (no NE570/571 on the schematic). [literature: https://www.tonehome.de/app/download/8747046/boss_ce1_chorus.pdf] Cutoff Hz not computed; **not found as a stated number.**
- Output: two outputs fed by FET stages Q11 and Q13 (Q12 shunt), an EFFECT/NORMAL switch S3, and a MONO/STEREO jack arrangement. The strip shows output M 240 mV and output N 350 mVp-p "3 msec of CHORUS MIN". How dry and wet are assigned to each jack, and the dry:wet ratio: **not established** (I could not trace it reliably).
- Vibrato = chorus path with no dry mixed in. [unverified memory; also asserted by a forum post surfaced only as a search snippet]
- Companders are not typically used in chorus/flanger/vibrato BBD circuits because delay lines are short. [literature: http://colinraffel.com/publications/dafx2010practical.pdf]

## 2. Boss CE-2 (MN3007, 1024 stages, MN3101 clock)

ElectroSmash page could not be fetched; the numbers below are secondary.
- Chips MN3007 (1024 stages) + MN3101 clock driver. [snippet only: https://electrosmash.com/boss-ce-2-analysis]
- MN3007 datasheet figures as quoted by a forum poster: clock 10-100 kHz, delay 5.12-51.2 ms; CE-2 uses a fixed clock setting (no trimmer); CE-2 delay "something like 5-40 ms" (unverified guess); CS-9 3.2-8.5 ms. [literature: https://madbeanpedals.com/forum/index.php?msg=316735] Check: 1024/(2*100 kHz)=5.12 ms, 1024/(2*10 kHz)=51.2 ms [derived], consistent.
- Same thread heuristic: clock should be roughly 3.5x the knee of the pre-BBD low-pass; suggested pre-BBD cutoff about 5.5 kHz (min clock about 22 kHz); at about 10 kHz clock the bandwidth into the BBD tops out near 3 kHz. [literature: https://madbeanpedals.com/forum/index.php?msg=316735] (forum opinion, not measurement)
- A reader comment on Electric Druid's article gives CE-2 total delay about 8.5-10.75 ms with linear clock response within ~1%. [literature: https://electricdruid.net/investigations-into-what-a-bbd-chorus-unit-really-does/] (a comment, not the article body)
- LFO 3.5 Hz at maximum rate (oscilloscope) and "8.4-11.6 ms at mid depth" for a DIY emulation: [snippet only: search result summary, page not opened].
- LFO shape triangle (CE-2 depth control sets triangle amplitude): [snippet only].
- Dry:wet ratio, filter cutoffs: **not found**.

## 3. Roland Dimension D (SDD-320) / Boss DC-2

Best numbers (Anwander's analysis of the service material): SDD-320 uses 2 x MN3007, one LFO; input passive ~80 Hz 3 dB high-pass, then modulated delay with companding noise reduction. [literature: http://www.florian-anwander.de/roland_string_choruses/]

| Mode | Delay | Mod amount (+/-) | Mod speed | Same-side pad |
|---|---|---|---|---|
| 1 | 10 ms | 2.0 ms | 0.25 Hz | no |
| 2 | 7.5 ms | 2.5 ms | 0.25 Hz | no |
| 3 | 7.5 ms | 1.5 ms | 0.50 Hz | no |
| 4 | 7.5 ms | 1.5 ms | 0.50 Hz | yes (-10 dB) |

[literature: http://www.florian-anwander.de/roland_string_choruses/]
- Output mix: "each output mixes its own modulated signal with its own original and the other channel's original" (Dimension-D). Standard Roland mono-to-stereo chorus: two delay lines, one LFO, inverted modulation on one line, dry mixed 1:1 into both outputs. [literature: http://www.florian-anwander.de/roland_string_choruses/]
- Implied clock at the 10 ms centre = 1024/(2*0.010) = 51.2 kHz; +/-2 ms is +/-20% in delay [derived].
- Two companded BBD lines with pre/de-emphasis; mode buttons 1-4 (1 mildest, 4 most extreme), 1+4/2+4/3+4 combos exist on the SDD-320 [snippet only: musictech.com Klark Teknik article, not opened]. Two clock generators and two BBD lines are needed for a true dimension chorus. [literature: https://bbd320.blogspot.com/2025/10/schematic-service-notes-for-roland-sdd.html]
- Two delays driven in antiphase by a triangle LFO, with no numbers. [literature: https://electricdruid.net/weird-wiggles-bbd-clock-modulation/] "Trapezoidal LFO to avoid warble": [snippet only].
- The cross-mix of inverted wet signals into the opposite channels: [unverified memory]. Anwander describes cross-mixing of the *originals*, not inverted wet; I could not confirm wet polarity inversion.
- Why "width without wobble": pitch deviation is tiny. peak cents = 1200*log2(1 + 2*pi*f*D) [derived, see topic 8]: mode 1 (0.25 Hz, 2.0 ms) = 5.4 cents; mode 2 (0.25 Hz, 2.5 ms) = 6.8 cents; mode 3/4 (0.5 Hz, 1.5 ms) = 8.2 cents [derived from Anwander's table].
- DC-2 (Dimension C): 13 ICs incl. two BBD lines and two companders; four mode buttons; based on SDD-320 [snippet only]. A DC-2 builder thread names BL3102 BBDs and two NE570 companders [literature: https://madbeanpedals.com/forum/index.php?msg=221127]. DC-2 LFO rates and depths per mode: **not found**.

## 4. Tri-chorus, Juno, string ensembles

Juno-6/60 (Anwander, mono-to-stereo chorus, one triangle LFO unless noted):

| Unit | Mode | Depth | LFO rate |
|---|---|---|---|
| Juno-6 | I | 100% | 0.4 Hz |
| Juno-6 | II | 100% | 0.6 Hz |
| Juno-6 | I+II | 25% | ~8 Hz (sine-like) |
| Juno-60 | I | 100% | 0.5 Hz |
| Juno-60 | II | 100% | 0.8 Hz |
| Juno-60 | I+II | 8% | 1 Hz (sine-like) |

[literature: http://www.florian-anwander.de/roland_string_choruses/] Juno-60 speeds are switched by VC switches; I+II "fast mode" drives the second BBD non-inverted; Juno-106 has one LFO with two speeds (frequencies not in the service manual, assumed similar to Juno-60); delay and depth "still need measurement" for most Juno units (same page). So the "~1.5-5.4 ms delay range" hypothesis is **not confirmed**.
- Juno BBD: MN3009 (datasheet 0.64-12.8 ms); 256 stages implied: 256/(2*200 kHz)=0.64 ms [derived]. [literature: https://www.eventideaudio.com/?p=154476]
- Same Eventide forum reply says the Juno's L/C/R LFOs are 120 degrees apart. This contradicts Anwander (one LFO, inverted on one line) so treat 120 degrees for the Juno as doubtful. [literature: https://www.eventideaudio.com/?p=154476]
- Juno-60 BBD clock minimum about 26 kHz, driven by a triangular LFO; the BBD adds ~2.3 dB gain; in/out filters are sixth order = first-order high-pass + fifth-order low-pass. [literature: https://www.dafx.de/paper-archive/2018/papers/DAFx2018_paper_12.pdf]
- Other Roland units on the same page: JX-3P 0.25 Hz fixed; RS-202 6.25 Hz and 0.66 Hz pairs; RS-505 0.17 Hz tri / 6.25 Hz sine / 0.14 Hz tri / 5.5 Hz sine; RS-09 0.15, 4.7, 0.18, 5.9 Hz; SA-09 chorus 0.24 Hz, vibrato 3.8-7.9 Hz; RS-101 12 Hz base divided to 6/3/1.5 Hz. [literature: http://www.florian-anwander.de/roland_string_choruses/]
- Solina/ARP Omni: forum discussion says three BBD lines each with its own LFO, 2 LFOs of different speed with shifted phases, no dry signal in the ARP Omni; stage counts and rates not given. [literature: https://www.kvraudio.com/forum/viewtopic.php?p=3269504]
- Dytronics CS-5 Tri-Stereo: three BBD lines (L, R, centre), shared LFO with per-channel depth controls [snippet only: MusicRadar]. Phase offsets and rates: **not found**. The "slow 0.6 Hz + fast 6 Hz" pair: [unverified memory]; the closest sourced analog is the Roland RS-202 pair (6.25 Hz and 0.66 Hz) above.
- Roland Alpha-Juno/MKS-50 rate measurements: [snippet only], page without the data table opened.

## 5. BBD modelling

- Delay = N / (2 f_clock). [literature: http://colinraffel.com/publications/dafx2010practical.pdf] Examples there: 4096 stages at 300 ms needs 6826 Hz; flanger 1024 stages at 10 ms needs 51.2 kHz.
- Raffel & Smith DAFx-10: signal chain input low-pass (anti-alias) -> compander compressor -> BBD -> expander <- reconstruction low-pass; modelling pieces are the filters (measured, fitted with IIR; eighth-order combined), compander (NE570/571, ratio 2, tau = 10000*C_rect, C typically 0.22-1 uF), variable-clock delay line with resampling/aliasing, noise, insertion gain, nonlinearity. Typical practice is a third-order Sallen-Key anti-alias filter and a third-order plus second-order "corner correction" reconstruction filter. [literature: http://colinraffel.com/publications/dafx2010practical.pdf]
- Filter cutoff typically chosen between 1/3 and 1/2 of the clock (sampling) frequency: the PDF text extraction renders it as "between 31 and 12 of the sampling frequency", which I read as 1/3 and 1/2 [literature: http://colinraffel.com/publications/dafx2010practical.pdf]; echo circuits can go as low as 1.5 kHz (same source). Nyquist of the BBD = f_clock/2 (same source).
- BBD insertion gain 0 to 2 dB at low frequency falling to -4 to -6 dB at Nyquist; SNR about 60 dB near max input; unavoidable THD about 1% per 1024 stages: THD = 1.01^(N/1024) - 1 (so 512 stages about 0.5%, 1024 stages about 1%); chorus/flanger/vibrato with <= 1024 stages usually ignore compander and nonlinearity. [literature: http://colinraffel.com/publications/dafx2010practical.pdf] Raffel states companders "are not typically used" for the short chorus/flanger lines (same page). MN3007 datasheet THD itself: **not opened**.
- Holters & Parker DAFx-18: variable-sample-rate BBD model that uses the circuit's own anti-alias/reconstruction filters, transformed with a modified impulse-invariant transform, so no extra interpolation filter is needed; avoids interpolation distortion and filter-response error. Example: Juno-60 chorus, sixth-order in/out filters (1st-order HP + 5th-order LP each; coefficients in their Table 1, not copied), validated with N=256, f_BBD=50 kHz, fs=44.1 kHz, min clock about 26 kHz on the real unit. [literature: https://www.dafx.de/paper-archive/2018/papers/DAFx2018_paper_12.pdf] Cutoff Hz values: in a table I did not extract.
- Clock modulation shape matters: linear-frequency modulation bends the delay because delay ~ 1/f; linear period modulation gives a clean triangular delay; Roland/Boss designs are said to use linear period modulation; examples 40 kHz base spanning 20-80 kHz, 32 kHz +/-18.75 us. [literature: https://electricdruid.net/weird-wiggles-bbd-clock-modulation/] Example in the related article: 40 kHz +/-20 kHz at 2 Hz gives max pitch change about 0.2 octave (about 240 cents), "quite a lot for a chorus". [literature: https://electricdruid.net/investigations-into-what-a-bbd-chorus-unit-really-does/]

## 6. Digital chorus design (Dattorro 1997)

[literature: https://ccrma.stanford.edu/%7Edattorro/EffectDesignPart2.pdf] (JAES vol. 45 no. 10, p. 764-788; table values read from the layout-preserving text extraction)

Table 6, knobs (blend / feedforward / feedback):
- Vibrato 0.0 / 1.0 / 0.0
- Flanger 0.7071 / 0.7071 / -0.7071 (minus sign as printed)
- Industry standard chorus 1.0 / 0.7071 / 0.0
- White chorus 0.7071 / 1.0 / 0.7071
- Doubling 0.7071 / 0.7071 / 0.0
- Echo 1.0 / <1.0 / <1.0 (modulation off)
Max |feedback| 0.9999999. White chorus: feedback tap fixed at the centre of the modulated delay; H(z) = (blend + feedforward*z^-M) / (1 + feedback*z^-M) is allpass when blend = feedback and feedforward = 1. Never feed back the modulated signal (pitch change compounds).
Table 7, approximate delay range (ms), onset / nominal / end (extraction scrambles columns; reliable rows): flange 0 / 1 / 10; chorus 1 / 5 / 30; vibrato 0 / minimal / 5; doubling and echo rows (about 10-100 and 50-100) uncertain. White-chorus example: tap centre 400 samples at 44.1 kHz (9.1 ms), width about 350 samples (7.9 ms) peak excursion, rate about 0.15 Hz. Strong flange zone is roughly the first 1 ms of delay. Doubling when single circuit tap centre reaches about 20 ms.
Stereo: two circuits with quadrature (90 degree) LFO, one per channel, creates a dynamic stereo field via the Haas cue; keep a switch for in-phase and antiphase modulation; flanger uses same-phase modulation in both channels.
Interpolation: linear interpolation is a time-varying low-pass; multi-voice (more than two) chorus with linear interpolation gives "significantly audible" low-pass filtering; "white" chorus = negative feedback + allpass interpolation. Allpass interpolation (v[n] = v[n-1]-recursion form given in Fig. 23: v[n] = VoiceL[i+1] + (1-frac)*VoiceL[i] - (1-frac)*v[n-1]) minimises amplitude distortion but its recursive transient distortion limits the useful pitch-change range to about +/-1 semitone; must keep it from Nyquist ringing at frac=0 by forcing a constant fractional offset (e.g. 1/256 sample).
Pitch change ratio = 1 - CHORUS_WIDTH * Omega * T * cos(Omega n T); extrema = 1 - CHORUS_WIDTH*Omega*T (CHORUS_WIDTH in samples, Omega = 2*pi*f_LFO).

Smith (PASP): chorus = several time-varying delay lines giving delay + frequency shift; efficient implementation = multiple interpolating taps on one delay line oscillating about fixed tap positions, modulation frequency sets the Doppler frequency shift, each tap panned separately. [literature: https://ccrma.stanford.edu/~jos/pasp/Chorus_Effect.html] Flanger y(n) = x(n) + g x[n - M(n)], LFO triangular or sinusoidal, needs an interpolated delay line. [literature: https://ccrma.stanford.edu/~jos/pasp/Flanging.html] Delay-line interpolation overview: linear is cheap and fine when bandwidth << fs/2; allpass has no gain distortion. [literature: https://ccrma.stanford.edu/~jos/pasp/Delay_Line_Interpolation.html]

## 7. Fractional-delay interpolation quality

- Linear interpolation (Smith): y(n+eta) = (1-eta) y(n) + eta y(n+1); a one-zero FIR; sounds best when oversampled. [literature: https://ccrma.stanford.edu/~jos/pasp/Linear_Interpolation.html, https://ccrma.stanford.edu/~jos/pasp/Fractional_Delay_Filtering_Linear.html] Smith's pages I opened did not print the cos formula; Dattorro states the middle polyphase filter is (1+z^-1)/2, the "averaging" case where "filtering is at its worst, the delay is ideal" (half sample). [literature: https://ccrma.stanford.edu/%7Edattorro/EffectDesignPart2.pdf]
- Magnitude at eta = 0.5: |(1+e^{-jw})/2| = cos(pi f / fs) [derived]. At fs = 48 kHz, eta = 0.5: 5 kHz -0.47 dB, 8 kHz -1.25 dB, **10 kHz -2.01 dB**, 15 kHz -5.1 dB; eta = 0.25: 10 kHz -1.41 dB [derived, computed].
- 3rd-order (4-point) Lagrange, eta = 0.5, fs = 48 kHz [derived]: 5 kHz -0.04, 10 kHz -0.53, 15 kHz -2.53 dB; eta = 0.25: 10 kHz -0.37 dB. 4-point Hermite (= Catmull-Rom, Niemitalo) eta = 0.5: identical to Lagrange at the half-sample point (-0.53 dB at 10 kHz); eta = 0.25: 10 kHz -0.28 dB, 15 kHz -1.17 dB [derived]. Niemitalo: 4-point 3rd-order Hermite "is also known as the Catmull-Rom spline"; Lagrange has no continuous first derivative; with no oversampling the highest passband frequencies are strongly attenuated by polynomial interpolators. [literature: https://yehar.com/blog/wp-content/uploads/2009/08/deip.pdf]
- Smith: first-order allpass interpolation H(z) = (eta + z^-1)/(1 + eta z^-1), DC delay Delta ~ (1-eta)/(1+eta), eta ~ (1-Delta)/(1+Delta); best used for delay in [0.1, 1.1] (avoid pole-zero cancellation at zero delay); not suitable for random access; unit gain at all frequencies. [literature: https://ccrma.stanford.edu/~jos/pasp/First_Order_Allpass_Interpolation.html] Thiran allpass of order N: maximally flat group delay at DC, a_k = (-1)^k C(N,k) prod_{n=0..N} (Delta-N+n)/(Delta-N+k+n); rule of thumb Delta ~ N for stability. [literature: https://ccrma.stanford.edu/~jos/pasp/Thiran_Allpass_Interpolators.html] Even-order Lagrange: use delay in (N/2-1/2, N/2+1/2), no gain zero at fs/2 for the middle delay. [literature: https://ccrma.stanford.edu/~jos/pasp/Even_Order_Lagrange_Interpolation_Summary.html]
- Laakso et al. "Splitting the unit delay" (IEEE SPM 13(1), 1996, doi 10.1109/79.482137): I could not open the paper; only citation confirmed through a search snippet.
- Guidance for chorus: Dattorro (above) wants allpass for transparency but limits modulation to about +/-1 semitone, which chorus satisfies; for multi-voice, linear's low-pass accumulates. A 4-point Lagrange/Hermite keeps 10 kHz within about -0.5 dB vs -2 dB for linear at the worst fractional position [derived], with no recursive transients.

## 8. Pitch deviation of a chorus

- d(t) = d0 + D sin(2 pi f t) gives pitch ratio 1 - d'(t) = 1 - 2 pi f D cos(2 pi f t), peak deviation 2 pi f D [derived, matches Dattorro's expression: pitch change ratio = 1 - CHORUS_WIDTH*Omega*T*cos(Omega n T)] [literature: https://ccrma.stanford.edu/%7Edattorro/EffectDesignPart2.pdf].
- cents_peak = 1200 * log2(1 + 2 pi f D) ~ 1731 * f * D (D in s) [derived]. Examples [derived]: Dimension D mode 1 5.4 cents, mode 2 6.8, mode 3/4 8.2 (from Anwander's numbers above); Dattorro white chorus example (0.15 Hz, 7.9 ms) = 2*pi*0.15*0.0079 = 0.0074 -> 12.9 cents; a CE-2-like 3.5 Hz with +/-1.1 ms (from snippet-only figures) = about 43 cents; Electric Druid example 40 kHz +/-20 kHz at 2 Hz = about 240 cents ("quite a lot for a chorus"). [literature: https://electricdruid.net/investigations-into-what-a-bbd-chorus-unit-really-does/]
- Perceptual: Martens & Marui, DAFx-06, 25 listeners mapped three zones (too subtle, most useful, too extreme) in rate x depth for vibrato, flange and stereo chorus, with common practice about 2-9 Hz; I did not get the numeric zone boundaries (full PDF not opened). [literature: https://dafx.de/paper-archive/details/o5E1oJMJjW58SzTgLy5Bsg] The "+/-5 to 20 cents" chorus guidance and "constant rate*depth product": [unverified memory]; the numbers above (5-13 cents for Dimension/white chorus at 0.15-0.5 Hz) are consistent with it.
- A forum claim that vocal vibrato is 20-50 cents: [snippet only].

## 9. Mono compatibility

No source states these; all [derived].
- L = x + w, R = x - w with w the wet (delay-modulated) path: mono sum (L+R)/2 = x, wet cancels exactly; chorus vanishes in mono. This is the "Dimension" mono behaviour only if the wet is polarity-inverted. In Roland's standard scheme the inversion is of the *LFO* (delay modulation), not of wet polarity, so L = x + w(t), R = x + w(-t): mono sum = x + (w(t)+w(-t))/2, i.e. still a comb/chorus but with doubled-rate-averaged modulation; no cancellation. [Anwander describes "inverted modulation on one line" and the dry mixed 1:1 into both: http://www.florian-anwander.de/roland_string_choruses/]
- Dimension D, per Anwander: each output = own wet + own dry + other channel's dry, so the mono sum keeps all dry and both wets; cancellation depends on wet polarity, which I could not confirm.
- Quadrature dual-voice (Dattorro: one circuit per channel, 90 degree LFO offset): mono sum has both voices with moving comb notches at different phases, so partial comb filtering rather than cancellation. [literature: https://ccrma.stanford.edu/%7Edattorro/EffectDesignPart2.pdf for the quadrature scheme; the mono consequence is derived] Dattorro also recommends an output stereo-field/pan control and an in-phase/antiphase switch.

## Design constants table

| Unit | BBD stages | Delay min/max ms | LFO shape | LFO rate Hz | Depth | Filter cutoffs | Dry:wet | Stereo scheme |
|---|---|---|---|---|---|---|---|---|
| CE-1 | 512 (MN3002) [djjondent] | 1.3-4 [electricdruid weird-wiggles]; schematic clock 5-16 us gives 1.28-4.10 ms [derived, tonehome PDF] | chorus triangle, vibrato sine-like [tonehome PDF scope strip] | not found | not found (delay span 1.3-4 ms) | multi-RC LPF each side, cutoffs not computed/not found | not found | two outputs, assignment not traced |
| CE-2 | 1024 (MN3007) [snippet only] | about 8.5-10.75 total [comment on electricdruid investigations]; chip 5.12-51.2 [madbean] | triangle [snippet only] | 3.5 max [snippet only] | not found | pre-BBD about 5.5 kHz suggested [madbean forum opinion] | not found | mono |
| Dimension D (SDD-320) | 2 x 1024 (MN3007) [anwander] | 10 +/-2.0 (m1); 7.5 +/-2.5 (m2); 7.5 +/-1.5 (m3, m4) [anwander] | triangle-like (trapezoid claim snippet only) | 0.25, 0.25, 0.50, 0.50 [anwander] | as delay column | ~80 Hz HP input [anwander]; LPFs not found | own wet + own dry + other dry; m4 same-side pad -10 dB [anwander] | stereo, antiphase mod |
| Dimension C (DC-2) | 2 BBD (BL3102 per builder) [madbean 221127] | not found | not found | not found | not found | not found | not found | not found |
| Juno-60 | MN3009, 256 implied [eventide, derived] | chip 0.64-12.8; actual range not found [eventide] | triangle (I, II), sine-like (I+II) [anwander] | I 0.5; II 0.8; I+II 1 (depth 8%) [anwander] | 100%, 100%, 8% [anwander] | 5th-order LP + 1st-order HP, in and out [holters]; min clock about 26 kHz [holters] | dry 1:1 into both outputs [anwander] | one LFO, inverted on second line [anwander] |
| Juno-6 | not found | not found | triangle | I 0.4; II 0.6; I+II about 8 [anwander] | 100%, 100%, 25% | not found | same | same |
| Tri-chorus (CS-5) | 3 BBD [snippet only] | not found | not found | not found | per-channel depth [snippet only] | not found | not found | L/C/R lines |
| String ensemble (Roland RS-202) | not found | not found | tri/sine pairs | 6.25 and 0.66 [anwander] | not found | not found | not found | not found |
| String ensemble (ARP Omni/Solina) | 3 BBD, 512? guess [kvraudio] | not found | not found | two LFOs, no numbers [kvraudio] | not found | not found | no dry [kvraudio] | not found |
