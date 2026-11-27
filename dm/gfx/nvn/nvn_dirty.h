// Write-protect dirty-page tracking for guest memory
#pragma once
#include <stddef.h>
#include <stdint.h>

void nvn_dirty_add_range(const void *mem, size_t size);
void nvn_dirty_stat(int *live, int *peak, int *cap);
void nvn_dirty_remove_range(const void *mem);
void nvn_dirty_open(const void *addr, size_t len);                 // pages writable, uncounted and unprotected until nvn_dirty_close (a zero-copy buffer the GPU writes through)
void nvn_dirty_close(void);                                        // pages changed meanwhile count as written
void nvn_dirty_open_stats(uint64_t *opened, uint64_t *changed);
int nvn_dirty_tracked(const void *addr, size_t len);
void nvn_dirty_protect(const void *addr, size_t len);
void nvn_dirty_protect_sparse(const void *addr, size_t len);
uint64_t nvn_dirty_sparse_writes(void);
void nvn_dirty_own_thread(void);                                   // the calling thread replays submissions
uint64_t nvn_dirty_own_writes(void);
// While a replay_ahead hold is open, count the guest pages a write unprotects (between the parked submissions and the game's signal)
void nvn_dirty_ahead_watch(int on);
void nvn_dirty_ahead_stats(uint64_t *pages, uint64_t *bytes, uint64_t *holds);
void nvn_dirty_gpu_written(uintptr_t addr, size_t len);             // GPU writes bypass page protection
void nvn_dirty_touch(void);                                        // moves the generation, no page written: callers that kept "nothing written since" ask again
uint64_t nvn_dirty_now(void);                                      // take the stamp before nvn_dirty_protect and before reading the content
int nvn_dirty_since(const void *addr, size_t len, uint64_t stamp);
const void *nvn_dirty_span(const void *addr, size_t len);          // NULL = untracked
int nvn_dirty_since_cached(const void **span, const void *addr, size_t len, uint64_t stamp);
int nvn_dirty_since_span(const void *span, const void *addr, size_t len, uint64_t stamp);
// Pages written after stamp outside ex[0..nex) (host writes the caller accounts for); the first in *first; SIZE_MAX when untracked
size_t nvn_dirty_since_except(const void *span, const void *addr, size_t len, uint64_t stamp, const uintptr_t (*ex)[2], int nex, uintptr_t *first);
void nvn_dirty_stats(uint64_t *faults, uint64_t *preps);
void nvn_dirty_mp_stats(uint64_t *calls, uint64_t *pages, uint64_t *ns);
void nvn_dirty_describe(const void *addr, const void *span, char *out, size_t n);
