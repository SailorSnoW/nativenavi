// The NVN resource owner: what a serial resolves to, the sharing table, held counts, change generation and retirement eligibility (dm/gfx/README.md).
// The replay layer changes its state only through these. Tables are under g_smu (_locked: the caller holds it), program pins under the submission lock.
// Lookups are borrowed: what the owner lets go is released after the submission in flight (own_hook_drop), so it outlives the submission it is encoded in.
// Lock order: g_smu, then g_cpu_tex_mu; g_smu, then the notes' lock.
#pragma once
#import <Foundation/Foundation.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include "nvn_hmap.h"
#include "nvn_obj.h"
@protocol MTLTexture;

#ifdef __clang_analyzer__
void dm_analyzer_map_takes(CFTypeRef CF_CONSUMED ref);   // no body: tells the analyzer the map takes the reference
#endif
static inline id hm_get_id(const nvn_hmap *m, const void *k) { return (__bridge id)nvn_hm_get(m, k); }
static inline void hm_put_id(nvn_hmap *m, const void *k, id o) {   // the map owns the CFBridgingRetain reference until the entry is replaced or removed
    CFTypeRef ref = CFBridgingRetain(o);
#ifdef __clang_analyzer__
    dm_analyzer_map_takes(ref);
#endif
    void *old = nvn_hm_put(m, k, (void *)ref);
    if (old) CFRelease(old);
}
static inline void hm_del_id(nvn_hmap *m, const void *k) {
    void *old = nvn_hm_del(m, k);
    if (old) CFRelease(old);
}

extern pthread_mutex_t g_smu;
enum { NGHOST = 512,             // ghost entries kept once idle
       GHOST_IDLE = 16,          // frames without a use (a game makes many submissions a frame)
       GHOST_IDLE_SUBS = 1024,   // or submissions without one, for stretches with no present
       GHOST_TRIM = 32 };        // at most per commit; each is a note, so under SER_NOTES


const void *own_live_ref(uint64_t k);
const void *own_resolve_ref(uint64_t k);    // live, then ghost (a use of it), then a texture pin
static inline id serial_get(uint64_t k) { return (__bridge id)own_live_ref(k); }
static inline id tex_get(uint64_t k) { return (__bridge id)own_resolve_ref(k); }

// Each takes g_smu
void serial_put(uint64_t k, id o);   // the one it replaces is dropped
void serial_del(uint64_t k);         // a ghost entry of the serial stays: a replayed record may still resolve it
void tex_put(uint64_t k, id o);      // live table for a live texture object or an external texture (2^62 and up), else the ghost table
void own_ghost_trim(void);           // idle ghost entries past NGHOST go, GHOST_TRIM at most

// The sharing table. A key's padding is hashed: build keys zeroed (memset or a zeroed initializer)
typedef struct { uint64_t pool; int64_t off; nvn_texdesc d; } alias_key;
enum { ALIAS_CELL = 16,    // log2 of the index's cell: 64 KB
       ALIAS_IDLE = 16 };  // a takeover spares an entry looked up in the last ALIAS_IDLE submissions
// Each takes g_smu and publishes under serial in the same hold as the lookup
id own_alias_take(uint64_t serial, const alias_key *k);                 // marked looked up; nil (nothing changed) when k has none
id own_alias_publish(uint64_t serial, const alias_key *k, id o);        // o takes over k's bytes (unheld overlapping entries of the pool go) and is published; returns
                                                                        // o, or the texture another thread registered under k first (o is then the caller's to retire)
