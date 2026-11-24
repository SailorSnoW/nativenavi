    .arch_extension lse
// Unimplemented NVN functions: x16 = slot address (nvn_slot[i]); reports through nvn_unimpl_c, returns 0.
    .text
    .globl _nvn_unimpl_entry
    .p2align 2
_nvn_unimpl_entry:
    stp x29, x30, [sp, #-16]!
    mov x29, sp
    mov x3, x2
    mov x2, x1
    mov x1, x0
    mov x0, x16
    bl _nvn_unimpl_c
    ldp x29, x30, [sp], #16
    ret

// DM_NVN_PROF=1 timing entry: x16 = slot address; NVN functions take no stack arguments. Not cntvct_el0: its rate is no fixed ratio of the mach timebase
    .globl _nvn_prof_entry
    .p2align 2
_nvn_prof_entry:
    sub sp, sp, #240
    stp x29, x30, [sp]
    mov x29, sp
    stp x0, x1, [sp, #16]
    stp x2, x3, [sp, #32]
    stp x4, x5, [sp, #48]
    stp x6, x7, [sp, #64]
    stp q0, q1, [sp, #96]
    stp q2, q3, [sp, #128]
    stp q4, q5, [sp, #160]
    stp q6, q7, [sp, #192]
    adrp x9, _nvn_slot@PAGE
    add x9, x9, _nvn_slot@PAGEOFF
    sub x16, x16, x9
    lsr x16, x16, #4                 // slot index
    str x16, [sp, #80]
    bl _mach_absolute_time
    str x0, [sp, #88]
    ldr x16, [sp, #80]
    adrp x9, _nvn_real@PAGE
    add x9, x9, _nvn_real@PAGEOFF
    ldr x17, [x9, x16, lsl #3]
    adrp x9, _nvn_slot@PAGE          // x16 = slot address again: nvn_big_entry finds the implementation through it
    add x9, x9, _nvn_slot@PAGEOFF
    add x16, x9, x16, lsl #4
    ldp x0, x1, [sp, #16]
    ldp x2, x3, [sp, #32]
    ldp x4, x5, [sp, #48]
    ldp x6, x7, [sp, #64]
    ldp q0, q1, [sp, #96]
    ldp q2, q3, [sp, #128]
    ldp q4, q5, [sp, #160]
    ldp q6, q7, [sp, #192]
    blr x17
    stp x0, x1, [sp, #16]
    stp q0, q1, [sp, #96]
    stp q2, q3, [sp, #128]
    bl _mach_absolute_time
    mov x9, x0
    ldr x10, [sp, #88]
    sub x9, x9, x10
    ldr x16, [sp, #80]
    adrp x10, _nvn_prof_ticks@PAGE
    add x10, x10, _nvn_prof_ticks@PAGEOFF
    add x10, x10, x16, lsl #3
    stadd x9, [x10]
    ldp x0, x1, [sp, #16]
    ldp q0, q1, [sp, #96]
    ldp q2, q3, [sp, #128]
    ldp x29, x30, [sp]
    add sp, sp, #240
    ret

// Big-stack entry: runs the implementation on this thread's big host stack, since Metal's driver compiles internal programs on a deeper stack than a guest thread's
    .globl _nvn_big_entry
    .p2align 2
_nvn_big_entry:
    adrp x17, _nvn_slot@PAGE
    add x17, x17, _nvn_slot@PAGEOFF
    sub x16, x16, x17
    lsr x16, x16, #4                 // slot index
    adrp x17, _nvn_big_real@PAGE
    add x17, x17, _nvn_big_real@PAGEOFF
    ldr x16, [x17, x16, lsl #3]
    b _dm_bigstack_enter
