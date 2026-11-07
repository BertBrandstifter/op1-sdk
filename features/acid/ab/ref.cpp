// A/B reference: Open303 (MIT) renders a note sequence to raw float at 44.1 kHz.
#include "rosic_Open303.h"
#include <cstdio>
#include <cstdlib>
using namespace rosic;
int main(int argc, char **argv) {
  // args: cutoffHz res% envmod% decayMs accent% wave(0/1) notes...(key:vel:ms:legato)
  Open303 s; s.setSampleRate(44100);
  s.setCutoff(atof(argv[1])); s.setResonance(atof(argv[2])); s.setEnvMod(atof(argv[3]));
  s.setDecay(atof(argv[4])); s.setAccent(atof(argv[5])); s.setWaveform(atof(argv[6])); s.setVolume(0);
  int prev = -1;
  for (int a = 7; a < argc; a++) {
    int key, vel, ms, leg; sscanf(argv[a], "%d:%d:%d:%d", &key, &vel, &ms, &leg);
    if (prev >= 0 && !leg) s.noteOn(prev, 0, 0.0);
    s.noteOn(key, vel, 0.0);
    if (prev >= 0 && leg) s.noteOn(prev, 0, 0.0);
    prev = key;
    int n = ms * 44100 / 1000;
    for (int i = 0; i < n; i++) { float x = (float)s.getSample(); fwrite(&x, 4, 1, stdout); }
  }
  return 0;
}
