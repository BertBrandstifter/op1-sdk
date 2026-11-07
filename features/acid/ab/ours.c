/* A/B: ACID's ac_core renders the same note list as ref.cpp, raw int16 -> float at 88.2 kHz */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "ac_core.h"
int main(int argc, char **argv) {
  ac_params p; ac_voice v; uint32_t inc[128]; int16_t o[128];
  double fc = atof(argv[1]), res = atof(argv[2]), em = atof(argv[3]), dec = atof(argv[4]), acc = atof(argv[5]);
  int a, i, prevheld = 0;
  memset(&p, 0, sizeof p);
  p.cutoff = (int)lround(255 * log(fc / 314.0) / log(2394.0 / 314.0));
  p.res = (int)lround(255 * res / 100); p.envmod = (int)lround(255 * em / 100);
  p.decay = (int)lround(255 * log(dec / 30.0) / log(100.0));
  p.accent = (uint8_t)lround(63 * acc / 100); p.wave = (uint8_t)atoi(argv[6]);
  p.accdec = (uint8_t)lround(31 * log(200 / 30.0) / log(100.0)); p.soft = (uint8_t)lround(31 * log(3 / 0.3) / log(100.0));
  p.slide = (uint8_t)lround(31 * log(60 / 10.0) / log(36.0)); p.sweep = AC_SWEEP_NORM; p.speed = 1; p.level = 10;
  ac_init(&v);
  for (a = 7; a < argc; a++) {
    int key, vel, ms, leg, b;
    sscanf(argv[a], "%d:%d:%d:%d", &key, &vel, &ms, &leg);
    if (prevheld && !leg) ac_release(&v);
    ac_note(&v, prevheld && leg, vel >= 100);
    prevheld = 1;
    for (i = 0; i < 128; i++) inc[i] = (uint32_t)(440.0 * pow(2, (key - 69) / 12.0) / 88200 * 4294967296.0);
    for (b = 0; b < ms * 882 / 1280; b++) {
      ac_render(&v, o, 128, inc, &p);
      for (i = 0; i < 128; i++) { float x = o[i] / 32768.0f; fwrite(&x, 4, 1, stdout); }
    }
  }
  return 0;
}
