/* DRUM ROLL store: patterns and settings as text rows for TE's op1.db (one row per pattern, one for
 * the settings). Pure C, no TE calls; host-tested by test_dr_store.c.
 *
 * A row is base64 of a blob: 'D' 'R' version id, then the payload (a dr_pattern, or dr_settings).
 * Base64 has no quote and no '%', so a row goes into SQL text as is (TE formats SQL printf-style).
 * Not reentrant (one static blob buffer): call from one thread only. */
#ifndef DR_STORE_H
#define DR_STORE_H

#include <stdint.h>
#include "dr_core.h"

#define DR_STORE_VERSION    2              /* 2: 14 finger patterns + owned mask (1: v0.9, A-D) */
#define DR_ROW_SETTINGS     DR_PATTERNS         /* rows 0..13 patterns 1-14, row 14 settings */
#define DR_ROWS             (DR_PATTERNS + 1)
#define DR_HDR              4

typedef struct dr_filt {
    uint8_t type, cut, res;                     /* type 0..filters-1, cut / res 0..127 */
} dr_filt;

typedef struct dr_settings {
    uint8_t cur;                                /* pattern 0..13 */
    uint8_t swing;                              /* 0..100 */
    uint8_t zoom;                               /* 0..4: 1 2 4 3 3.5 bars */
    uint8_t pad;
    uint8_t owned[2];                           /* bit p: pattern p is DRUM ROLL's (else TE's) */
    dr_filt filt[DR_KEYS];
} dr_settings;

#define DR_BLOB_MAX         (DR_HDR + sizeof(dr_pattern))
#define DR_B64_LEN(n)       (((n) + 2) / 3 * 4)
#define DR_ROW_TEXT_MAX     (DR_B64_LEN(DR_BLOB_MAX) + 1)

int dr_b64_enc(const uint8_t *in, int n, char *out);           /* returns the length, out 0-ended */
int dr_b64_dec(const char *in, uint8_t *out, int max);         /* bytes, -1 invalid or too long */

/* row text for a pattern / the settings (out: DR_ROW_TEXT_MAX); returns the length */
int dr_store_pattern(int id, const dr_pattern *p, char *out);
int dr_store_settings(const dr_settings *s, char *out);

/* parse a row: 0 ok (out filled, every field in range), -1 not ours / damaged (out untouched) */
int dr_load_pattern(int id, const char *text, dr_pattern *out);
int dr_load_settings(const char *text, int filters, dr_settings *out);

uint32_t dr_hash(const void *p, int n);       /* change detection between saves */

#endif
