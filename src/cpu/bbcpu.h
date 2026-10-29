/* bbcpu: runs the game's x86-64 code on any host (docs/ARM64_NATIVE.md). Guest memory is the
 * host's (identity-mapped); a guest call or jump to an address outside the guest code ranges is a
 * call of a host function (SysV arguments bridged to the host ABI). */
#ifndef BB_CPU_H
#define BB_CPU_H
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* bbcpu is built on macOS (src/cpu, build.sh); elsewhere guest code runs natively. */
#if defined(__APPLE__)
#define BB_HAVE_BBCPU 1
#endif
#ifdef BB_HAVE_BBCPU

/* Whether guest code runs through bbcpu (BB_CPU=interp in x86-64 builds; always on arm64). */
int bbcpu_enabled(void);
/* The ranges holding guest code (the linked image); anything else is host code. */
void bbcpu_add_guest_code(uintptr_t start, size_t size);
/* Calls guest function `fn` on this thread with up to 6 integer arguments; returns rax. */
uint64_t bbcpu_call(uintptr_t fn, int count, const uint64_t *args);
/* Like bbcpu_call with SysV integer arguments (rdi rsi rdx rcx r8 r9) and vector ones (xmm0-7);
 * rax, rdx and xmm0, xmm1 back. */
void bbcpu_call_full(uintptr_t fn, const uint64_t gpr[6], const uint8_t xmm[8][16], uint64_t out_gpr[2],
                     uint8_t out_xmm[2][16]);
/* Native versions of game functions (docs/RECOMPILATION.md): a call of guest `address` runs host
 * function `fn` (SysV arguments bridged, like a host import). Set before guest code runs. */
void bbcpu_set_native(uint64_t address, const void *fn);
/* Probes (diagnostics): host function `fn` runs when guest code reaches `address`, with the
 * guest's registers (rax rcx rdx rbx rsp rbp rsi rdi r8-r15) and `tag`; the guest code then runs
 * as usual. Set before guest code runs. */
typedef void (*BbProbe)(const uint64_t regs[16], uint64_t address, uint32_t tag);
void bbcpu_set_probe(uint64_t address, BbProbe fn, uint32_t tag);
/* Guards: `enter` runs when guest code reaches `address` (a function's first instruction) and
 * returns a cookie; with a non-zero cookie, `leave(cookie)` runs when that call returns (its
 * return address is kept on a per-thread stack and the guest's slot holds BB_GUARD_RETURN until
 * then). Recompiled functions (BB_RECOMP_LIB) call each other directly: not guarded there. */
typedef uint64_t (*BbGuardEnter)(const uint64_t regs[16], uint32_t tag);
typedef void (*BbGuardLeave)(uint64_t cookie);
void bbcpu_set_guard(uint64_t address, BbGuardEnter enter, BbGuardLeave leave, uint32_t tag);
/* Active guards on this thread, filtered by owner; terminating diagnostics only. */
size_t bbcpu_guard_cookies(BbGuardLeave leave, uint64_t *cookies, size_t capacity);
/* BB_RECOMP_LIB: recompiled game functions replace the translated ones (recomp.c). */
void bbcpu_recomp_load(uint64_t image_base);
/* Code at [address, address + size) is rewritten while the game runs: the recompiled library
 * leaves the functions holding it to the translator. Set before bbcpu_recomp_load. */
void bbcpu_recomp_exclude(uint64_t address, size_t size);
/* How many game functions the recompiled library replaced (0: none loaded). */
size_t bbcpu_recomp_functions(void);
/* Like bbcpu_call on a new guest stack [stack, stack + size) (thread entry, game entry). */
uint64_t bbcpu_call_on_stack(uintptr_t fn, int count, const uint64_t *args, void *stack,
                             size_t size);
/* The guest thread pointer (TCB, the guest's fs base) of this thread. */
void bbcpu_set_thread_pointer(uintptr_t tcb);
/* Optional diagnostic label for the calling thread. */
void bbcpu_set_thread_label(const char *name);
/* The guest's SSE and x87 control words (MXCSR, FCW) of this thread: get reads them (the defaults
 * before the thread ran guest code), set gives them to its guest code. A new thread starts with its
 * creator's, as FreeBSD's libthr does. */
void bbcpu_get_fp_control(uint32_t *mxcsr, uint16_t *fcw);
void bbcpu_set_fp_control(uint32_t mxcsr, uint16_t fcw);
/* Note a completed host memory write for opt-in JIT watch diagnostics. */
void bbcpu_watch_write(uintptr_t address, size_t size);
/* The GS displacement of the loader's rewritten `mov rax, fs:[0]` (relocation kind 3). */
void bbcpu_set_tls_displacement(uint32_t displacement);
/* Whether address is guest code. */
int bbcpu_is_guest(uintptr_t address);
/* Decoded code over [address, address + size) is dropped (guest code patched at run time). */
void bbcpu_invalidate(uintptr_t address, size_t size);

/* int3 in guest code (hooks patched into the game): the handler gets the guest's registers as
 * the hardware would after the int3 (rip past it), may change them, and returns 1 if it handled
 * the trap. */
typedef struct {
    uint64_t r[16]; /* rax rcx rdx rbx rsp rbp rsi rdi r8-r15 */
    uint64_t rip;
} BbGuestRegs;
/* Handlers are tried in the order they were set (at most four) until one handles the trap. */
void bbcpu_set_trap_handler(int (*handler)(BbGuestRegs *regs));
/* In a trap handler: the rest of the thread's guest state, readable and writable until the
 * handler returns (rflags, MXCSR, ymm0-15 as 32 bytes each). Returns 0 outside a handler. */
