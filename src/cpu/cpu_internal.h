/* bbcpu internals: guest state, decoded instructions, helpers shared by the interpreter parts. */
#ifndef BB_CPU_INTERNAL_H
#define BB_CPU_INTERNAL_H
#include "bbcpu.h"
#include <Zydis/Zydis.h>
#include <setjmp.h>
#include <stdint.h>
#include <string.h>

/* General registers in x86 encoding order. */
enum { RAX, RCX, RDX, RBX, RSP, RBP, RSI, RDI, R8, R9, R10, R11, R12, R13, R14, R15 };
/* RFLAGS bits. */
enum {
    F_CF = 1u << 0, F_PF = 1u << 2, F_AF = 1u << 4, F_ZF = 1u << 6, F_SF = 1u << 7,
    F_DF = 1u << 10, F_OF = 1u << 11,
    F_ARITH = F_CF | F_PF | F_AF | F_ZF | F_SF | F_OF,
};

typedef union {
    uint8_t b[32];
    int8_t sb[32];
    uint16_t w[16];
    int16_t sw[16];
    uint32_t d[8];
    int32_t sd[8];
    uint64_t q[4];
    int64_t sq[4];
    float f[8];
    double fd[4];
} BbVec;

enum { WATCH_RECORDS = 64 };
typedef struct { uint64_t tick, rip, address, value, sequence, reserved[3]; } BbWatchRecord;
_Static_assert(sizeof(BbWatchRecord) == 64 && WATCH_RECORDS == 64, "JIT watch ring addressing");
typedef struct BbWatch {
    uint64_t count;
    BbWatchRecord records[WATCH_RECORDS];
    struct BbWatch *next;
    char name[32];
} BbWatch;
void bbcpu_watch_dump(void);

typedef struct BbCpu {
    uint64_t r[16];
    uint64_t rip;
    uint64_t flags;
    BbVec v[16];
    uint32_t mxcsr;
    uint64_t fs_base, gs_base;
    /* GS-relative slot the loader's rewritten TLS loads read: gs_base + displacement = &tcb. */
    uint64_t tcb;
    /* x87: values as doubles (the game uses x87 for few, plain computations). */
    double st[8];
    int top;
    uint16_t fcw, fsw;
    /* Nesting of host -> guest calls on this thread. */
    int depth;
    /* The guest stack's end (stack arguments of host calls are copied up to it). */
    uint64_t stack_top;
    /* JIT flag state (jit_arm64.c): bits 31-28 NZCV with C = !CF, the other bits RFLAGS's;
     * PF and AF from the last result (low byte parity) and its operands' xor (bit 4). */
    uint64_t jf_flags, jf_res, jf_ab;
    /* JIT: the direct branch of the block exit just taken (patched to the next block's code), and
     * this thread's indirect branch cache (guest rip -> translated code). */
    uint64_t jit_link;
    struct BbJitCacheEntry *jit_cache;
    uint64_t instructions;
    BbWatch *watch; /* allocated only for BB_JIT_WATCH diagnostics */
    /* Calls of recompiled functions not yet added to bbcpu_recomp_calls (trace.h). Last: libraries
     * built before it read the fields above at the same offsets. */
    uint32_t recomp_pending;
} BbCpu;

/* Operand of a decoded instruction (compact copy of Zydis's). */
enum { OP_NONE, OP_REG, OP_MEM, OP_IMM, OP_AGEN };
enum { RK_GPR, RK_VEC, RK_X87, RK_OTHER };
typedef struct {
    uint8_t type;
    uint8_t size;      /* bytes */
    uint8_t kind;      /* register kind */
    uint8_t reg;       /* GPR 0-15, vector 0-15, x87 0-7 */
    uint8_t high8;     /* AH, CH, DH, BH */
    uint8_t base, index, scale; /* memory: 0xff = none, base 0xfe = rip */
    uint8_t segment;   /* 0 none, 1 fs, 2 gs */
    int64_t disp;      /* memory displacement or immediate */
} BbOp;

