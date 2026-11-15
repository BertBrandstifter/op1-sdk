/* yaffs direct API (R246) used by PLAY: pread = yaffsfs_do_read(fd, buf, n, 1, offset); the
 * directory walk and stat as features/diskinfo uses them. Thread context only (yaffs locks). */
#ifndef PY_YAFFS_H
#define PY_YAFFS_H

#define PY_PREAD        ((int (*)(int fd, void *buf, uint32_t n, int32_t off))0x01a99ca8)
#define PY_OPENDIR      ((void *(*)(const char *path))0x01a9b450)
#define PY_READDIR      ((uint8_t *(*)(void *dir))0x01a9b648)   /* dirent, d_name at +11 */
#define PY_CLOSEDIR     ((int (*)(void *dir))0x01a9b74c)
#define PY_STAT         ((int (*)(const char *path, void *st))0x01a9a69c)   /* mode +0x08, size +0x1c */
/* PY_UNLINK, PY_MKDIR, PY_RENAME: from the disk-sync RE (py_te.c notes) */
#include "py_yaffs_re.h"

#endif
