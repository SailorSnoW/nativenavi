// Metal layer of the NVN device: pools are zero-copy MTLBuffers, submits command buffers, syncs MTLSharedEvents.
// Every effect of GPU completion (syncs, event values, counters) happens only once the command buffer has completed.
#pragma once
#include <stdint.h>
#include <stddef.h>

void mtl_init(void);
void mtl_vbuf_stats(uint64_t *resolve, uint64_t *reuse, uint64_t *set, uint64_t *skip, uint64_t *copies, uint64_t *copy_bytes);
void mtl_set_depth_zero(int on);          // 1 = near is 0 (D3D / Metal), 0 = near is -w (GL, NVN's default)
void mtl_set_origin_lower_left(int on);   // 1 = lower left (GL, NVN's default), 0 = upper left
void mtl_set_queue_origin_lower_left(int on); // read under the submit lock

// expanded to page boundaries; GPU address = CPU address
void mtl_pool_add(const void *mem, uint64_t size);
void mtl_pool_addressed(const void *mem, uint64_t size);   // raw global memory may reach it
void mtl_pool_del(const void *mem);
void mtl_pool_replace(const void *const *del, int ndel, const void *const *add, const uint64_t *size, int nadd);   // all at once for every lookup
void dm_gfx_addr_note(uint64_t a, char *note, size_t n);
void mtl_alias_drop_pool(const void *pool);
// its shared sparse tiles go once no texture maps them
void mtl_sp_phys_gone(const void *mem, uint64_t size);
int mtl_addr(uint64_t addr, uint64_t len, void **buf, uint64_t *off);   // buf: a (__bridge) id<MTLBuffer>
int mtl_addr_known(uint64_t addr, uint64_t len);   // does not make the pool resident

void mtl_prepare(void);                                // no GPU work; called outside the graphics gate
void mtl_sub_begin(void);
void mtl_sub_ready(uint64_t guest_ns);   // guest ns the submission was ready to run (counter timestamps)
void mtl_queue_activate(uint64_t serial);              // submit lock held
void mtl_queue_forget(uint64_t serial);
void mtl_sub_copy(uint64_t src, uint64_t dst, uint64_t n);
void mtl_sub_event(volatile uint32_t *val, uint32_t mode, uint32_t v);   // mode: 0 write, 1 add
void mtl_sub_counter(uint64_t addr, uint32_t type);
uint64_t mtl_sub_commit(void);

uint64_t mtl_completed_fence(void);
int mtl_replay_ahead(void);
int mtl_hold_begin(volatile uint32_t *word, unsigned mode, uint32_t value);   // submit thread, between submissions: 1 when later work is held until the wait is satisfied
int mtl_hold_active(void);
void mtl_hold_signal(volatile uint32_t *word, uint32_t value);   // any thread
void mtl_hold_force(volatile uint32_t *word);        // any thread: a hold on it cannot be satisfied
void mtl_hold_drain(void);                           // any thread: a hold ends now
void mtl_hold_wait(void);                            // submit thread
void mtl_hold_satisfied(void);                       // any thread
uint64_t mtl_last_fence(void);                       // 0 = never submitted
enum { POOL_STATS = 14 };
void mtl_pool_stats(uint64_t s[POOL_STATS]);
int mtl_wait(uint64_t fence, uint64_t timeout_ns);   // 0 on timeout
int mtl_wait_acq(uint64_t fence, uint64_t timeout_ns);
void mtl_finish(void);
void mtl_drain(void);                                // including presents
void mtl_arch_save(int wait_ms, int ending);         // writes even during the prewarm; ending: the process ends after up to wait_ms
enum { MTL_QUIT_ARCH_MS = 10000 };   // atexit's wait for the archive write; dm.h DM_QUIT_ARCH_MS is the same
void mtl_warm_stop(void);   // the process is ending: the prewarm builds nothing more
void mtl_present_wait(uint64_t ticket);
void mtl_present_wait_texture(uint64_t ticket, uint64_t copy);   // with a snapshot, its copy has run on the GPU; else as mtl_present_wait