typedef struct BbInsn {
    uint16_t mnemonic;    /* ZydisMnemonic */
    uint8_t length;
    uint8_t count;        /* visible operands */
    uint8_t opsize;       /* operand width, bytes */
    uint8_t vex;          /* VEX/EVEX encoded */
    uint8_t vl;           /* vector length, bytes (VEX) */
    uint8_t lock, rep, repne;
    uint8_t branch;       /* ends a block */
    uint16_t freads;      /* arithmetic flags read (RFLAGS bits) */
    uint16_t fwrites;     /* arithmetic flags written, defined or not */
    BbOp op[5];
} BbInsn;

/* A decoded run of instructions ending at a branch (or a size cap). */
typedef struct BbBlock {
    uint64_t start;
    void *jit_code;        /* arm64 translation (jit_arm64.c), or null */
    uint8_t jit_failed;    /* its first instruction is interpreted */
    uint32_t count;
    struct BbBlock *next;  /* hash chain */
    BbInsn insn[];
} BbBlock;

/* Stops guest execution of the current thread with a message (an unsupported instruction). */
_Noreturn void bbcpu_fatal(BbCpu *cpu, const BbInsn *in, const char *what);
/* Host function call bridge (hostcall_<arch>.S): integer args, then 16-byte vector args. */
typedef struct {
    uint64_t gpr[8];      /* rdi rsi rdx rcx r8 r9, then the first two stack slots */
    uint64_t stack[16];   /* the guest's stack arguments */
    BbVec xmm[8];         /* low 16 bytes used */
    uint64_t rax, rdx;
    BbVec xmm0, xmm1;
} BbHostArgs;
void bbcpu_hostcall(const void *fn, BbHostArgs *args);

/* Decode cache. */
const BbBlock *bbcpu_block(uint64_t rip);
int bbcpu_is_guest_code(uint64_t address);

/* Interpreter: runs from cpu->rip until rip == stop (the sentinel return address). */
void bbcpu_run(BbCpu *cpu, uint64_t stop);
/* Called by bbcpu_run when the guest reaches host code other than `stop` (recomp.c: the return
 * address of an outer frame of the runtime, after a longjmp or an exception; it does not return
 * then). NULL: none. */
extern void (*bbcpu_return_hook)(BbCpu *cpu);
/* Executes one instruction at cpu->rip; returns the next rip (interp.c). */
uint64_t bbcpu_step(BbCpu *cpu, const BbInsn *in);
/* A host function reached at cpu->rip with its return address on the guest stack (interp.c). */
void bbcpu_call_host_at_rip(BbCpu *cpu);
BbBlock *bbcpu_block_uncached(uint64_t rip); /* decode.c: the caller frees it */
/* JIT (jit_arm64.c, arm64 hosts): runs like bbcpu_run; 0 when unavailable (BB_JIT=0). */
int bbcpu_jit_run(BbCpu *cpu, uint64_t stop);
/* The guest TSC as virtual counter * num / den (arm64; interp.c). */
void bbcpu_tsc_scale(uint64_t *num, uint64_t *den);
/* Whether the JIT translates the instruction itself (not through the interpreter). */
int bbcpu_jit_translates(const BbInsn *in, uint64_t rip);
/* Vector and x87 instructions (interp_vec.c, interp_x87.c); return 0 when not theirs. */
int bbcpu_exec_vec(BbCpu *cpu, const BbInsn *in);
int bbcpu_exec_x87(BbCpu *cpu, const BbInsn *in);

/* Operand helpers (interp.c). */
uint64_t bbcpu_addr(BbCpu *cpu, const BbInsn *in, const BbOp *op);
uint64_t bbcpu_read(BbCpu *cpu, const BbInsn *in, const BbOp *op);
void bbcpu_write(BbCpu *cpu, const BbInsn *in, const BbOp *op, uint64_t value);

