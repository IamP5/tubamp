# Literature and practice: what was opened, and what it says

Only pages that were fetched and read are listed as sources. Items seen only as a search-result snippet
are marked so and are not used as evidence in the report.

## Opened

| # | URL | fact taken |
| --- | --- | --- |
| L1 | https://raw.githubusercontent.com/sdatkinson/AudioDSPTools/main/dsp/ImpulseResponse.h | `const size_t mMaxLength = 8192;` class comment: cubic resampling, 8192-tap limit, gain law `pow(10.0, -18.0 / 20.0) * 48000 / sampleRate` |
| L2 | https://raw.githubusercontent.com/sdatkinson/AudioDSPTools/main/dsp/ImpulseResponse.cpp | `dsp::ResampleCubic<float>(padded, mRawAudioSampleRate, mSampleRate, 0.0, resampled);` `const float gain = pow(10, -18 * 0.05) * 48000 / mSampleRate;` output is an Eigen dot product of the weights with the input history: a direct time-domain FIR; a second channel is loaded when the file has two |
| L3 | https://www.dafx.de/paper-archive/2011/Papers/74_e.pdf (Wefers and Vorlaender, DAFx-11) | a direct-form FIR "has no inherent latency" but is O(N^2); the usual choice is a transform size of twice the block length; "it is always beneficial to partition filters"; an optimised FFT size gave a speed-up of about 1.3 over the standard choice at B = 128 |
| L4 | https://ccrma.stanford.edu/~dtyeh/cabinet08/dafx08cabinet.pdf (Yeh, Bank, Karjalainen, DAFx-08, slides) | Celestion G12 Vintage 30 in an Engl 12-inch cabinet; "Distortion falls rapidly with frequency"; distortion modelled "only at low frequencies where it is most significant", as a static 5th-order polynomial on cone displacement between linear filters; "Most salient effects are linear. Nonlinear effects are subtle." |
| L5 | https://celestion.com/?p=10987 | voice-coil DC resistance is "about 75% to 85% of the nominal impedance" (example readings 6.7 and 12.9 ohm); resonance of a guitar speaker roughly 55 Hz to 100 Hz; the resonance peak can reach "60 ohm or more"; impedance rises to 20, 30 or 40+ ohm at the top of the range |
| L6 | https://duncanamps.com/technical/dummyload.html (R. Aiken's reactive load) | resonant peaks can be "as much as 5 times the rated impedance, or even higher"; the load is a series R, an L with a parallel R for the high-frequency rise, and a parallel R-L-C for the resonance at `1/(2*pi*sqrt(L*C))`; the peak is limited by the resistor in the resonant branch; component values are only in the schematic images, not in the text |
| L7 | https://manuals.line6.com/en/helix-stadium/live/cab-blocks | every IR is processed "as a 2048-sample, 48kHz, 32-bit float mono file"; IR block has Low Cut, High Cut, Polarity, Delay, Mix, Level, Pan; cab models have Mic, Position, Distance 1.00..12.00 in, Angle 0 or 45 degrees, and a Delay whose "Auto" setting approximates the air delay of the mic distance; one cab sums the input to mono, two make the block stereo |
| L8 | https://www.avnirvana.com/threads/minimum-phase.4699/ (J. Mulcahy, author of REW) | "The minimum phase response isn't the response of your cab"; it shares only the magnitude; "It is more often useful to remove time delay from the measurement." |

Local source read, not a web page: `build/_deps/juce-src/modules/juce_dsp/frequency/juce_Convolution.cpp`
and `juce_audio_basics/sources/juce_ResamplingAudioSource.cpp` (see report section 2).

## Tried, not readable

- https://wiki.fractalaudio.com/wiki/index.php?title=Cab_block and `...title=Amp_block`: HTTP 403.
- Gardner, "Efficient convolution without input-output delay", JAES 43(3), 1995 (https://www.aes.org/e-lib/browse.cfm?elib=7957): paywalled. Only the abstract-level statement from the search result was seen: a hybrid of direct-form and block FFT processing gives a zero-delay convolver.
- Wefers' thesis "Partitioned convolution algorithms for real-time auralization" (RWTH, 2015): no open copy found; the DAFx-11 paper above (L3) by the same author was read instead.

## Search-result snippets only (not opened, not used as evidence)

- Line 6 forum: Helix cab and IR cuts are said to be 6 dB per octave (https://line6.com/support/topic/57668-questions-about-highlow-cut-blocks).
- IR vendors describe minimum-phase files as giving a shared time and phase reference for blending.
- Fractal Audio forum: negative feedback around the power amp reduces the speaker resonance; guitar speakers are close to minimum phase.
- Forum measurement of two DIY tube amps: output impedance 40 and 50 ohm (music-electronics-forum.com thread 47024).
