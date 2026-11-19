/* py_export: the renders copied into disk mode's volume (py_export.h). */
#include "py_export.h"

#define MAX_FILES   200                         /* bounded: the folder holds 99 renders */

/* one file; 1 = copied, 0 = failed (removed), -1 = the volume is full (removed, stop) */
static int copy(pex_io *io, const char *name, uint32_t size)
{
    uint32_t off = 0, n;
    int r;

    if (io->create(io->ctx, name))
        return 0;
    while (off < size) {
        n = size - off < io->chunk ? size - off : io->chunk;
        r = io->read(io->ctx, name, off, io->buf, n);
        if (r != (int)n) {
            io->close(io->ctx);
            io->remove(io->ctx, name);
            return 0;
        }
        if (io->write(io->ctx, io->buf, n) != (int)n) {
            io->close(io->ctx);
            io->remove(io->ctx, name);
            return -1;
        }
        off += n;
    }
    if (io->close(io->ctx)) {
        io->remove(io->ctx, name);
        return 0;
    }
    return 1;
}

void py_export_run(pex_io *io, pex_result *res)
{
    char name[13];
    uint32_t size, need, clus = io->cluster ? io->cluster : 1;
    int i, r, first = 1;

    res->copied = res->skipped = res->failed = 0;
    res->bytes = 0;
    for (i = 0; i < MAX_FILES && io->next(io->ctx, name, &size); i++) {
        need = (size + clus - 1) / clus * clus + (first ? clus : 0);   /* (the folder's own cluster) */
        if (io->fat_free(io->ctx) < need + PEX_FAT_RESERVE
            || io->flash_free(io->ctx) < size + PEX_FLASH_RESERVE) {
            res->skipped++;
            continue;
        }
        first = 0;
        r = copy(io, name, size);
        if (r > 0) {
            res->copied++;
            res->bytes += size;
        } else {
            res->failed++;
            if (r < 0)
                break;
        }
    }
}
