/* yaffs direct API calls found for PLAY's import (static RE 2026-11-15, disk-sync notes in
 * README.md): TE creates its /yaffs2/user/audio folders with yaffs_mkdir(path, 0). There is no
 * yaffs_rename in the image. Thread context only. */
#define PY_MKDIR        ((int (*)(const char *path, int mode))0x01a9a886)
#define PY_UNLINK       ((int (*)(const char *path))0x01a9a3fc)
#define PY_RMDIR        ((int (*)(const char *path))0x01a9aad0)
