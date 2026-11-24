// A file inside a shader library image, <DM_MSL_DIR>/b-<chain>.img: a flat RomFS-format folder, read in place, mapping only the files a run loads.
#pragma once
#include "../../host/romfs_image.h"
#include <sys/mman.h>

// Any thread
static const img_ent *msl_image_find(const romfs_img *m, const char *name) {
    size_t len = strlen(name);
    uint64_t lo = m->e[0].a, hi = lo + m->e[0].b;
    while (lo < hi) {
        uint64_t mid = lo + (hi - lo) / 2;
        const img_ent *x = &m->e[mid];
        int c = memcmp(m->names + x->name, name, x->nlen < len ? x->nlen : len);
        if (!c) c = x->nlen < len ? -1 : x->nlen > len;
        if (!c) return x->type ? x : NULL;
        if (c < 0) lo = mid + 1; else hi = mid;
    }
    return NULL;
}

// 0 on failure, errno set (an empty file: EINVAL); base / base_len are munmap's. MAP_SHARED: an image never changes in place (replaced by rename)
typedef struct { const void *data; size_t size; void *base; size_t base_len; } msl_map;
static int msl_image_map(const romfs_img *m, const img_ent *x, msl_map *out) {
    if (!x->b) { errno = EINVAL; return 0; }
    uint64_t delta = x->a % (uint64_t)getpagesize();
    void *p = mmap(NULL, (size_t)(x->b + delta), PROT_READ, MAP_SHARED, m->fd, (off_t)(x->a - delta));
    if (p == MAP_FAILED) return 0;
    out->base = p; out->base_len = (size_t)(x->b + delta); out->data = (const char *)p + delta; out->size = (size_t)x->b;
    return 1;
}
