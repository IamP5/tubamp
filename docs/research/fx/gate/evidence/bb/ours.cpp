// Runs tubamp's shipped gate (AudioDSPTools dsp::noise_gate with the constants in
// src/dsp/FxBlocks.cpp:27-31) over a mono float32 WAV. Usage: ours in.wav out.wav thresholdDb [block]
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>
#include <cstdint>
#include "NoiseGate.h"
int main(int argc, char** argv) {
  if (argc < 4) return 1;
  FILE* f = fopen(argv[1], "rb"); if (!f) return 2;
  std::vector<unsigned char> raw; unsigned char buf[65536]; size_t n;
  while ((n = fread(buf, 1, sizeof buf, f)) > 0) raw.insert(raw.end(), buf, buf + n);
  fclose(f);
  size_t pos = 12, dataPos = 0, dataLen = 0; uint32_t sr = 48000;
  while (pos + 8 <= raw.size()) {
    uint32_t sz; memcpy(&sz, &raw[pos + 4], 4);
    if (!memcmp(&raw[pos], "fmt ", 4)) memcpy(&sr, &raw[pos + 12], 4);
    if (!memcmp(&raw[pos], "data", 4)) { dataPos = pos + 8; dataLen = sz; }
    pos += 8 + sz + (sz & 1);
  }
  size_t frames = dataLen / 4;
  std::vector<float> x(frames); memcpy(x.data(), &raw[dataPos], frames * 4);
  double thr = atof(argv[3]); int block = argc > 4 ? atoi(argv[4]) : 512;
  dsp::noise_gate::Trigger trig; dsp::noise_gate::Gain gain; trig.AddListener(&gain);
  trig.SetSampleRate(sr);
  trig.SetParams(dsp::noise_gate::TriggerParams(0.01, thr, 0.1, 0.005, 0.01, 0.05));
  std::vector<DSP_SAMPLE> in(block); std::vector<float> y(frames);
  for (size_t i = 0; i + block <= frames; i += block) {
    for (int k = 0; k < block; ++k) in[k] = x[i + k];
    DSP_SAMPLE* p[1] = { in.data() };
    DSP_SAMPLE** t = trig.Process(p, 1, block);
    DSP_SAMPLE** o = gain.Process(t, 1, block);
    for (int k = 0; k < block; ++k) y[i + k] = (float) o[0][k];
  }
  memcpy(&raw[dataPos], y.data(), frames * 4);
  f = fopen(argv[2], "wb"); fwrite(raw.data(), 1, raw.size(), f); fclose(f);
  return 0;
}