id own_alias_peek(const alias_key *k);                                  // not marked looked up
// sRGB / linear twin views, decided in the same hold as the entry (docs/reference/nvn-objects.md#a-finalized-twin-view-left-in-the-ghost-ring)
int own_view_put(uint64_t k, const alias_key *pk, id<MTLTexture> view);   // only while its parent is pk's shared texture: 1 published, 0 nothing changed
int own_view_drop_stale(uint64_t k, id view);        // 1: no longer valid, and k's entry goes if it still names the view
// A texture made again with more usage: every name of `from` moves to `to` (a new epoch), then `put` is published under serial. One hold of g_smu.
int own_upgrade(id from, id to, uint64_t serial, id put);
// Shared textures another one drew over (nvn_owner.m says why); stamps in the replay layer's clock, 0 = none
typedef struct { uint32_t use, wr; } own_use;   // last use (read or write), last GPU write
typedef int (*own_use_fn)(id o, own_use *u);    // 0: a texture the release does not cover
typedef struct { uint32_t n, serials, pinned; uint64_t bytes; } own_dead_stats;
// Up to max covered entries go: idle since now - idle, not looked up in ALIAS_IDLE submissions, unpinned, and written over by another entry of their pool.
// *serials (malloc'ed, caller frees; NULL when none) lists the serials whose entries went
own_dead_stats own_alias_dead_locked(uint32_t now, uint32_t idle, own_use_fn use_of, uint32_t max, uint64_t **serials, uint32_t *nserials);
// A pool's Finalize: its sharing entries, its live textures' entries and every serial entry naming one of them, of `also`, or a part or view of one go
uint32_t own_pool_finalize_locked(const void *pool, CFSetRef also);
void own_alias_at_locked(const void *pool, int64_t off, void (^fn)(const alias_key *k, id o));   // the entries that start at off (its index cell)
void own_alias_each_locked(void (^fn)(const alias_key *k, id o));
typedef struct { uint32_t n, cap, cells; uint64_t evicted, taken, pool_entries, looks; } own_alias_stats;
own_alias_stats own_alias_stats_locked(void);
// Entries only the sharing table keeps: bytes count each texture once; oldest_idle in submissions
typedef struct { uint32_t entries; uint64_t bytes, writable_bytes, oldest_idle; } own_alias_unheld;
own_alias_unheld own_alias_unheld_locked(void);

int own_retire_locked(id o);    // fence passed: 1 = may leave the residency set; 0 = held: the owner keeps it, the caller drops only its reference
int own_keeps_locked(id o);     // pinned or shared: stays resident whatever its entries do
int own_object_free(id o);      // takes g_smu; 1: nothing holds o, the replay layer may write it over
int own_holds_locked(const void *o);   // by address alone
// Objects that left their last holder since the last take, for the view cache; they may be held again (check own_holds_locked).
// 1 = more went than the list holds (check all); *list valid while g_smu stays held.
int own_gone_take_locked(const void *const **list, unsigned *n);

void own_tex_live_put_locked(const tex_t *t);
int own_tex_live_locked(uint64_t serial);
int own_tex_live(uint64_t serial);
int own_tex_end_locked(tex_t *t);   // -1: t is a copy and the object it was initialized in still holds it; else whether it was live

// Program pins, under the submission lock
void own_prog_pin(uint64_t serial);

// g_ser_gen is read directly (atomic load on hot paths); only the owner's functions change it
enum { SER_NOTES = 64,     // notes kept between two takes
       SER_LOG = 4096 };   // by generation: a cache further behind rebuilds whole
extern _Atomic uint64_t g_ser_gen;
void own_note(uint64_t serial);                                  // +1, naming serial
void own_epoch(void);                                            // +1, naming no serial
unsigned own_notes_take(uint64_t out[SER_NOTES], uint64_t *gen); // at most SER_NOTES, else the count noted; *gen: the generation they reach
int own_note_at(uint64_t gen, uint64_t *serial);                 // 1 with the serial noted at gen (0: a takeover); 0 when none was noted or the log moved past it: rebuild whole

typedef struct { uint32_t live, live_cap, ghost, retire_wait; uint64_t ghost_puts; } own_stats;
own_stats own_stats_locked(void);
void own_each_locked(void (^fn)(uint64_t serial, id o, int ghost));
// Entries whose object gone() names go, each serial noted and passed to each(). → how many went
uint32_t own_drop_where_locked(int (^gone)(id o), void (^each)(uint64_t serial));

// Provided by the replay layer, called with g_smu held
void own_hook_drop(id o, int keep_resident);   // released after the submission in flight; keep_resident: something else holds it
uint64_t own_hook_submission(void);            // g_fence + 1, which ALIAS_IDLE and GHOST_IDLE_SUBS count
uint64_t own_hook_frame(void);                 // presents so far, which GHOST_IDLE counts
void own_hook_parts(id o, void (^add)(id part));   // textures an object stands for besides itself (a package's), which entries may hold directly

// For t_serial_owner only (it links the owner alone)
const void *own_live_ref_locked(uint64_t k);
int serial_table_holds(const void *o);   // a live entry or a texture pin holds o or a view of it
int entry_holds(const void *o);          // or a ghost entry
int tex_object_pinned(id o);
int own_shared_locked(id o);
id own_alias_get_locked(const alias_key *k);
int own_rename_locked(id from, id to, uint64_t serial);                // no epoch
void own_alias_put_locked(const alias_key *k, id o, uint64_t seen);   // seen: the submission of its last lookup (0: none)
int own_alias_del_locked(const alias_key *k);
uint32_t own_alias_take_over_locked(const alias_key *k);
int own_check_locked(char *why, size_t n);   // 0 with the first difference in why
