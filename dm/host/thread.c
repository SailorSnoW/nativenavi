// Guest thread slots: gate entry, x18 (cleared), tpidr_el0 = ELF TLS block, tpidrro_el0 = ThreadLocalRegion (0x200). The kernel owns
// TLR[0x1f8] (the current thread object) and TLR[0x110] (the thread's ID).
#include "dm.h"
#include <sys/mman.h>
#define TLS_SIZE 8192
#define TLR_SIZE 0x200
#define THREAD_OBJ_SIZE 0x400
static pthread_key_t g_keyown;
static pthread_once_t g_keyown_once = PTHREAD_ONCE_INIT;
static void own_free(void *p) {
    void **own = p;
    free(own[0]);
    free(own[1]);
    free(own);
}
static void own_key_init(void) { if (pthread_key_create(&g_keyown, own_free)) die("cannot create the free key for thread-local blocks"); }
void guest_thread_init(void) {
    pthread_setspecific(g_key18, 0);
    // the guest overwrites the slots: g_keyown's destructor frees the blocks (free only, safe on a small guest stack)
    pthread_once(&g_keyown_once, own_key_init);
    void **own = calloc(2, sizeof *own);
    uint8_t *tlr = calloc(1, TLR_SIZE + THREAD_OBJ_SIZE);
    if (!own || !tlr) die("guest thread-local block allocation failed");
    own[0] = calloc(1, TLS_SIZE);
    own[1] = tlr;
    if (!own[0]) die("guest thread TLS block allocation failed");
    *(void **)(tlr + 0x1f8) = tlr + TLR_SIZE;
    pthread_setspecific(g_keytls, own[0]);
    pthread_setspecific(g_keytlr, tlr);
    void **prev = pthread_getspecific(g_keyown);
    if (prev) own_free(prev);
    pthread_setspecific(g_keyown, own);
}
// never 0 (an unlocked mutex's owner word): without it every unlocked SDK 22 mutex reads as the caller's (docs/reference/threads.md)
void guest_thread_set_id(uint64_t id) { *(uint32_t *)((uint8_t *)pthread_getspecific(g_keytlr) + 0x110) = (uint32_t)id; }

// Gate stacks: SVC frames and handlers run on a stack of the thread's own, never below the guest's sp, as Horizon leaves it untouched.
// A guest thread's is its pthread stack below thread_main's frame; the main guest thread's is mapped once, kept for a reset.
#define GATE_SLOTS 1024                 // = entry.S .rept count and dm_gate_top size
#define GATE_STACK_SIZE (1 << 20)       // = entry.S nesting check
extern uint64_t dm_gate_top[GATE_SLOTS];
extern const uint8_t dm_gate_stubs[];
static pthread_mutex_t g_gate_lock = PTHREAD_MUTEX_INITIALIZER;
static uint16_t g_gate_free[GATE_SLOTS];
static int g_gate_nfree = -1;
static pthread_key_t g_keygslot;   // slot + 1
static pthread_once_t g_gslot_once = PTHREAD_ONCE_INIT;
static void gslot_release(void *p) {
    pthread_mutex_lock(&g_gate_lock);
    g_gate_free[g_gate_nfree++] = (uint16_t)((uintptr_t)p - 1);
    pthread_mutex_unlock(&g_gate_lock);
}
static void gslot_key_init(void) {
    if (pthread_key_create(&g_keygslot, gslot_release)) die("cannot create the gate slot key");
    if ((uintptr_t)dm_gate_top & 0xfff) die("gate stack table %p is not page-aligned (entry.S stubs address it with adrp alone)", (void *)dm_gate_top);
}
uintptr_t guest_gate_stack_top(void) {
    uintptr_t k = (uintptr_t)pthread_getspecific(g_keygslot);
    return k ? (uintptr_t)dm_gate_top[k - 1] : 0;
}
void dm_gate_nested(uint64_t sp) { die("SVC gate entered on its own gate stack (sp %#llx): a gate handler ran guest code", (unsigned long long)sp); }
__attribute__((noinline)) void guest_gate_stack(int own_stack) {
    pthread_once(&g_gslot_once, gslot_key_init);
    uint8_t *top;
    if (own_stack) top = (uint8_t *)__builtin_frame_address(0);   // below this frame is free once the guest runs elsewhere
    else {
        static uint8_t *main_top;
        if (!main_top) {
            uint8_t *m = mmap(NULL, GATE_STACK_SIZE + 16384, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
            if (m == MAP_FAILED || mprotect(m, 16384, PROT_NONE)) die("cannot map a gate stack (errno %d)", errno);   // guard page
            main_top = m + 16384 + GATE_STACK_SIZE;
        }
        top = main_top;
    }
    uintptr_t prev = (uintptr_t)pthread_getspecific(g_keygslot);
    int k;
    if (prev) k = (int)prev - 1;
    else {
        pthread_mutex_lock(&g_gate_lock);
        if (g_gate_nfree < 0) {
            g_gate_nfree = 0;
            for (int i = GATE_SLOTS - 1; i >= 0; i--) g_gate_free[g_gate_nfree++] = (uint16_t)i;
        }
        if (!g_gate_nfree) die("more than %d threads with a gate stack", GATE_SLOTS);
        k = g_gate_free[--g_gate_nfree];
        pthread_mutex_unlock(&g_gate_lock);
        pthread_setspecific(g_keygslot, (void *)(uintptr_t)(k + 1));
    }
    dm_gate_top[k] = (uint64_t)(uintptr_t)top & ~15ull;
    pthread_setspecific(g_keygate, (void *)(dm_gate_stubs + 16 * k));
}
