/* TE R246 addresses only the WHEEL feature uses (drwave engine as host). Mapped from the R246
 * disassembly; see README.md. */
#ifndef WH_TE246_H
#define WH_TE246_H

#include <stdint.h>

/* voice render contract and drwave hooks: features/enginehost (eh_host.h, enginehost.c) */

/* TE's knob ranges for drwave (Ranges table 0x01de5890, read by running TE's init 0x01a1e914 in
 * the simulator): slot 1 0..24575, 2 0..16383, 3 0..16383, 4 0..32767, 5-8 0..32767. Every
 * stored value must stay inside them (boot-time patch validator). */
#define WH_TE_PAGE1_MAX      16383

/* page 3 (the sound's FX knobs): features/enginehost/eh_ui.c */

#endif