/* Guest memory accesses. x86 orders stores after stores and loads after loads (TSO); arm64
 * does not, and the game publishes data between threads with plain stores (glyphs rendered by a
 * worker went missing). On arm64 guest loads acquire and stores release (bb_fence_* around the
 * accesses that cannot: unaligned and vector ones). */
#if defined(__aarch64__)
#define bb_fence_load() __atomic_thread_fence(__ATOMIC_ACQUIRE)
#define bb_fence_store() __atomic_thread_fence(__ATOMIC_RELEASE)
#else
#define bb_fence_load() ((void)0)
#define bb_fence_store() ((void)0)
#endif
static inline uint64_t bb_load(uint64_t address, int size) {
#if defined(__aarch64__)
    if (!(address & (uint64_t)(size - 1))) {
        switch (size) {
        case 1: return __atomic_load_n((const uint8_t *)address, __ATOMIC_ACQUIRE);
        case 2: return __atomic_load_n((const uint16_t *)address, __ATOMIC_ACQUIRE);
        case 4: return __atomic_load_n((const uint32_t *)address, __ATOMIC_ACQUIRE);
        default: return __atomic_load_n((const uint64_t *)address, __ATOMIC_ACQUIRE);
        }
    }
    uint64_t v = 0;
    memcpy(&v, (const void *)address, (size_t)size);
    bb_fence_load();
    return v;
#endif
    switch (size) {
    case 1: return *(const uint8_t *)address;
    case 2: { uint16_t v; memcpy(&v, (const void *)address, 2); return v; }
    case 4: { uint32_t v; memcpy(&v, (const void *)address, 4); return v; }
    default: { uint64_t v; memcpy(&v, (const void *)address, 8); return v; }
    }
}
static inline void bb_store(uint64_t address, int size, uint64_t value) {
#if defined(__aarch64__)
    if (!(address & (uint64_t)(size - 1))) {
        switch (size) {
        case 1: __atomic_store_n((uint8_t *)address, (uint8_t)value, __ATOMIC_RELEASE); return;
        case 2: __atomic_store_n((uint16_t *)address, (uint16_t)value, __ATOMIC_RELEASE); return;
        case 4: __atomic_store_n((uint32_t *)address, (uint32_t)value, __ATOMIC_RELEASE); return;
        default: __atomic_store_n((uint64_t *)address, value, __ATOMIC_RELEASE); return;
        }
    }
    bb_fence_store();
#endif
    switch (size) {
    case 1: *(uint8_t *)address = (uint8_t)value; break;
    case 2: { uint16_t v = (uint16_t)value; memcpy((void *)address, &v, 2); break; }
    case 4: { uint32_t v = (uint32_t)value; memcpy((void *)address, &v, 4); break; }
    default: memcpy((void *)address, &value, 8); break;
    }
}
static inline uint64_t bb_mask(int size) {
    return size >= 8 ? ~UINT64_C(0) : (UINT64_C(1) << (size * 8)) - 1;
}
static inline uint64_t bb_sign(int size) { return UINT64_C(1) << (size * 8 - 1); }
static inline int64_t bb_sext(uint64_t value, int size) {
    const int shift = 64 - size * 8;
    return shift ? (int64_t)(value << shift) >> shift : (int64_t)value;
}

_Static_assert(__builtin_offsetof(BbHostArgs, stack) == 64, "hostcall.S layout");
_Static_assert(__builtin_offsetof(BbHostArgs, xmm) == 192, "hostcall.S layout");
_Static_assert(__builtin_offsetof(BbHostArgs, rax) == 448, "hostcall.S layout");
_Static_assert(__builtin_offsetof(BbHostArgs, xmm0) == 464, "hostcall.S layout");
_Static_assert(__builtin_offsetof(BbHostArgs, xmm1) == 496, "hostcall.S layout");

/* Sets this host thread's FPCR.FZ from the guest MXCSR (cpu.c). */
void bbcpu_apply_host_fp(uint32_t mxcsr);

#endif
