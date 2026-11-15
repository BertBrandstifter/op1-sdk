/* op1-sdk library "fat" (libs: ["fat"]): read-only FAT16 / FAT32 volume reader.
 *
 * Disk mode's volume is one file on yaffs (user/tape_c.raw); this reads files inside it through a
 * caller-supplied "read n bytes at offset" function, so the same code runs on the OP-1 (yaffs
 * pread) and in host tests (a memory image). No TE calls, no allocation, every loop bounded,
 * every cluster number range-checked: a corrupt volume gives an error code, never a hang.
 * Short names only (long-name entries are skipped). Thread context (file reads), never audio.
 * On the OP-1 every buffer handed to a yaffs read / write must start at an even address (TE's NAND
 * driver asserts te_yflash2.c:600 "!((unsigned int)data & 1)"): this library's own buffers are
 * aligned; a caller's buffer for fat_read must be too. */
#ifndef OP1_FAT_H
#define OP1_FAT_H

#include <stdint.h>

/* read n bytes at byte offset off into buf; returns n on success, anything else = error */
typedef int (*fat_read_fn)(void *ctx, uint32_t off, void *buf, uint32_t n);

enum {                                  /* fat_mount / fat_find / fat_read errors (negative) */
    FAT_OK = 0,
    FAT_EIO = -1,                        /* the read function failed */
    FAT_ENOFS = -2,                      /* no FAT boot sector (or MBR partition with one) */
    FAT_EFAT12 = -3,                     /* FAT12: not supported */
    FAT_EGEOM = -4,                      /* boot sector numbers out of range */
    FAT_ENOENT = -5,                     /* name not found */
    FAT_ECHAIN = -6,                     /* cluster chain broken / loops / too short */
};

typedef struct fat_vol {
    fat_read_fn read;
    void *ctx;
    uint8_t type;                       /* 16 or 32 */
    uint8_t spc;                        /* sectors per cluster */
    uint16_t bps;                       /* bytes per sector */
    uint32_t base;                      /* byte offset of the volume (partition) */
    uint32_t fat_off;                   /* byte offset of FAT #1 */
    uint32_t root_off, root_bytes;      /* FAT16 fixed root directory */
    uint32_t root_clus;                 /* FAT32 root directory cluster */
    uint32_t data_off;                  /* byte offset of cluster 2 */
    uint32_t clus_bytes;
    uint32_t nclus;                     /* data clusters: valid numbers are 2 .. nclus + 1 */
    uint32_t fat_cache_at;              /* byte offset of the cached FAT block, ~0 = none */
    uint8_t fat_cache[512] __attribute__((aligned(4)));  /* read through yaffs: even address */
} fat_vol;

typedef struct fat_ent {                 /* one directory entry (short name) */
    char name[13];                      /* "A1.WAV", NUL-terminated, as stored (upper case) */
    uint8_t attr;                       /* 0x10 directory */
    uint32_t clus;                      /* first cluster (0 = empty file) */
    uint32_t size;                      /* bytes */
} fat_ent;

typedef struct fat_file {                /* a file being read: a cursor that makes sequential reads cheap */
    fat_ent e;
    uint32_t at_idx;                    /* cluster index of at_clus within the file */
    uint32_t at_clus;
} fat_file;

int fat_mount(fat_vol *v, fat_read_fn read, void *ctx);

/* next cluster of the chain; 0 = end of chain, 1 = bad / out of range */
uint32_t fat_next(fat_vol *v, uint32_t clus);

/* list directory dir (cluster, 0 = root): calls back for every used short entry (no volume
 * labels, no "." / "..", no deleted entries, LFN parts skipped). Returns the entry count or < 0. */
typedef void (*fat_list_fn)(void *ctx, const fat_ent *e);
int fat_list(fat_vol *v, uint32_t dir, fat_list_fn fn, void *ctx);

/* every 32-byte slot of dir up to the end marker, deleted / long-name / label ones too (debug);
 * returns the slot count or < 0 */
typedef void (*fat_raw_fn)(void *ctx, const uint8_t *slot);
int fat_dir_raw(fat_vol *v, uint32_t dir, fat_raw_fn fn, void *ctx);

/* find name (case-insensitive short name "PLAY", "A1.WAV") in dir; 0 or FAT_ENOENT / error */
int fat_find(fat_vol *v, uint32_t dir, const char *name, fat_ent *out);

/* the chain of e has exactly the clusters its size needs (no loop, not cut short, no junk) */
int fat_check(fat_vol *v, const fat_ent *e);

void fat_open(fat_file *f, const fat_ent *e);

/* read n bytes at off of the file (clamped to its size); contiguous clusters are read with one
 * call. Returns the bytes read (0 at / past the end) or < 0. */
int fat_read(fat_vol *v, fat_file *f, uint32_t off, void *buf, uint32_t n);


#endif
