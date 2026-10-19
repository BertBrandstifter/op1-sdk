/* NOTE ROLL store: patterns and settings as text rows for TE's op1.db (one row per pattern, one for
 * the settings). Pure C, no TE calls; host-tested by test_nr_store.c.
 *
 * A row is base64 of a blob: 'N' 'R' version id, then the payload (an nr_pattern, or nr_settings).
 * Base64 has no quote and no '%', so a row goes into SQL text as is (TE formats SQL printf-style).
 * Not reentrant (one static blob buffer): call from one thread only. */
#ifndef NR_STORE_H
#define NR_STORE_H

#include <stdint.h>
#include "nr_core.h"

#define NR_STORE_VERSION    1
#define NR_ROW_SETTINGS     NR_PATTERNS         /* rows 0..13 patterns 1-14, row 14 settings */
#define NR_ROWS             (NR_PATTERNS + 1)
#define NR_HDR              4

typedef struct nr_settings {
    uint8_t cur;                                /* pattern 0..13 */
    uint8_t swing;                              /* 0..100 */
    uint8_t zoom;                               /* 0..4: 1 2 4 3 3.5 bars */
    uint8_t pad;
    uint8_t owned[2];                           /* bit p: pattern p is NOTE ROLL's (else TE's) */
    uint8_t pad2[2];
} nr_settings;

#define NR_BLOB_MAX         (NR_HDR + sizeof(nr_pattern))
#define NR_B64_LEN(n)       (((n) + 2) / 3 * 4)
#define NR_ROW_TEXT_MAX     (NR_B64_LEN(NR_BLOB_MAX) + 1)

int nr_b64_enc(const uint8_t *in, int n, char *out);           /* returns the length, out 0-ended */
int nr_b64_dec(const char *in, uint8_t *out, int max);         /* bytes, -1 invalid or too long */

/* row text for a pattern / the settings (out: NR_ROW_TEXT_MAX); returns the length */
int nr_store_pattern(int id, const nr_pattern *p, char *out);
int nr_store_settings(const nr_settings *s, char *out);

/* parse a row: 0 ok (out filled, every field in range), -1 not ours / damaged (out untouched) */
int nr_load_pattern(int id, const char *text, nr_pattern *out);
int nr_load_settings(const char *text, nr_settings *out);

uint32_t nr_hash(const void *p, int n);       /* change detection between saves */

#endif