typedef struct {
    uint64_t *flags;
    uint32_t *mxcsr;
    uint8_t (*ymm)[32];
} BbTrapState;
int bbcpu_trap_state(BbTrapState *state);

/* The guest's time stamp counter (rdtsc) and its frequency: 1.6 GHz like the PS4's. */
uint64_t bbcpu_tsc(void);
#define BBCPU_TSC_HZ UINT64_C(1600000000)

/* A fault in translated code that only needs an unaligned access emulated (arm64): handled. */
int bbcpu_handle_alignment_fault(void *context);
/* Registers at a translated fault; rip is exact with BB_JIT_FAULT_MAP=1, otherwise zero.
 * Returns 0 outside translated code. Called only by a terminating fault handler. */
int bbcpu_fault_guest_regs(void *context, BbGuestRegs *regs);
/* Describes a fault in translated code (guest block and registers); 0 when not one. */
int bbcpu_describe_fault(void *context, char *out, size_t size);

/* Statistics and the guest location of this thread, for logs. */
uint64_t bbcpu_guest_rip(void);
/* In a host import called from guest code: the guest return address (the game's call site,
 * past its call), read at the guest stack pointer; 0 when not known. */
uint64_t bbcpu_import_return(void);
/* A return address read from the guest stack, as the guest's code left it: where the recompiled
 * library entered the translator, a frame's own address stands there instead of the caller's
 * (recomp.c), and this gives the caller's back. Any other value is returned as it is. */
uint64_t bbcpu_guest_return(uint64_t value);
void bbcpu_report(void);

#else
static inline int bbcpu_enabled(void) { return 0; }
static inline int bbcpu_is_guest(uintptr_t address) { (void)address; return 0; }
static inline void bbcpu_add_guest_code(uintptr_t start, size_t size) { (void)start; (void)size; }
static inline void bbcpu_recomp_exclude(uint64_t address, size_t size) { (void)address; (void)size; }
static inline void bbcpu_invalidate(uintptr_t address, size_t size) { (void)address; (void)size; }
static inline uint64_t bbcpu_call(uintptr_t fn, int count, const uint64_t *args) {
    (void)fn; (void)count; (void)args; return 0;
}
static inline void bbcpu_set_thread_pointer(uintptr_t tcb) { (void)tcb; }
static inline void bbcpu_set_thread_label(const char *name) { (void)name; }
static inline void bbcpu_get_fp_control(uint32_t *mxcsr, uint16_t *fcw) { *mxcsr = 0x1f80; *fcw = 0x37f; }
static inline void bbcpu_set_fp_control(uint32_t mxcsr, uint16_t fcw) { (void)mxcsr; (void)fcw; }
static inline void bbcpu_watch_write(uintptr_t address, size_t size) { (void)address; (void)size; }
static inline uint64_t bbcpu_tsc(void) { return 0; }
static inline uint64_t bbcpu_import_return(void) { return 0; }
static inline uint64_t bbcpu_guest_return(uint64_t value) { return value; }
static inline int bbcpu_handle_alignment_fault(void *context) { (void)context; return 0; }
static inline int bbcpu_describe_fault(void *context, char *out, size_t size) { (void)context; (void)out; (void)size; return 0; }
#define BBCPU_TSC_HZ UINT64_C(1600000000)
typedef struct { uint64_t r[16]; uint64_t rip; } BbGuestRegs;
static inline int bbcpu_fault_guest_regs(void *context, BbGuestRegs *regs) { (void)context; (void)regs; return 0; }
static inline void bbcpu_set_trap_handler(int (*handler)(BbGuestRegs *regs)) { (void)handler; }
typedef struct { uint64_t *flags; uint32_t *mxcsr; uint8_t (*ymm)[32]; } BbTrapState;
static inline int bbcpu_trap_state(BbTrapState *state) { (void)state; return 0; }
#endif

/* Convenience for host code calling guest function pointers. */
static inline uint64_t bb_guest_call0(const void *fn) { return bbcpu_call((uintptr_t)fn, 0, NULL); }
static inline uint64_t bb_guest_call1(const void *fn, uint64_t a) {
    return bbcpu_call((uintptr_t)fn, 1, &a);
}
static inline uint64_t bb_guest_call2(const void *fn, uint64_t a, uint64_t b) {
    const uint64_t args[2] = {a, b};
    return bbcpu_call((uintptr_t)fn, 2, args);
}
static inline uint64_t bb_guest_call3(const void *fn, uint64_t a, uint64_t b, uint64_t c) {
    const uint64_t args[3] = {a, b, c};
    return bbcpu_call((uintptr_t)fn, 3, args);
}
static inline uint64_t bb_guest_call4(const void *fn, uint64_t a, uint64_t b, uint64_t c,
                                      uint64_t d) {
    const uint64_t args[4] = {a, b, c, d};
    return bbcpu_call((uintptr_t)fn, 4, args);
}
static inline uint64_t bb_guest_call5(const void *fn, uint64_t a, uint64_t b, uint64_t c,
                                      uint64_t d, uint64_t e) {
    const uint64_t args[5] = {a, b, c, d, e};
    return bbcpu_call((uintptr_t)fn, 5, args);
}

#ifdef __cplusplus
}
#endif
#endif
