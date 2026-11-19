/* py_export: the renders copied into disk mode's volume (README "Render"), pure C, host-tested
 * (test_py_export.c), no TE calls.
 *
 * When disk mode is entered, every file of the render folder is copied into the volume's play_r/
 * folder, so the computer sees it. TE deletes the folder again when disk mode is left. A file is
 * copied only if it fits with room to spare on both sides: the volume keeps TE's own reserve, and
 * yaffs (the flash the volume lives in, which frees written-over space lazily) keeps a margin;
 * a file that does not fit is skipped, the next ones are still tried. A short write (the volume
 * full after all) removes the half-written file and stops. */
#ifndef PY_EXPORT_H
#define PY_EXPORT_H

#include <stdint.h>

#define PEX_FAT_RESERVE     2000000u            /* TE skips its own exports below this */
#define PEX_FLASH_RESERVE   (16u << 20)         /* yaffs: what op1.db and the rest need */

typedef struct pex_io {
    void *ctx;
    uint32_t cluster;                           /* the volume's cluster bytes */
    void *buf;                                  /* copy buffer (4-byte aligned) */
    uint32_t chunk;                             /* its bytes */
    /* the next render file: 1 (name, size), 0 = no more */
    int (*next)(void *ctx, char name[13], uint32_t *size);
    int (*read)(void *ctx, const char *name, uint32_t off, void *buf, uint32_t n);  /* bytes read */
    int (*create)(void *ctx, const char *name);                 /* 0 = open for writing */
    int (*write)(void *ctx, const void *buf, uint32_t n);       /* bytes written */
    int (*close)(void *ctx);                                    /* 0 = ok */
    void (*remove)(void *ctx, const char *name);
    uint32_t (*fat_free)(void *ctx);            /* the volume's free bytes */
    uint32_t (*flash_free)(void *ctx);          /* yaffs' free bytes */
} pex_io;

typedef struct pex_result {
    uint16_t copied, skipped, failed;
    uint32_t bytes;                             /* copied */
} pex_result;

void py_export_run(pex_io *io, pex_result *res);

#endif
