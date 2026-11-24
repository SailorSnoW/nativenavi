// Big-stack entry for HLE_FORCE takeovers and log lines (docs/reference/memory.md#host-calls-on-small-guest-stacks): with sp in the guest block, switch to
// this thread's 512 KB host stack, call x16 with x0-x8 / d0-d7 untouched and the stack arguments copied, and switch back; elsewhere jump straight through.
#include <pthread.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdatomic.h>
#include <sys/mman.h>
#define DM_LOG_IMPL   // dm_log is defined here, not by dm.h's weak one
#include "dm.h"

int dm_guest_as_contains(uintptr_t a);

#define BS_SIZE (512u << 10)
#define BS_GUARD 16384u
static pthread_key_t g_bs_key;
static pthread_once_t g_bs_once = PTHREAD_ONCE_INIT;
static void bs_free(void *top) { if (top) munmap((uint8_t *)top - BS_SIZE - BS_GUARD, BS_SIZE + BS_GUARD); }
static void bs_init(void) { if (pthread_key_create(&g_bs_key, bs_free)) die("big stack pthread_key_create"); }

// sp in the guest block -> this thread's host stack top (16-aligned); else 0
uintptr_t dm_bigstack_top(uintptr_t sp);
uintptr_t dm_bigstack_top(uintptr_t sp) {
    if (__builtin_expect(atomic_load_explicit(&dm_reset_stop, memory_order_relaxed), 0)) dm_reset_entry(sp);   // reset: a game thread may end here
    if (!dm_guest_as_contains(sp)) return 0;
    pthread_once(&g_bs_once, bs_init);
    uint8_t *top = pthread_getspecific(g_bs_key);
    if (!top) {
        uint8_t *m = mmap(NULL, BS_SIZE + BS_GUARD, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
        if (m == MAP_FAILED) {
            fputs("[dm] [RED] big stack mapping failed, staying on the guest stack this time\n", stderr);   // not LOG: it would come back here
            return 0;
        }
        mprotect(m, BS_GUARD, PROT_NONE);
        top = m + BS_GUARD + BS_SIZE;
        pthread_setspecific(g_bs_key, top);
    }
    return (uintptr_t)top;
}

// Frame (0xb0, original stack): +0 x0-x8, x16, +0x50 d0-d7, +0x90 x17 / x19, +0xa0 x29 / x30. enter_n: x17 = stack argument bytes, 16-64.
// The original sp and the game's return address go at the new stack top; x19 (callee-saved) points at them.
__asm__(
    ".text\n"
    ".p2align 2\n"
    ".globl _dm_bigstack_enter\n"
    ".private_extern _dm_bigstack_enter\n"
    "_dm_bigstack_enter:\n"
    "    mov x17, #0\n"
    ".globl _dm_bigstack_enter_n\n"
    ".private_extern _dm_bigstack_enter_n\n"
    "_dm_bigstack_enter_n:\n"
    "    sub sp, sp, #0xb0\n"
    "    stp x29, x30, [sp, #0xa0]\n"
    "    add x29, sp, #0xa0\n"
    "    stp x0, x1, [sp, #0x00]\n"
    "    stp x2, x3, [sp, #0x10]\n"
    "    stp x4, x5, [sp, #0x20]\n"
    "    stp x6, x7, [sp, #0x30]\n"
    "    stp x8, x16, [sp, #0x40]\n"
    "    stp d0, d1, [sp, #0x50]\n"
    "    stp d2, d3, [sp, #0x60]\n"
    "    stp d4, d5, [sp, #0x70]\n"
    "    stp d6, d7, [sp, #0x80]\n"
    "    stp x17, x19, [sp, #0x90]\n"
    "    mov x0, sp\n"
    "    bl _dm_bigstack_top\n"
    "    mov x17, x0\n"
    "    ldp x0, x1, [sp, #0x00]\n"
    "    ldp x2, x3, [sp, #0x10]\n"
    "    ldp x4, x5, [sp, #0x20]\n"
    "    ldp x6, x7, [sp, #0x30]\n"
    "    ldp x8, x16, [sp, #0x40]\n"
    "    ldp d0, d1, [sp, #0x50]\n"
    "    ldp d2, d3, [sp, #0x60]\n"
    "    ldp d4, d5, [sp, #0x70]\n"
    "    ldp d6, d7, [sp, #0x80]\n"
    "    cbz x17, 1f\n"
    "    mov x9, sp\n"
    "    ldr x10, [x9, #0xa8]\n"         // the game's return address
    "    ldr x11, [x9, #0x90]\n"         // bytes of stack arguments
    "    mov sp, x17\n"
    "    stp x9, x10, [sp, #-16]!\n"
    "    mov x19, sp\n"
    "    add x12, x9, #0xb0\n"
    "    sub sp, sp, x11\n"
    "    mov x13, sp\n"
    "    cbz x11, 3f\n"
    "2:  ldp x14, x15, [x12], #16\n"
    "    stp x14, x15, [x13], #16\n"
    "    subs x11, x11, #16\n"
    "    b.ne 2b\n"
    "3:  blr x16\n"
    ".globl _dm_bigstack_ret\n"
    ".private_extern _dm_bigstack_ret\n"
    "_dm_bigstack_ret:\n"
    "    ldr x9, [x19]\n"
    "    mov sp, x9\n"
    "    ldr x19, [sp, #0x98]\n"
    "    ldp x29, x30, [sp, #0xa0]\n"
    "    add sp, sp, #0xb0\n"
    "    ret\n"
    "1:\n"
    "    ldp x29, x30, [sp, #0xa0]\n"
    "    add sp, sp, #0xb0\n"
    "    br x16\n");

// Only a thread's outermost takeover switches, so the slot at its host stack top belongs to the call in progress.
void *dm_bigstack_caller(void *ra) {
    extern const char dm_bigstack_ret[];
    void **top = ra == (void *)dm_bigstack_ret ? pthread_getspecific(g_bs_key) : NULL;
    return top ? top[-1] : ra;
}

// Only the formatting moves to the host stack; the variable arguments stay on the caller's.
static void log_emit(const char *fmt, va_list *ap) __attribute__((used));
static void log_emit(const char *fmt, va_list *ap) {
    flockfile(stderr);
    vfprintf(stderr, fmt, *ap);
    fputc('\n', stderr);
    funlockfile(stderr);
}
void log_emit_bs(const char *fmt, va_list *ap);
__asm__(".text\n.p2align 2\n.globl _log_emit_bs\n.private_extern _log_emit_bs\n_log_emit_bs:\n"
        "    adrp x16, _log_emit@PAGE\n    add x16, x16, _log_emit@PAGEOFF\n    b _dm_bigstack_enter\n");
void dm_log(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    log_emit_bs(fmt, &ap);
    va_end(ap);
}
