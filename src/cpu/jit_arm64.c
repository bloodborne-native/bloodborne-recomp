/* bbcpu JIT (arm64 hosts): guest basic blocks translated to arm64 code.
 *
 * Registers: guest rax..r15 live in x19-x26, x9-x15, x27 while translated code runs; x28 holds
 * the BbCpu. Flags: x8 holds RFLAGS with the arithmetic flags replaced by NZCV in bits 31-28
 * (C = !CF, arm64's borrow convention, so x86 conditions map onto arm64 ones); PF and AF come
 * from x7 (the last result) and x6 (its operands' xor) when something reads them. Flags are only
 * computed where a later instruction may read them (Zydis's flag masks, per block).
 * Temporaries: x0-x5, x16, x17 (x18 is reserved on Apple platforms).
 *
 * A block ends at its branch: translated code stores the next guest rip in cpu->rip and returns
 * to bbcpu_jit_run, which finds (or translates) the next block. Instructions the translator does
 * not handle run in the interpreter (bbcpu_step) through a call from the translated code.
 * BB_JIT=0 runs the interpreter instead. */
#include "cpu_internal.h"
#include "a64.h"
#include "trace.h"

#if defined(__aarch64__) && defined(__APPLE__)
#include <libkern/OSCacheControl.h>
#include <pthread.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <sys/ucontext.h>

#define M(name) ZYDIS_MNEMONIC_##name

/* The game image's base (platform.c in the game; 0 in tests). */
extern uint64_t bb_image_base __attribute__((weak));
static uint64_t bb_image_base_for_reports(void) { return &bb_image_base ? bb_image_base : 0; }

static const uint8_t G[16] = {19, 20, 21, 22, 23, 24, 25, 26, 9, 10, 11, 12, 13, 14, 15, 27};
enum { T0 = 0, T1 = 1, T2 = 2, T3 = 3, T4 = 4, T5 = 5, TA = 16, TB = 17, FL = 8, FRES = 7,
       FAB = 6, CPU = 28 };
#define OFF(field) ((uint32_t)offsetof(BbCpu, field))

enum { CODE_SIZE = 128 << 20 };
static uint32_t *code_base, *code_end, *code_next;
static uint32_t *enter_stub, *exit_stub, *fallback_stub, *native_stub, *recomp_stub, *probe_stub,
                *guard_return_stub, *host_function_stub;
static pthread_mutex_t jit_lock = PTHREAD_MUTEX_INITIALIZER;
static uint64_t translated_blocks, fallback_insns;
/* Translated blocks in code order (the buffer only grows), for fault reports. */
typedef struct { const uint32_t *start, *end; uint64_t rip; } InstructionRange;
typedef struct {
    const uint32_t *start, *end;
    const BbBlock *block;
    InstructionRange *instructions;
    size_t instruction_count;
} CodeRange;
enum { RANGE_CHUNK_SIZE = 256 };
typedef struct CodeRangeChunk {
    struct CodeRangeChunk *next;
    size_t count;
    CodeRange entries[RANGE_CHUNK_SIZE];
} CodeRangeChunk;
/* Published chunks and entries are retained: a fault handler cannot lock or follow realloc. */
static CodeRangeChunk *range_chunks;

typedef void (*EnterFn)(BbCpu *cpu, const void *code);

/* ---- flag state conversions (C side) ---- */

static void jit_to_x86(BbCpu *cpu) {
    const uint64_t j = cpu->jf_flags;
    uint64_t f = j & ~(uint64_t)F_ARITH & 0x0fffffffu;
    if (!(j & (1u << 29))) f |= F_CF;
    if (j & (1u << 30)) f |= F_ZF;
    if (j & (1u << 31)) f |= F_SF;
    if (j & (1u << 28)) f |= F_OF;
    if (!__builtin_parity((unsigned)(cpu->jf_res & 0xff))) f |= F_PF;
    if ((cpu->jf_ab ^ cpu->jf_res) & 0x10) f |= F_AF;
    cpu->flags = f;
}

static void x86_to_jit(BbCpu *cpu) {
    const uint64_t f = cpu->flags;
    uint64_t j = f & ~(uint64_t)F_ARITH & 0x0fffffffu;
    if (!(f & F_CF)) j |= 1u << 29;
    if (f & F_ZF) j |= 1u << 30;
    if (f & F_SF) j |= 1u << 31;
    if (f & F_OF) j |= 1u << 28;
    cpu->jf_flags = j;
    cpu->jf_res = (f & F_PF) ? 0 : 1;
    cpu->jf_ab = cpu->jf_res ^ ((f & F_AF) ? 0x10 : 0);
}

/* BB_JIT_PROFILE=1: interpreted instructions by mnemonic (from translated code and the
 * dispatcher), printed every 2^24 of them. */
static uint64_t profile[ZYDIS_MNEMONIC_MAX_VALUE + 1];
static int profiling = -1;
static void profile_note(const BbInsn *in) {
    if (profiling < 0) { const char *env = getenv("BB_JIT_PROFILE"); profiling = env && env[0] == '1'; }
    if (!profiling) return;
    __atomic_add_fetch(&profile[in->mnemonic], 1, __ATOMIC_RELAXED);
    static uint64_t total;
    if ((__atomic_add_fetch(&total, 1, __ATOMIC_RELAXED) & ((1u << 24) - 1)) == 0) {
        uint64_t copy[ZYDIS_MNEMONIC_MAX_VALUE + 1];
        memcpy(copy, profile, sizeof(copy));
        printf("JIT profile (interpreted instructions):");
        for (int n = 0; n < 25; ++n) {
            int best = 0;
            for (int m = 1; m <= ZYDIS_MNEMONIC_MAX_VALUE; ++m) if (copy[m] > copy[best]) best = m;
            if (!copy[best]) break;
            printf(" %s %llu", ZydisMnemonicGetString((ZydisMnemonic)best), (unsigned long long)copy[best]);
            copy[best] = 0;
        }
        printf("\n");
    }
}

/* Called from translated code for instructions it does not translate (registers synced). Native
 * and recompiled functions and probes start blocks, which the dispatcher and probe_stub handle:
 * only the instruction runs here. */
static void jit_fallback(BbCpu *cpu, const BbInsn *in) {
    jit_to_x86(cpu);
    bbcpu_step_insn(cpu, in);
    x86_to_jit(cpu);
    ++fallback_insns;
    profile_note(in);
}

/* Called from translated code for a direct call of a game function with a native version
 * (native.c): the native function runs through the guest -> host bridge, registers synced. */
static void jit_native(BbCpu *cpu, const void *fn) {
    jit_to_x86(cpu);
    bbcpu_call_native(cpu, fn);
    x86_to_jit(cpu);
}

/* Called from translated code for a direct call of a recompiled function (recomp.c): its return
 * address is pushed; it returns with cpu->rip at it. */
static void jit_recomp(BbCpu *cpu, RcFn fn) {
    jit_to_x86(cpu);
    if (bbcpu_recomp_thread_ok()) {
        bbcpu_recomp_counted(cpu);
        fn(cpu);
    } else { /* not on this thread (BB_RECOMP_THREADS): translated, returning to a sentinel */
        const uint64_t next = bb_load(cpu->r[RSP], 8);
        bb_store(cpu->r[RSP], 8, UINT64_C(0x0000700000000d00));
        bbcpu_run(cpu, UINT64_C(0x0000700000000d00));
        cpu->rip = next;
    }
    x86_to_jit(cpu);
}

/* ---- code buffer and stubs ---- */

/* Guest xmm0-15 (the low 128 bits of ymm) live in v16-v31 inside translated code; the upper
 * halves stay in the BbCpu. */
#define XR(i) (16 + (i))
static void sync_out(A64 *a) { /* guest registers and flags to the BbCpu */
    for (int i = 0; i < 16; ++i) a64_str_uoff(a, 3, G[i], CPU, OFF(r) + 8 * (uint32_t)i);
    for (int i = 0; i < 16; ++i) a64_str_q(a, XR(i), CPU, OFF(v) + 32u * (uint32_t)i);
    a64_str_uoff(a, 3, FL, CPU, OFF(jf_flags));
    a64_str_uoff(a, 3, FRES, CPU, OFF(jf_res));
    a64_str_uoff(a, 3, FAB, CPU, OFF(jf_ab));
}
static void sync_in(A64 *a) {
    for (int i = 0; i < 16; ++i) a64_ldr_uoff(a, 3, G[i], CPU, OFF(r) + 8 * (uint32_t)i);
    for (int i = 0; i < 16; ++i) a64_ldr_q(a, XR(i), CPU, OFF(v) + 32u * (uint32_t)i);
    a64_ldr_uoff(a, 3, FL, CPU, OFF(jf_flags));
    a64_ldr_uoff(a, 3, FRES, CPU, OFF(jf_res));
    a64_ldr_uoff(a, 3, FAB, CPU, OFF(jf_ab));
}

/* Indirect branch cache, per thread (BbCpu): guest rip -> translated code. The index folds in
 * the bits above the low four, which are zero at the (16-byte aligned) function starts. */
enum { CACHE_BITS = 16 };
struct BbJitCacheEntry { uint64_t rip; void *code; };
static struct BbJitCacheEntry *cache_entry(BbCpu *cpu, uint64_t rip) {
    return &cpu->jit_cache[(rip ^ (rip >> 4)) & ((1u << CACHE_BITS) - 1)];
}

static void *cached_code(BbCpu *cpu) {
    const struct BbJitCacheEntry *entry = cache_entry(cpu, cpu->rip);
    return entry->rip == cpu->rip ? entry->code : NULL;
}

/* A guarded call's return reached from translated code through the marker's cache entry: the
 * guard's leave, then the translated code at the real return address if the cache has it. */
static void *jit_guard_return(BbCpu *cpu) {
    bbcpu_guard_return(cpu);
    return cached_code(cpu);
}

/* A host function reached from translated code through its cache entry (bbcpu_jit_run adds one
 * once it called the function, so never for the return hook's frame addresses): the call, then
 * the code at the return address if the cache has it. */
static void *jit_host_function(BbCpu *cpu) {
    bbcpu_call_host_at_rip(cpu);
    return cached_code(cpu);
}

/* fn(cpu, TB) in C from translated code: the guest's registers are stored in the BbCpu around
 * the call. */
static void host_call(A64 *a, const void *fn) {
    sync_out(a);
    a64_stp_pre(a, 29, 30, SP, -16);
    a64_mov(a, 1, 0, CPU);
    a64_mov(a, 1, 1, TB);
    a64_mov_imm(a, TA, (uint64_t)(uintptr_t)fn);
    a64_blr(a, TA);
    a64_ldp_post(a, 29, 30, SP, 16);
    sync_in(a);
}

/* After a host_call that returned the code to continue at in x0, or NULL: that code, with TA the
 * guest rip as an indirect exit leaves it (the code may be another host function's stub), else
 * the dispatcher. */
static void continue_at_result(A64 *a) {
    a64_cbz(a, 1, 0, 3);
    a64_ldr_uoff(a, 3, TA, CPU, OFF(rip));
    a64_br(a, 0);
    a64_b(a, (int32_t)(exit_stub - a64_here(a)));
}

/* A stub translated code calls (bl) to run fn(cpu, TB) with TA as the guest rip. */
static uint32_t *host_call_stub(A64 *a, const void *fn) {
    uint32_t *stub = a64_here(a);
    a64_str_uoff(a, 3, TA, CPU, OFF(rip));
    host_call(a, fn);
    a64_ret(a);
    return stub;
}

static int jit_init(void) {
    static int state; /* 0 unknown, 1 on, -1 off */
    if (state) return state > 0;
    pthread_mutex_lock(&jit_lock);
    if (!state) {
        const char *env = getenv("BB_JIT");
        void *p = (env && env[0] == '0') ? MAP_FAILED
                : mmap(NULL, CODE_SIZE, PROT_READ | PROT_WRITE | PROT_EXEC, MAP_PRIVATE | MAP_ANON | MAP_JIT, -1, 0);
        if (p == MAP_FAILED) {
            state = -1;
            printf("CPU: JIT off; guest code is interpreted\n");
        } else {
            code_base = p;
            code_end = code_base + CODE_SIZE / 4;
            pthread_jit_write_protect_np(0);
            A64 a = {code_base, code_base, code_end};
            /* enter(cpu, code): save the host's callee-saved registers, load the guest's. */
            enter_stub = a64_here(&a);
            a64_stp_pre(&a, 29, 30, SP, -96);
            a64_add_imm(&a, 1, 29, SP, 0, 0);
            a64_stp(&a, 19, 20, SP, 16);
            a64_stp(&a, 21, 22, SP, 32);
            a64_stp(&a, 23, 24, SP, 48);
            a64_stp(&a, 25, 26, SP, 64);
            a64_stp(&a, 27, 28, SP, 80);
            a64_mov(&a, 1, CPU, 0);
            sync_in(&a);
            a64_br(&a, 1);
            /* exit: store the guest's registers, return to bbcpu_jit_run. */
            exit_stub = a64_here(&a);
            sync_out(&a);
            a64_ldp(&a, 19, 20, SP, 16);
            a64_ldp(&a, 21, 22, SP, 32);
            a64_ldp(&a, 23, 24, SP, 48);
            a64_ldp(&a, 25, 26, SP, 64);
            a64_ldp(&a, 27, 28, SP, 80);
            a64_ldp_post(&a, 29, 30, SP, 96);
            a64_ret(&a);
            /* TA = guest rip, TB = the BbInsn */
            fallback_stub = host_call_stub(&a, (const void *)&jit_fallback);
            /* TA = guest rip, TB = the native function */
            native_stub = host_call_stub(&a, (const void *)&jit_native);
            /* TA = the function's guest address, TB = the recompiled function */
            recomp_stub = host_call_stub(&a, (const void *)&jit_recomp);
            /* TA = the probed address */
            probe_stub = host_call_stub(&a, (const void *)&bbcpu_run_probe);
            /* The guard marker's cache entry (bbcpu_jit_run sets it): the code jit_guard_return
             * finds, else the dispatcher at the rip it set. */
            guard_return_stub = a64_here(&a);
            host_call(&a, (const void *)&jit_guard_return);
            continue_at_result(&a);
            /* A host function's cache entry: TA = the function. */
            host_function_stub = a64_here(&a);
            a64_str_uoff(&a, 3, TA, CPU, OFF(rip));
            host_call(&a, (const void *)&jit_host_function);
            continue_at_result(&a);
            code_next = a64_here(&a);
            pthread_jit_write_protect_np(1);
            sys_icache_invalidate(code_base, (size_t)(code_next - code_base) * 4);
            state = 1;
            printf("CPU: guest code translated to arm64 (JIT; BB_JIT=0: interpreter)\n");
        }
    }
    pthread_mutex_unlock(&jit_lock);
    return state > 0;
}

/* ---- translation ---- */

/* A misaligned guest access, emitted after the block (out of the hot path): the branch to it,
 * the access (a plain one between barriers), and where it continues. */
typedef struct {
    uint32_t *branch, *resume;
    uint8_t store, lg, rt, addr;
    uint64_t rip;
} SlowAccess;
enum { MAX_SLOW = 256 };

typedef struct {
    A64 a;
    const BbBlock *block;
    uint64_t rip; /* of the instruction being translated */
    SlowAccess slow[MAX_SLOW];
    int slow_count;
    InstructionRange *fault_map;
    size_t fault_count;
} Tx;

/* A direct exit: a branch the dispatcher patches to the target's code once it has it
 * (block chaining); until then it goes to the slow path that returns to the dispatcher. */
static void exit_to(Tx *t, uint64_t rip) {
    A64 *a = &t->a;
    uint32_t *site = a64_here(a);
    a64_b(a, 1);
    a64_mov_imm(a, TA, rip);
    a64_str_uoff(a, 3, TA, CPU, OFF(rip));
    a64_adr(a, TB, (int32_t)((site - a64_here(a)) * 4));
    a64_str_uoff(a, 3, TB, CPU, OFF(jit_link));
    a64_b(a, (int32_t)(exit_stub - a64_here(a)));
}
/* An indirect exit: the target looked up in the thread's cache, else the dispatcher. */
static int deny_icache;
static void exit_to_reg(Tx *t, int reg) {
    A64 *a = &t->a;
    if (reg != TA) a64_mov(a, 1, TA, reg);
    if (deny_icache) {
        a64_str_uoff(a, 3, TA, CPU, OFF(rip));
        a64_b(a, (int32_t)(exit_stub - a64_here(a)));
        return;
    }
    a64_ldr_uoff(a, 3, TB, CPU, OFF(jit_cache));
    a64_logic(a, 2, 0, 1, T4, TA, TA, 1, 4); /* eor T4, TA, TA, lsr #4 */
    a64_ubfx(a, 1, T4, T4, 0, CACHE_BITS);
    a64_arith(a, 0, 1, TB, TB, T4, 0, 4);
    a64_ldp(a, T4, T5, TB, 0);
    a64_subs(a, 1, XZR, T4, TA);
    a64_bcond(a, CC_NE, 3);
    /* An empty cache entry has both fields zero. A guest null branch must go through the
     * dispatcher rather than branch to host address zero. */
    a64_cbz(a, 1, T5, 2);
    a64_br(a, T5);
    a64_str_uoff(a, 3, TA, CPU, OFF(rip));
    /* On a null target, retain the guest branch site for the dispatcher's report. This
     * slow path is also reached for a miss in the indirect branch cache. */
    uint32_t *non_null = a64_here(a);
    a64_cbnz(a, 1, TA, 0);
    a64_mov_imm(a, TB, t->rip);
    a64_str_uoff(a, 3, TB, CPU, OFF(jit_link));
    a64_patch_bcond(non_null, a64_here(a));
    a64_b(a, (int32_t)(exit_stub - a64_here(a)));
}

static void fallback(Tx *t, const BbInsn *in) {
    A64 *a = &t->a;
    a64_mov_imm(a, TA, t->rip);
    a64_mov_imm(a, TB, (uint64_t)(uintptr_t)in);
    a64_bl(a, (int32_t)(fallback_stub - a64_here(a)));
}

/* Out-of-line interpretation of the instruction being translated, for the cases its fast path
 * leaves out (a zero divisor, a misaligned atomic, ...). The branches to it come before the fast
 * path changes any guest state; slow_end emits it after the fast path. */
typedef struct { uint32_t *branch[6]; int count; } Slow;
static void slow_if(Tx *t, Slow *s, int cc) { s->branch[s->count++] = a64_here(&t->a); a64_bcond(&t->a, cc, 0); }
static void slow_cbz(Tx *t, Slow *s, int sf, int reg) { s->branch[s->count++] = a64_here(&t->a); a64_cbz(&t->a, sf, reg, 0); }
static void slow_cbnz(Tx *t, Slow *s, int sf, int reg) { s->branch[s->count++] = a64_here(&t->a); a64_cbnz(&t->a, sf, reg, 0); }
static void slow_end(Tx *t, Slow *s, const BbInsn *in) {
    if (!s->count) return;
    uint32_t *done = a64_here(&t->a);
    a64_b(&t->a, 0);
    for (int i = 0; i < s->count; ++i) a64_patch_bcond(s->branch[i], a64_here(&t->a)); /* b.cond, cbz, cbnz: imm19 */
    fallback(t, in);
    a64_patch_b(done, a64_here(&t->a));
}

static int log2_scale(int scale) { return scale == 8 ? 3 : scale == 4 ? 2 : scale == 2 ? 1 : 0; }

/* Effective address of a memory operand into `rd` (TA by default). */
static void address(Tx *t, const BbInsn *in, const BbOp *op, int rd) {
    A64 *a = &t->a;
    if (op->base == 0xfe) { /* rip-relative: a constant */
        a64_mov_imm(a, rd, t->rip + in->length + (uint64_t)op->disp);
        if (op->index != 0xff) a64_arith(a, 0, 1, rd, rd, G[op->index], 0, log2_scale(op->scale));
    } else {
        int have = 0;
        if (op->base != 0xff && op->index != 0xff) {
            a64_arith(a, 0, 1, rd, G[op->base], G[op->index], 0, log2_scale(op->scale));
            have = 1;
        } else if (op->base != 0xff) {
            if (op->disp >= 0 && op->disp < 4096 && !op->segment) {
                a64_add_imm(a, 1, rd, G[op->base], (uint32_t)op->disp, 0);
                return;
            }
            a64_mov(a, 1, rd, G[op->base]);
            have = 1;
        } else if (op->index != 0xff) {
            a64_lsl_imm(a, 1, rd, G[op->index], log2_scale(op->scale));
            have = 1;
        }
        if (!have) a64_mov_imm(a, rd, (uint64_t)op->disp);
        else if (op->disp > 0 && op->disp < 4096) a64_add_imm(a, 1, rd, rd, (uint32_t)op->disp, 0);
        else if (op->disp < 0 && op->disp > -4096) a64_sub_imm(a, 1, rd, rd, (uint32_t)-op->disp, 0);
        else if (op->disp) {
            a64_mov_imm(a, TB, (uint64_t)op->disp);
            a64_add(a, 1, rd, rd, TB);
        }
    }
    if (op->segment) {
        a64_ldr_uoff(a, 3, TB, CPU, op->segment == 1 ? OFF(fs_base) : OFF(gs_base));
        a64_add(a, 1, rd, rd, TB);
    }
}

static int size_log2(int size) { return size == 8 ? 3 : size == 4 ? 2 : size == 2 ? 1 : 0; }

/* Guest loads acquire and stores release (x86 store ordering; cpu_internal.h), with ldapr/stlr.
 * Those fault when misaligned across 16 bytes, which x86 code does freely: a misaligned address
 * takes a plain access and a barrier instead (an inline check; the fault handler costs ~us). */
static int plain_access; /* set by the memory operand forms below for stack operands */
/* Bisection switches: BB_JIT_ALIGN_CHECK=0 (ordered accesses only, the fault handler fixes
 * misaligned ones), BB_JIT_PLAIN_PUSH=0 (push/pop/call/ret ordered too). */
static int env_flag(const char *name, int fallback) {
    const char *env = getenv(name);
    return env ? env[0] == '1' : fallback;
}
static int align_check(void) {
    static int check = -1;
    if (check < 0) check = env_flag("BB_JIT_ALIGN_CHECK", 1);
    return check;
}
/* The ordered access, or (misaligned: tst/b.ne) a branch to its out-of-line plain version. */
static void ordered_access(Tx *t, int store_, int size, int rt, int addr) {
    A64 *a = &t->a;
    const int lg = size_log2(size);
    const uint32_t op = store_ ? 0x089ffc00u : 0x38bfc000u; /* stlr / ldapr */
    if (size > 1 && align_check()) {
        if (t->slow_count < MAX_SLOW) {
            a64_logic_imm(a, 3, 1, XZR, addr, 0, lg); /* tst addr, #size-1 */
            SlowAccess *s = &t->slow[t->slow_count++];
            s->branch = a64_here(a);
            a64_bcond(a, CC_NE, 0);
            a64_emit(a, (uint32_t)lg << 30 | op | (uint32_t)addr << 5 | (uint32_t)rt);
            s->resume = a64_here(a);
            s->store = (uint8_t)store_;
            s->lg = (uint8_t)lg;
            s->rt = (uint8_t)rt;
            s->addr = (uint8_t)addr;
            s->rip = t->rip;
            return;
        }
        /* The block's table is full: inline, as before. */
        a64_logic_imm(a, 3, 1, XZR, addr, 0, lg);
        a64_bcond(a, CC_NE, 3);
        a64_emit(a, (uint32_t)lg << 30 | op | (uint32_t)addr << 5 | (uint32_t)rt);
        a64_b(a, 3);
        if (store_) { a64_dmb_ish(a); a64_str_uoff(a, lg, rt, addr, 0); }
        else { a64_ldr_uoff(a, lg, rt, addr, 0); a64_emit(a, 0xd50339bfu); }
        return;
    }
    a64_emit(a, (uint32_t)lg << 30 | op | (uint32_t)addr << 5 | (uint32_t)rt);
}
/* The misaligned accesses' plain versions, after the block's code. */
static void emit_slow_accesses(Tx *t) {
    A64 *a = &t->a;
    for (int i = 0; i < t->slow_count; ++i) {
        const SlowAccess *s = &t->slow[i];
        const uint32_t *start = a64_here(a);
        a64_patch_bcond(s->branch, a64_here(a));
        if (s->store) {
            a64_dmb_ish(a);
            a64_str_uoff(a, s->lg, s->rt, s->addr, 0);
        } else {
            a64_ldr_uoff(a, s->lg, s->rt, s->addr, 0);
            a64_emit(a, 0xd50339bfu); /* dmb ishld */
        }
        a64_b(a, (int32_t)(s->resume - a64_here(a)));
        if (t->fault_map)
            t->fault_map[t->fault_count++] = (InstructionRange){start, a64_here(a), s->rip};
    }
    t->slow_count = 0;
}
/* BB_JIT_UNORDERED=1 (measurement only, unsafe for threads): plain loads and stores. */
static int unordered(void) {
    static int on = -1;
    if (on < 0) on = env_flag("BB_JIT_UNORDERED", 0);
    return on;
}
static void load(Tx *t, int size, int rt, int addr) {
    if (plain_access || unordered()) { a64_ldr_uoff(&t->a, size_log2(size), rt, addr, 0); return; }
    ordered_access(t, 0, size, rt, addr);
}
static void store(Tx *t, int size, int rt, int addr) {
    if (plain_access || unordered()) { a64_str_uoff(&t->a, size_log2(size), rt, addr, 0); return; }
    ordered_access(t, 1, size, rt, addr);
}
/* BB_JIT_STACK_PLAIN=1: rsp-based operands without ordering (rbp is a general register in
 * optimized code). Off by default: the game shares stack objects between threads (with it on, a
 * job's assertion fired in the level, 2026-10-08). */
static int stack_operand(const BbOp *op) {
    static int enabled = -1;
    if (enabled < 0) { const char *env = getenv("BB_JIT_STACK_PLAIN"); enabled = env && env[0] == '1'; }
    return enabled && op->type == OP_MEM && !op->segment && op->base == RSP;
}

/* BB_JIT_WATCH=address:displacement[:displacement...]: selected scalar MOV accesses.
 * The loaded/stored value is recorded directly; no second guest read or host call. */
static void watch_move(Tx *t, const BbInsn *in, const BbOp *op, int rt, int addr, int store_) {
    static int initialized;
    static uint64_t target, load_site;
    static int64_t displacements[8];
    static size_t count;
    if (!initialized) {
        const char *env = getenv("BB_JIT_WATCH");
        if (env) {
            char *end;
            target = strtoull(env, &end, 0);
            while (*end == ':' && count < 8) {
                const char *start = end + 1;
                const int64_t displacement = strtoll(start, &end, 0);
                if (end == start) break;
                displacements[count++] = displacement;
            }
        }
        const char *site = getenv("BB_JIT_WATCH_LOAD");
        if (site) load_site = strtoull(site, NULL, 0);
        initialized = 1; /* translation holds jit_lock */
    }
    if (!target || in->mnemonic != M(MOV) || op->size != 8) return;
    size_t i;
    for (i = 0; i < count && op->disp != displacements[i]; ++i) {}
    if (i == count) return;
    A64 *a = &t->a;
    /* Scalar MOV has no live scratch values in T3-T5/TB at this point. Preserve NZCV. */
    a64_mov(a, 1, T3, addr);
    a64_mov_imm(a, T4, target);
    a64_logic(a, 2, 0, 1, T4, T3, T4, 0, 0); /* eor */
    uint32_t *skip = NULL;
    if (store_ || t->rip != load_site) { skip = a64_here(a); a64_cbnz(a, 1, T4, 0); }
    a64_ldr_uoff(a, 3, TB, CPU, OFF(watch));
    uint32_t *no_watch = a64_here(a); a64_cbz(a, 1, TB, 0);
    a64_ldr_uoff(a, 3, T4, TB, offsetof(BbWatch, count));
    a64_logic_imm(a, 0, 1, T5, T4, 0, 6); /* count & 63 */
    a64_add_imm(a, 1, T3, TB, offsetof(BbWatch, records), 0);
    a64_arith(a, 0, 1, T3, T3, T5, 0, 6); /* record = records + index * 64 */
    a64_str_uoff(a, 3, XZR, T3, offsetof(BbWatchRecord, sequence));
    a64_dmb_ish(a); /* invalidate the old slot before publishing new payload stores */
    a64_str_uoff(a, 3, addr, T3, offsetof(BbWatchRecord, address));
    a64_str_uoff(a, 3, rt, T3, offsetof(BbWatchRecord, value));
    a64_mov_imm(a, T5, t->rip);
    a64_str_uoff(a, 3, T5, T3, offsetof(BbWatchRecord, rip));
    a64_mrs_cntvct(a, T5);
    a64_str_uoff(a, 3, T5, T3, offsetof(BbWatchRecord, tick));
    a64_add_imm(a, 1, T4, T4, 1, 0);
    a64_mov(a, 1, T5, T4);
    if (store_) {
        a64_mov_imm(a, T5, UINT64_C(1) << 63);
        a64_orr(a, 1, T5, T5, T4);
    }
    a64_add_imm(a, 1, T3, T3, offsetof(BbWatchRecord, sequence), 0);
    a64_emit(a, 0xc89ffc00u | (uint32_t)T3 << 5 | T5); /* stlr sequence */
    a64_emit(a, 0xc89ffc00u | (uint32_t)TB << 5 | T4); /* stlr count (offset 0) */
    if (skip) a64_patch_bcond(skip, a64_here(a));
    a64_patch_bcond(no_watch, a64_here(a));
}

/* Value of an operand (zero-extended to its size) in a register; may be the guest register
 * itself (64-bit or 32-bit reads, `*direct` set). */
static int read_op(Tx *t, const BbInsn *in, const BbOp *op, int size, int tmp, int *direct) {
    A64 *a = &t->a;
    if (direct) *direct = 0;
    switch (op->type) {
    case OP_REG:
        if (op->high8) { a64_ubfx(a, 1, tmp, G[op->reg], 8, 8); return tmp; }
        if (size >= 4) { if (direct) *direct = 1; if (size == 8 || direct) return G[op->reg]; a64_mov(a, 0, tmp, G[op->reg]); return tmp; }
        a64_ubfx(a, 1, tmp, G[op->reg], 0, size * 8);
        return tmp;
    case OP_MEM:
        address(t, in, op, TA);
        plain_access = stack_operand(op);
        load(t, size, tmp, TA);
        plain_access = 0;
        watch_move(t, in, op, tmp, TA, 0);
        return tmp;
    case OP_IMM:
        a64_mov_imm(a, tmp, (uint64_t)op->disp & bb_mask(size));
        return tmp;
    default:
        return -1;
    }
}

/* Writes the low `size` bytes of `src` to a register or memory operand (x86 merge rules). */
static void write_op(Tx *t, const BbInsn *in, const BbOp *op, int size, int src) {
    A64 *a = &t->a;
    if (op->type == OP_REG) {
        const int d = G[op->reg];
        if (op->high8) a64_bfi(a, 1, d, src, 8, 8);
        else if (size == 8) { if (d != src) a64_mov(a, 1, d, src); }
        else if (size == 4) a64_mov(a, 0, d, src);
        else a64_bfi(a, 1, d, src, 0, size * 8);
    } else {
        address(t, in, op, TA);
        plain_access = stack_operand(op);
        store(t, size, src, TA);
        plain_access = 0;
        watch_move(t, in, op, src, TA, 1);
    }
}

/* Stores NZCV (after a flag-setting instruction) into x8; invert: x86 CF = arm64 C (adds). */
static void save_nzcv(Tx *t, int invert_c) {
    A64 *a = &t->a;
    a64_mrs_nzcv(a, TB);
    if (invert_c) a64_eor_bit(a, TB, TB, 29);
    a64_lsr_imm(a, 1, TB, TB, 28);
    a64_bfi(a, 1, FL, TB, 28, 4);
}

/* x86 condition (0-15, jcc order) as an arm64 one on x8's NZCV; -1 for parity. */
static int arm_cc(int cc) {
    static const int map[16] = {CC_VS, CC_VC, CC_LO, CC_HS, CC_EQ, CC_NE, CC_LS, CC_HI,
                                CC_MI, CC_PL, -1, -1, CC_LT, CC_GE, CC_LE, CC_GT};
    return map[cc];
}

/* Mnemonic of a jcc/setcc/cmovcc -> condition (filled from the interpreter's table order). */
static int8_t cc_of[ZYDIS_MNEMONIC_MAX_VALUE + 1];
static int8_t cc_kind[ZYDIS_MNEMONIC_MAX_VALUE + 1];
static void init_cc(void) {
    static const int jcc[16] = {M(JO), M(JNO), M(JB), M(JNB), M(JZ), M(JNZ), M(JBE), M(JNBE),
                                M(JS), M(JNS), M(JP), M(JNP), M(JL), M(JNL), M(JLE), M(JNLE)};
    static const int setcc[16] = {M(SETO), M(SETNO), M(SETB), M(SETNB), M(SETZ), M(SETNZ),
                                  M(SETBE), M(SETNBE), M(SETS), M(SETNS), M(SETP), M(SETNP),
                                  M(SETL), M(SETNL), M(SETLE), M(SETNLE)};
    static const int cmovcc[16] = {M(CMOVO), M(CMOVNO), M(CMOVB), M(CMOVNB), M(CMOVZ),
                                   M(CMOVNZ), M(CMOVBE), M(CMOVNBE), M(CMOVS), M(CMOVNS),
                                   M(CMOVP), M(CMOVNP), M(CMOVL), M(CMOVNL), M(CMOVLE),
                                   M(CMOVNLE)};
    memset(cc_of, -1, sizeof(cc_of));
    for (int i = 0; i < 16; ++i) {
        cc_of[jcc[i]] = (int8_t)i; cc_kind[jcc[i]] = 1;
        cc_of[setcc[i]] = (int8_t)i; cc_kind[setcc[i]] = 2;
        cc_of[cmovcc[i]] = (int8_t)i; cc_kind[cmovcc[i]] = 3;
    }
}

/* The operation of add/or/adc/sbb/and/sub/xor/cmp/test on zero-extended va and vb: the result
 * in T2 and, when live, the flags. Clobbers T3, T4 and TB; NZCV is left as the operation set it
 * (its Z holds for the arithmetic ones whether or not the flags are live). */
static int alu_core(Tx *t, int m, int size, int va, int vb, int flags_live) {
    A64 *a = &t->a;
    const int sf = size == 8;
    const int arith = m == M(ADD) || m == M(SUB) || m == M(CMP) || m == M(ADC) || m == M(SBB);
    int r = T2;
    if (size >= 4) {
        switch (m) {
        case M(ADD): a64_adds(a, sf, r, va, vb); break;
        case M(SUB): case M(CMP): a64_subs(a, sf, r, va, vb); break;
        case M(ADC):
            a64_eor_bit(a, TB, FL, 29); /* C = x86 CF */
            a64_msr_nzcv(a, TB);
            a64_adc(a, sf, 1, r, va, vb);
            break;
        case M(SBB):
            a64_msr_nzcv(a, FL); /* C = !CF: subtract with borrow */
            a64_sbc(a, sf, 1, r, va, vb);
            break;
        case M(AND): case M(TEST): a64_ands(a, sf, r, va, vb); break;
        case M(OR): a64_orr(a, sf, r, va, vb); if (flags_live) a64_ands(a, sf, XZR, r, r); break;
        default: a64_eor(a, sf, r, va, vb); if (flags_live) a64_ands(a, sf, XZR, r, r); break;
        }
    } else {
        /* 8/16-bit: operands at the top of a 32-bit register give the right NZCV. */
        const int shift = 32 - size * 8;
        if (arith) {
            a64_lsl_imm(a, 0, T3, va, shift);
            a64_lsl_imm(a, 0, T4, vb, shift);
            if (m == M(ADD)) a64_adds(a, 0, r, T3, T4); else a64_subs(a, 0, r, T3, T4);
            a64_lsr_imm(a, 0, r, r, shift);
        } else {
            if (m == M(AND) || m == M(TEST)) a64_and(a, 0, r, va, vb);
            else if (m == M(OR)) a64_orr(a, 0, r, va, vb);
            else a64_eor(a, 0, r, va, vb);
            if (flags_live) {
                a64_lsl_imm(a, 0, T3, r, shift);
                a64_ands(a, 0, XZR, T3, T3);
            }
        }
    }
    if (flags_live) {
        /* x8 keeps C = !CF. adds/adcs set C = CF: inverted. subs/sbcs set C = !borrow = !CF.
         * Logic ops clear CF, so C must be 1 where ands left 0: inverted too. */
        const int invert = m == M(ADD) || m == M(ADC) || !arith;
        save_nzcv(t, invert);
        a64_mov(a, 1, FRES, r);
        if (arith) a64_eor(a, 1, FAB, va, vb);
    }
    return r;
}

/* add/or/adc/sbb/and/sub/xor/cmp/test (locked forms: atomic_rmw). Returns 0 when not translated. */
static int alu(Tx *t, const BbInsn *in, int flags_live) {
    const int m = in->mnemonic;
    const BbOp *dst = &in->op[0], *src = &in->op[1];
    const int size = dst->size;
    if (in->lock) return 0;
    if ((m == M(ADC) || m == M(SBB)) && size < 4) return 0;
    const int va = read_op(t, in, dst, size, T0, NULL);
    const int vb = read_op(t, in, src, size, T1, NULL);
    const int r = alu_core(t, m, size, va, vb, flags_live);
    if (m != M(CMP) && m != M(TEST)) write_op(t, in, dst, size, r);
    return 1;
}

/* inc/dec/neg/not of the zero-extended va: the result in T2 and, when live, the flags (inc/dec
 * keep CF). Clobbers T3, T4, T5 and TB. */
static int unary_core(Tx *t, int m, int size, int va, int flags_live) {
    A64 *a = &t->a;
    const int sf = size == 8;
    const int r = T2;
    if (m == M(NOT)) {
        a64_mvn(a, sf, r, va);
        return r;
    }
    if (size < 4) {
        const int shift = 32 - size * 8;
        a64_lsl_imm(a, 0, T3, va, shift);
        if (m == M(NEG)) a64_subs(a, 0, r, XZR, T3);
        else {
            a64_movz(a, 0, T4, 1, 0);
            a64_lsl_imm(a, 0, T4, T4, shift);
            if (m == M(INC)) a64_adds(a, 0, r, T3, T4); else a64_subs(a, 0, r, T3, T4);
        }
        a64_lsr_imm(a, 0, r, r, shift);
    } else if (m == M(NEG)) {
        a64_subs(a, sf, r, XZR, va);
    } else {
        a64_movz(a, 0, T4, 1, 0);
        if (m == M(INC)) a64_adds(a, sf, r, va, T4); else a64_subs(a, sf, r, va, T4);
    }
    if (flags_live) {
        if (m == M(NEG)) save_nzcv(t, 0);
        else {
            /* inc/dec keep CF */
            a64_ubfx(a, 1, T5, FL, 29, 1);
            save_nzcv(t, m == M(INC));
            a64_bfi(a, 1, FL, T5, 29, 1);
        }
        a64_mov(a, 1, FRES, r);
        if (m == M(NEG)) a64_mov(a, 1, FAB, va); /* 0 ^ a */
        else a64_eor_bit(a, FAB, va, 0);          /* a ^ 1 */
    }
    return r;
}

/* inc/dec/neg/not on registers and memory (locked forms: atomic_rmw). */
static int unary(Tx *t, const BbInsn *in, int flags_live) {
    const BbOp *dst = &in->op[0];
    const int size = dst->size;
    if (in->lock) return 0;
    const int va = read_op(t, in, dst, size, T0, NULL);
    write_op(t, in, dst, size, unary_core(t, in->mnemonic, size, va, flags_live));
    return 1;
}

/* shl/shr/sar by an immediate count (register counts are interpreted). */
static int shift_imm(Tx *t, const BbInsn *in, int flags_live) {
    A64 *a = &t->a;
    const int m = in->mnemonic;
    const BbOp *dst = &in->op[0];
    const int size = dst->size, bits = size * 8;
    if (dst->high8 || size < 4) return 0;
    unsigned count = in->count > 1 ? (unsigned)in->op[1].disp : 1;
    if (in->count > 1 && in->op[1].type != OP_IMM) return 0;
    count &= size == 8 ? 63 : 31;
    if (!count) return 1; /* no change, flags kept */
    const int sf = size == 8;
    const int va = read_op(t, in, dst, size, T0, NULL);
    const int r = T2;
    if (m == M(SHL)) a64_lsl_imm(a, sf, r, va, (int)count);
    else if (m == M(SHR)) a64_lsr_imm(a, sf, r, va, (int)count);
    else a64_asr_imm(a, sf, r, va, (int)count);
    if (flags_live) {
        /* N, Z from the result; C = !(last bit out); V: shl count 1 = MSB(r) ^ CF, shr = MSB(a). */
        a64_ands(a, sf, XZR, r, r);
        a64_mrs_nzcv(a, TB);                      /* N Z, C=0 V=0 */
        if (m == M(SHL)) a64_ubfx(a, 1, T3, va, bits - (int)count, 1);
        else a64_ubfx(a, 1, T3, va, (int)count - 1, 1);
        a64_eor_bit(a, T4, T3, 0);                /* !CF */
        a64_logic(a, 1, 0, 1, TB, TB, T4, 0, 29); /* TB |= !CF << 29 */
        if (count == 1) {
            if (m == M(SHL)) {
                a64_ubfx(a, 1, T4, r, bits - 1, 1);
                a64_eor(a, 1, T4, T4, T3);
            } else if (m == M(SHR)) {
                a64_ubfx(a, 1, T4, va, bits - 1, 1);
            } else {
                a64_movz(a, 0, T4, 0, 0);
            }
            a64_logic(a, 1, 0, 1, TB, TB, T4, 0, 28);
        }
        a64_lsr_imm(a, 1, TB, TB, 28);
        a64_bfi(a, 1, FL, TB, 28, 4);
        a64_mov(a, 1, FRES, r);
    }
    write_op(t, in, dst, size, r);
    return 1;
}

/* ---- SSE/AVX: guest vector registers stay in the BbCpu; v0-v3 are temporaries ---- */

#define VOFF(r) (OFF(v) + 32u * (uint32_t)(r))

/* Guest register `reg`'s low (half 0, a host register) or high 128 bits into v`dst`: `bytes`
 * 16, or 8/4 with the rest zeroed (as a scalar load). */
static void vget(Tx *t, int reg, int half, int dst, int bytes) {
    A64 *a = &t->a;
    if (half) { a64_ldr_v(a, bytes == 4 ? 2 : bytes == 8 ? 3 : 4, dst, CPU, VOFF(reg) + 16u); return; }
    if (bytes == 16) { if (dst != XR(reg)) a64_vmov(a, dst, XR(reg)); }
    else a64_fmov_reg(a, bytes == 8, dst, XR(reg));
}
/* v`src` as guest register `reg`'s low (half 0) or high 128 bits. */
static void vput(Tx *t, int reg, int half, int src) {
    if (half) a64_str_v(&t->a, 4, src, CPU, VOFF(reg) + 16u);
    else if (src != XR(reg)) a64_vmov(&t->a, XR(reg), src);
}

static int vreg(const BbOp *op) { return op->type == OP_REG && op->kind == RK_VEC; }

/* Vector memory accesses ordered like the interpreter's (barriers); BB_JIT_VEC_PLAIN=1: none. */
static int vec_ordered(void) {
    static int on = -1;
    if (on < 0) on = !env_flag("BB_JIT_VEC_PLAIN", 0);
    return on;
}

/* Loads `bytes` (4, 8, 16) of operand `op` (+`half` * 16 for the upper 128 bits) into v`dst`.
 * Scalar loads zero the rest of the q register (arm64 does that by itself). */
static int vload(Tx *t, const BbInsn *in, const BbOp *op, int bytes, int half, int dst) {
    const int size = bytes == 4 ? 2 : bytes == 8 ? 3 : 4;
    if (vreg(op)) { vget(t, op->reg, half, dst, bytes); return 1; }
    if (op->type != OP_MEM) return 0;
    address(t, in, op, TA);
    if (half) a64_add_imm(&t->a, 1, TA, TA, 16, 0);
    a64_ldr_v(&t->a, size, dst, TA, 0);
    if (vec_ordered()) a64_emit(&t->a, 0xd50339bfu); /* dmb ishld: x86 load ordering */
    return 1;
}
/* Stores q`src` as the low (half 0) or high (half 1) 128 bits of a register; VEX 128-bit
 * results zero bits 128-255. */
static void vstore_reg(Tx *t, const BbInsn *in, int reg, int src, int half, int zero_upper) {
    vput(t, reg, half, src);
    if (zero_upper && in->vex && !half) {
        a64_str_uoff(&t->a, 3, XZR, CPU, VOFF(reg) + 16);
        a64_str_uoff(&t->a, 3, XZR, CPU, VOFF(reg) + 24);
    }
}
static void vstore_mem(Tx *t, const BbInsn *in, const BbOp *op, int bytes, int half, int src) {
    const int size = bytes == 4 ? 2 : bytes == 8 ? 3 : 4;
    address(t, in, op, TA);
    if (half) a64_add_imm(&t->a, 1, TA, TA, 16, 0);
    if (vec_ordered()) a64_dmb_ish(&t->a); /* x86 store ordering */
    a64_str_v(&t->a, size, src, TA, 0);
}

typedef struct { const BbOp *dst, *s1, *s2, *imm; } VOps;
static VOps vops3(const BbInsn *in) {
    VOps o = {&in->op[0], NULL, NULL, NULL};
    if (in->vex && in->count >= 3 && in->op[2].type != OP_IMM) {
        o.s1 = &in->op[1]; o.s2 = &in->op[2];
        if (in->count >= 4 && in->op[3].type == OP_IMM) o.imm = &in->op[3];
    } else {
        o.s1 = &in->op[0]; o.s2 = &in->op[1];
        if (in->count >= 3 && in->op[2].type == OP_IMM) o.imm = &in->op[2];
    }
    return o;
}

/* x86 min/max: (a < b) ? a : b and (a > b) ? a : b, the second operand on NaN. v0 = a, v1 = b. */
static void minmax(Tx *t, int is_max, int dbl) {
    if (is_max) a64_fcmgt_v(&t->a, dbl, 2, 0, 1); /* a > b */
    else a64_fcmgt_v(&t->a, dbl, 2, 1, 0);        /* b > a */
    a64_bsl(&t->a, 2, 0, 1);
    a64_vmov(&t->a, 0, 2);
}

static int vector(Tx *t, const BbInsn *in, int flags_live) {
    A64 *a = &t->a;
    const int m = in->mnemonic;
    const int wide = in->vex && in->vl == 32;
    switch (m) {
    /* full-width moves */
    case M(MOVAPS): case M(MOVUPS): case M(MOVAPD): case M(MOVUPD): case M(MOVDQA): case M(MOVDQU):
    case M(VMOVAPS): case M(VMOVUPS): case M(VMOVAPD): case M(VMOVUPD): case M(VMOVDQA): case M(VMOVDQU):
    case M(LDDQU): case M(VLDDQU): case M(MOVNTPS): case M(VMOVNTPS): case M(MOVNTDQ): case M(VMOVNTDQ): {
        const BbOp *dst = &in->op[0], *src = &in->op[1];
        for (int h = 0; h <= wide; ++h) {
            if (!vload(t, in, src, 16, h, 0)) return 0;
            if (vreg(dst)) vstore_reg(t, in, dst->reg, 0, h, !wide);
            else vstore_mem(t, in, dst, 16, h, 0);
        }
        return 1;
    }
    case M(MOVSS): case M(VMOVSS): case M(MOVSD): case M(VMOVSD): {
        const int bytes = (m == M(MOVSS) || m == M(VMOVSS)) ? 4 : 8, es = bytes;
        const BbOp *dst = &in->op[0];
        if (!vreg(dst) && dst->type != OP_MEM) return 0;
        if (dst->type == OP_MEM) { /* store the low element */
            if (!vload(t, in, &in->op[in->count - 1], bytes, 0, 0)) return 0;
            vstore_mem(t, in, dst, bytes, 0, 0);
            return 1;
        }
        if (in->count == 3) { /* VEX reg form: element from src2, the rest from src1 */
            vload(t, in, &in->op[1], 16, 0, 0);
            vload(t, in, &in->op[2], 16, 0, 1);
            a64_ins_elem(a, es, 0, 0, 1, 0);
            vstore_reg(t, in, dst->reg, 0, 0, 1);
            return 1;
        }
        const BbOp *src = &in->op[1];
        if (src->type == OP_MEM) { /* load: the rest zeroed */
            vload(t, in, src, bytes, 0, 0);
            vstore_reg(t, in, dst->reg, 0, 0, 1);
            return 1;
        }
        if (!vreg(src)) return 0;
        vload(t, in, dst, 16, 0, 0); /* legacy reg form: merge the element */
        vload(t, in, src, 16, 0, 1);
        a64_ins_elem(a, es, 0, 0, 1, 0);
        vstore_reg(t, in, dst->reg, 0, 0, 1);
        return 1;
    }
    case M(MOVD): case M(VMOVD): case M(MOVQ): case M(VMOVQ): {
        const int bytes = (m == M(MOVD) || m == M(VMOVD)) ? 4 : 8, sf = bytes == 8;
        const BbOp *dst = &in->op[0], *src = &in->op[1];
        if (vreg(dst)) {
            if (src->type == OP_REG && src->kind == RK_GPR) {
                if (src->high8) return 0;
                const int v = read_op(t, in, src, bytes, T0, NULL);
                a64_fmov_from_gpr(a, sf, 0, v); /* zeroes the rest */
            } else if (!vload(t, in, src, bytes, 0, 0)) return 0;
            vput(t, dst->reg, 0, 0);
            if (in->vex) { a64_str_uoff(a, 3, XZR, CPU, VOFF(dst->reg) + 16); a64_str_uoff(a, 3, XZR, CPU, VOFF(dst->reg) + 24); }
            return 1;
        }
        if (!vreg(src)) return 0;
        if (dst->type == OP_MEM) { vload(t, in, src, bytes, 0, 0); vstore_mem(t, in, dst, bytes, 0, 0); return 1; }
        if (dst->type != OP_REG || dst->kind != RK_GPR) return 0;
        a64_fmov_to_gpr(a, sf, T0, XR(src->reg));
        write_op(t, in, dst, bytes, T0);
        return 1;
    }
    /* scalar arithmetic */
    case M(ADDSS): case M(VADDSS): case M(SUBSS): case M(VSUBSS): case M(MULSS): case M(VMULSS):
    case M(DIVSS): case M(VDIVSS): case M(ADDSD): case M(VADDSD): case M(SUBSD): case M(VSUBSD):
    case M(MULSD): case M(VMULSD): case M(DIVSD): case M(VDIVSD): case M(MINSS): case M(VMINSS):
    case M(MAXSS): case M(VMAXSS): case M(MINSD): case M(VMINSD): case M(MAXSD): case M(VMAXSD):
    case M(SQRTSS): case M(VSQRTSS): case M(SQRTSD): case M(VSQRTSD): {
        const char *name = ZydisMnemonicGetString((ZydisMnemonic)m);
        const size_t len = strlen(name);
        const int dbl = name[len - 1] == 'd', es = dbl ? 8 : 4;
        const VOps o = vops3(in);
        if (!vreg(o.dst)) return 0;
        vload(t, in, o.s1, 16, 0, 0);
        if (!vload(t, in, o.s2, vreg(o.s2) ? 16 : es, 0, 1)) return 0;
        const char *op = name + (name[0] == 'v' ? 1 : 0);
        if (!strncmp(op, "sqrt", 4)) a64_fsqrt_s(a, dbl, 2, 1);
        else if (!strncmp(op, "min", 3) || !strncmp(op, "max", 3)) {
            a64_vmov(a, 3, 0);
            minmax(t, op[1] == 'a', dbl); /* v0 = result (lane 0 used) */
            a64_vmov(a, 2, 0);
            a64_vmov(a, 0, 3);
        } else {
            const int fop = !strncmp(op, "mul", 3) ? 0 : !strncmp(op, "div", 3) ? 1 : !strncmp(op, "add", 3) ? 2 : 3;
            a64_fop_s(a, fop, dbl, 2, 0, 1);
        }
        a64_ins_elem(a, es, 0, 0, 2, 0);
        vstore_reg(t, in, o.dst->reg, 0, 0, 1);
        return 1;
    }
    /* packed arithmetic and logic */
    case M(ADDPS): case M(VADDPS): case M(SUBPS): case M(VSUBPS): case M(MULPS): case M(VMULPS):
    case M(DIVPS): case M(VDIVPS): case M(ADDPD): case M(VADDPD): case M(SUBPD): case M(VSUBPD):
    case M(MULPD): case M(VMULPD): case M(DIVPD): case M(VDIVPD): case M(MINPS): case M(VMINPS):
    case M(MAXPS): case M(VMAXPS): case M(ANDPS): case M(VANDPS): case M(ANDPD): case M(VANDPD):
    case M(PAND): case M(VPAND): case M(ORPS): case M(VORPS): case M(ORPD): case M(VORPD):
    case M(POR): case M(VPOR): case M(XORPS): case M(VXORPS): case M(XORPD): case M(VXORPD):
    case M(PXOR): case M(VPXOR): case M(ANDNPS): case M(VANDNPS): case M(ANDNPD): case M(VANDNPD):
    case M(PANDN): case M(VPANDN): {
        const char *name = ZydisMnemonicGetString((ZydisMnemonic)m);
        const char *op = name + (name[0] == 'v' ? 1 : 0);
        const size_t len = strlen(name);
        const int dbl = name[len - 1] == 'd' && name[len - 2] == 'p';
        const VOps o = vops3(in);
        if (!vreg(o.dst)) return 0;
        for (int h = 0; h <= wide; ++h) {
            if (!vload(t, in, o.s1, 16, h, 0) || !vload(t, in, o.s2, 16, h, 1)) return 0;
            if (!strncmp(op, "andn", 4) || !strncmp(op, "pandn", 5)) a64_vlogic(a, 3, 0, 1, 0);
            else if (!strncmp(op, "and", 3) || !strncmp(op, "pand", 4)) a64_vlogic(a, 0, 0, 0, 1);
            else if (!strncmp(op, "or", 2) || !strncmp(op, "por", 3)) a64_vlogic(a, 1, 0, 0, 1);
            else if (!strncmp(op, "xor", 3) || !strncmp(op, "pxor", 4)) a64_vlogic(a, 2, 0, 0, 1);
            else if (!strncmp(op, "min", 3) || !strncmp(op, "max", 3)) minmax(t, op[1] == 'a', dbl);
            else {
                const int fop = !strncmp(op, "mul", 3) ? 0 : !strncmp(op, "div", 3) ? 1 : !strncmp(op, "add", 3) ? 2 : 3;
                a64_fop_v(a, fop, dbl, 0, 0, 1);
            }
            vstore_reg(t, in, o.dst->reg, 0, h, !wide);
        }
        return 1;
    }
    case M(UCOMISS): case M(VUCOMISS): case M(COMISS): case M(VCOMISS): case M(UCOMISD):
    case M(VUCOMISD): case M(COMISD): case M(VCOMISD): {
        const int dbl = m == M(UCOMISD) || m == M(VUCOMISD) || m == M(COMISD) || m == M(VCOMISD);
        if (!vload(t, in, &in->op[0], dbl ? 8 : 4, 0, 0) || !vload(t, in, &in->op[1], dbl ? 8 : 4, 0, 1)) return 0;
        a64_fcmp(a, dbl, 0, 1);
        if (!flags_live) return 1;
        /* arm64: greater C, less N, equal ZC, unordered CV. x86: ZF PF CF = 000 / 001 / 100 / 111.
         * x8 keeps C = !CF: greater and equal map as they are, less needs N cleared, unordered
         * becomes Z with C clear and PF set. */
        a64_mrs_nzcv(a, TB);
        a64_logic_imm(a, 0, 1, TB, TB, 29, 2);   /* keep Z, C */
        a64_movz(a, 1, T3, 0x4000, 16);          /* Z only */
        a64_csel(a, 1, TB, T3, TB, CC_VS);
        a64_cset(a, 1, FRES, CC_VC);              /* ordered: odd parity byte, PF = 0 */
        a64_mov(a, 1, FAB, FRES);                 /* AF = 0 */
        a64_lsr_imm(a, 1, TB, TB, 28);
        a64_bfi(a, 1, FL, TB, 28, 4);
        return 1;
    }
    case M(PTEST): case M(VPTEST): {
        /* ZF: a & b is zero; CF: b & ~a (b ^ (a & b)) is zero; the other flags clear */
        if (!vreg(&in->op[0])) return 0;
        for (int h = 0; h <= wide; ++h) {
            if (!vload(t, in, &in->op[0], 16, h, 0) || !vload(t, in, &in->op[1], 16, h, 1)) return 0;
            if (h) {
                a64_vlogic(a, 0, 0, 0, 1);
                a64_vlogic(a, 2, 1, 1, 0);
                a64_vlogic(a, 1, 2, 2, 0);
                a64_vlogic(a, 1, 3, 3, 1);
            } else {
                a64_vlogic(a, 0, 2, 0, 1);
                a64_vlogic(a, 2, 3, 1, 2);
            }
        }
        if (!flags_live) return 1;
        a64_umov(a, 8, T0, 2, 0);
        a64_umov(a, 8, T1, 2, 1);
        a64_orr(a, 1, T0, T0, T1);
        a64_umov(a, 8, T2, 3, 0);
        a64_umov(a, 8, T3, 3, 1);
        a64_orr(a, 1, T2, T2, T3);
        a64_subs(a, 1, XZR, T0, XZR);
        a64_cset(a, 1, T4, CC_EQ);
        a64_subs(a, 1, XZR, T2, XZR);
        a64_cset(a, 1, T5, CC_NE);
        a64_logic(a, 1, 0, 1, TB, T5, T4, 0, 1); /* N Z C V = 0 ZF !CF 0 */
        a64_lsl_imm(a, 1, TB, TB, 1);
        a64_bfi(a, 1, FL, TB, 28, 4);
        a64_movz(a, 1, FRES, 1, 0); /* PF = 0 */
        a64_mov(a, 1, FAB, FRES);   /* AF = 0 */
        return 1;
    }
    case M(CVTSI2SS): case M(VCVTSI2SS): case M(CVTSI2SD): case M(VCVTSI2SD): {
        const int dbl = m == M(CVTSI2SD) || m == M(VCVTSI2SD);
        const VOps o = vops3(in);
        if (!vreg(o.dst) || o.s2->high8 || o.s2->size < 4) return 0;
        const int v = read_op(t, in, o.s2, o.s2->size, T0, NULL);
        vload(t, in, o.s1, 16, 0, 0);
        a64_scvtf(a, o.s2->size == 8, dbl, 1, v);
        a64_ins_elem(a, dbl ? 8 : 4, 0, 0, 1, 0);
        vstore_reg(t, in, o.dst->reg, 0, 0, 1);
        return 1;
    }
    case M(CVTTSS2SI): case M(VCVTTSS2SI): case M(CVTTSD2SI): case M(VCVTTSD2SI): {
        const int dbl = m == M(CVTTSD2SI) || m == M(VCVTTSD2SI);
        const BbOp *dst = &in->op[0];
        if (dst->type != OP_REG || dst->kind != RK_GPR) return 0;
        const int sf = dst->size == 8;
        if (!vload(t, in, &in->op[1], dbl ? 8 : 4, 0, 0)) return 0;
        a64_fcvtzs(a, sf, dbl, T0, 0);
        /* x86 "integer indefinite" (the most negative value) for NaN and out of range: below the
         * range fcvtzs saturates to it; at or above 2^31 (2^63) and NaN, fcmp sets C. The
         * saturated maximum itself is a valid result (a double in [2^31 - 1, 2^31)). */
        static const uint64_t limit[2][2] = {{0x4f000000, 0x5f000000},
                                             {UINT64_C(0x41e0000000000000), UINT64_C(0x43e0000000000000)}};
        if (sf) a64_movz(a, 1, T1, 0x8000, 48); else a64_movz(a, 0, T1, 0x8000, 16);
        a64_mov_imm(a, T2, limit[dbl][sf]);
        a64_fmov_from_gpr(a, dbl, 1, T2);
        a64_fcmp(a, dbl, 0, 1);
        a64_csel(a, sf, T0, T1, T0, CC_HS);
        write_op(t, in, dst, dst->size, T0);
        return 1;
    }
    case M(CVTSS2SD): case M(VCVTSS2SD): case M(CVTSD2SS): case M(VCVTSD2SS): {
        const int to_double = m == M(CVTSS2SD) || m == M(VCVTSS2SD);
        const VOps o = vops3(in);
        if (!vreg(o.dst)) return 0;
        vload(t, in, o.s1, 16, 0, 0);
        if (!vload(t, in, o.s2, to_double ? 4 : 8, 0, 1)) return 0;
        a64_fcvt_sd(a, to_double, 2, 1);
        a64_ins_elem(a, to_double ? 8 : 4, 0, 0, 2, 0);
        vstore_reg(t, in, o.dst->reg, 0, 0, 1);
        return 1;
    }
    case M(PSHUFD): case M(VPSHUFD): {
        const BbOp *dst = &in->op[0], *src = &in->op[1];
        if (!vreg(dst) || in->op[2].type != OP_IMM) return 0;
        const int imm = (int)in->op[2].disp;
        for (int h = 0; h <= wide; ++h) {
            if (!vload(t, in, src, 16, h, 0)) return 0;
            for (int i = 0; i < 4; ++i) a64_ins_elem(a, 4, 1, i, 0, (imm >> (2 * i)) & 3);
            vstore_reg(t, in, dst->reg, 1, h, !wide);
        }
        return 1;
    }
    case M(SHUFPS): case M(VSHUFPS): {
        const VOps o = vops3(in);
        if (!vreg(o.dst) || !o.imm) return 0;
        const int imm = (int)o.imm->disp;
        for (int h = 0; h <= wide; ++h) {
            if (!vload(t, in, o.s1, 16, h, 0) || !vload(t, in, o.s2, 16, h, 1)) return 0;
            a64_ins_elem(a, 4, 2, 0, 0, imm & 3);
            a64_ins_elem(a, 4, 2, 1, 0, (imm >> 2) & 3);
            a64_ins_elem(a, 4, 2, 2, 1, (imm >> 4) & 3);
            a64_ins_elem(a, 4, 2, 3, 1, (imm >> 6) & 3);
            vstore_reg(t, in, o.dst->reg, 2, h, !wide);
        }
        return 1;
    }
    case M(VZEROUPPER):
        for (int r = 0; r < 16; ++r) {
            a64_str_uoff(a, 3, XZR, CPU, VOFF(r) + 16);
            a64_str_uoff(a, 3, XZR, CPU, VOFF(r) + 24);
        }
        return 1;
    default:
        return 0;
    }
}

/* cmpps/cmppd predicates 0-15 (16-31: the same results, other exception behavior) on v0, v1 into
 * v2 (v3 scratch). */
static void fcmp_mask(A64 *a, int p, int dbl) {
    const uint32_t eq = dbl ? V_FCMEQ_2D : V_FCMEQ_4S, ge = dbl ? V_FCMGE_2D : V_FCMGE_4S;
    const uint32_t gt = dbl ? V_FCMGT_2D : V_FCMGT_4S;
    int invert = 0;
    switch (p) {
    case 0: a64_v3(a, eq, 2, 0, 1); break;                     /* EQ_OQ */
    case 1: a64_v3(a, gt, 2, 1, 0); break;                     /* LT_OS */
    case 2: a64_v3(a, ge, 2, 1, 0); break;                     /* LE_OS */
    case 3: case 7:                                             /* UNORD_Q, ORD_Q */
        a64_v3(a, eq, 2, 0, 0); a64_v3(a, eq, 3, 1, 1); a64_vlogic(a, 0, 2, 2, 3); invert = p == 3; break;
    case 4: a64_v3(a, eq, 2, 0, 1); invert = 1; break;         /* NEQ_UQ */
    case 5: a64_v3(a, gt, 2, 1, 0); invert = 1; break;         /* NLT_US */
    case 6: a64_v3(a, ge, 2, 1, 0); invert = 1; break;         /* NLE_US */
    case 8: case 12:                                            /* EQ_UQ, NEQ_OQ */
        a64_v3(a, gt, 2, 0, 1); a64_v3(a, gt, 3, 1, 0); a64_vlogic(a, 1, 2, 2, 3); invert = p == 8; break;
    case 9: a64_v3(a, ge, 2, 0, 1); invert = 1; break;         /* NGE_US */
    case 10: a64_v3(a, gt, 2, 0, 1); invert = 1; break;        /* NGT_US */
    case 11: a64_vzero(a, 2); break;                            /* FALSE */
    case 13: a64_v3(a, ge, 2, 0, 1); break;                    /* GE_OS */
    case 14: a64_v3(a, gt, 2, 0, 1); break;                    /* GT_OS */
    default: a64_vzero(a, 2); invert = 1; break;               /* TRUE */
    }
    if (invert) a64_v2(a, V_NOT_16B, 2, 2);
}

/* Integer and permute ops of the form dst = op(s1, s2) per 128-bit lane. */
static uint32_t int_op3(int m) {
    switch (m) {
#define P(x, op) case M(x): case M(V##x): return op;
    P(PADDB, V_ADD_16B) P(PADDW, V_ADD_8H) P(PADDD, V_ADD_4S) P(PADDQ, V_ADD_2D)
    P(PSUBB, V_SUB_16B) P(PSUBW, V_SUB_8H) P(PSUBD, V_SUB_4S) P(PSUBQ, V_SUB_2D)
    P(PMULLW, V_MUL_8H) P(PMULLD, V_MUL_4S)
    P(PCMPEQB, V_CMEQ_16B) P(PCMPEQW, V_CMEQ_8H) P(PCMPEQD, V_CMEQ_4S) P(PCMPEQQ, V_CMEQ_2D)
    P(PCMPGTB, V_CMGT_16B) P(PCMPGTW, V_CMGT_8H) P(PCMPGTD, V_CMGT_4S) P(PCMPGTQ, V_CMGT_2D)
    P(PMAXSB, V_SMAX_16B) P(PMAXSW, V_SMAX_8H) P(PMAXSD, V_SMAX_4S)
    P(PMINSB, V_SMIN_16B) P(PMINSW, V_SMIN_8H) P(PMINSD, V_SMIN_4S)
    P(PMAXUB, V_UMAX_16B) P(PMAXUW, V_UMAX_8H) P(PMAXUD, V_UMAX_4S)
    P(PMINUB, V_UMIN_16B) P(PMINUW, V_UMIN_8H) P(PMINUD, V_UMIN_4S)
    P(PADDSB, V_SQADD_16B) P(PADDSW, V_SQADD_8H) P(PADDUSB, V_UQADD_16B) P(PADDUSW, V_UQADD_8H)
    P(PSUBSB, V_SQSUB_16B) P(PSUBSW, V_SQSUB_8H) P(PSUBUSB, V_UQSUB_16B) P(PSUBUSW, V_UQSUB_8H)
    P(PAVGB, V_URHADD_16B) P(PAVGW, V_URHADD_8H)
    P(PUNPCKLBW, V_ZIP1_16B) P(PUNPCKLWD, V_ZIP1_8H) P(PUNPCKLDQ, V_ZIP1_4S) P(PUNPCKLQDQ, V_ZIP1_2D)
    P(PUNPCKHBW, V_ZIP2_16B) P(PUNPCKHWD, V_ZIP2_8H) P(PUNPCKHDQ, V_ZIP2_4S) P(PUNPCKHQDQ, V_ZIP2_2D)
    P(UNPCKLPS, V_ZIP1_4S) P(UNPCKHPS, V_ZIP2_4S) P(UNPCKLPD, V_ZIP1_2D) P(UNPCKHPD, V_ZIP2_2D)
#undef P
    default: return 0;
    }
}

/* More SSE/AVX: horizontal adds, compares, dot products, blends, inserts and extracts, masks,
 * broadcasts, integer arithmetic, shuffles and conversions (the rest: the interpreter). */
static int vector2(Tx *t, const BbInsn *in) {
    A64 *a = &t->a;
    const int m = in->mnemonic;
    const int wide = in->vex && in->vl == 32;
    const uint32_t iop = int_op3(m);
    if (iop) {
        const VOps o = vops3(in);
        if (!vreg(o.dst)) return 0;
        for (int h = 0; h <= wide; ++h) {
            if (!vload(t, in, o.s1, 16, h, 0) || !vload(t, in, o.s2, 16, h, 1)) return 0;
            a64_v3(a, iop, 2, 0, 1);
            vstore_reg(t, in, o.dst->reg, 2, h, !wide);
        }
        return 1;
    }
    switch (m) {
    case M(HADDPS): case M(VHADDPS): case M(HSUBPS): case M(VHSUBPS): case M(HADDPD): case M(VHADDPD): {
        const VOps o = vops3(in);
        if (!vreg(o.dst)) return 0;
        const int sub = m == M(HSUBPS) || m == M(VHSUBPS), dbl = m == M(HADDPD) || m == M(VHADDPD);
        for (int h = 0; h <= wide; ++h) {
            if (!vload(t, in, o.s1, 16, h, 0) || !vload(t, in, o.s2, 16, h, 1)) return 0;
            if (sub) {
                a64_v3(a, V_UZP1_4S, 2, 0, 1);
                a64_v3(a, V_UZP2_4S, 3, 0, 1);
                a64_fop_v(a, 3, 0, 2, 2, 3);
            } else {
                a64_v3(a, dbl ? V_FADDP_2D : V_FADDP_4S, 2, 0, 1);
            }
            vstore_reg(t, in, o.dst->reg, 2, h, !wide);
        }
        return 1;
    }
    case M(CMPPS): case M(VCMPPS): case M(CMPPD): case M(VCMPPD): case M(CMPSS): case M(VCMPSS):
    case M(CMPSD): case M(VCMPSD): {
        const VOps o = vops3(in);
        if (!vreg(o.dst) || !o.imm) return 0;
        const int dbl = m == M(CMPPD) || m == M(VCMPPD) || m == M(CMPSD) || m == M(VCMPSD);
        const int scalar = m == M(CMPSS) || m == M(VCMPSS) || m == M(CMPSD) || m == M(VCMPSD);
        const int es = dbl ? 8 : 4;
        for (int h = 0; h <= (scalar ? 0 : wide); ++h) {
            if (!vload(t, in, o.s1, 16, h, 0) || !vload(t, in, o.s2, scalar && !vreg(o.s2) ? es : 16, h, 1)) return 0;
            fcmp_mask(a, (int)o.imm->disp & 15, dbl);
            if (scalar) a64_ins_elem(a, es, 0, 0, 2, 0);
            vstore_reg(t, in, o.dst->reg, scalar ? 0 : 2, h, scalar ? 1 : !wide);
        }
        return 1;
    }
    case M(DPPS): case M(VDPPS): {
        const VOps o = vops3(in);
        if (!vreg(o.dst) || !o.imm) return 0;
        const int imm = (int)o.imm->disp;
        for (int h = 0; h <= wide; ++h) {
            if (!vload(t, in, o.s1, 16, h, 0) || !vload(t, in, o.s2, 16, h, 1)) return 0;
            a64_fop_v(a, 0, 0, 2, 0, 1);                       /* products */
            for (int i = 0; i < 4; ++i) if (!(imm & (0x10 << i))) a64_ins_gpr(a, 4, 2, i, XZR);
            a64_v3(a, V_FADDP_4S, 2, 2, 2);                    /* p0+p1, p2+p3 */
            a64_v3(a, V_FADDP_4S, 2, 2, 2);                    /* (p0+p1)+(p2+p3), the hardware's order */
            for (int i = 0; i < 4; ++i) if (!(imm & (1 << i))) a64_ins_gpr(a, 4, 2, i, XZR);
            vstore_reg(t, in, o.dst->reg, 2, h, !wide);
        }
        return 1;
    }
    case M(INSERTPS): case M(VINSERTPS): {
        const VOps o = vops3(in);
        if (!vreg(o.dst) || !o.imm) return 0;
        const int imm = (int)o.imm->disp;
        if (!vload(t, in, o.s1, 16, 0, 0)) return 0;
        if (vreg(o.s2)) {
            vload(t, in, o.s2, 16, 0, 1);
            a64_ins_elem(a, 4, 0, (imm >> 4) & 3, 1, (imm >> 6) & 3);
        } else {
            if (!vload(t, in, o.s2, 4, 0, 1)) return 0;
            a64_ins_elem(a, 4, 0, (imm >> 4) & 3, 1, 0);
        }
        for (int i = 0; i < 4; ++i) if (imm & (1 << i)) a64_ins_gpr(a, 4, 0, i, XZR);
        vstore_reg(t, in, o.dst->reg, 0, 0, 1);
        return 1;
    }
    case M(VEXTRACTF128): case M(VEXTRACTI128): {
        const BbOp *dst = &in->op[0], *src = &in->op[1];
        if (!vreg(src) || in->count < 3 || in->op[2].type != OP_IMM) return 0;
        vget(t, src->reg, (int)(in->op[2].disp & 1), 0, 16);
        if (vreg(dst)) vstore_reg(t, in, dst->reg, 0, 0, 1);
        else if (dst->type == OP_MEM) vstore_mem(t, in, dst, 16, 0, 0);
        else return 0;
        return 1;
    }
    case M(VINSERTF128): case M(VINSERTI128): {
        const BbOp *dst = &in->op[0], *s1 = &in->op[1], *s2 = &in->op[2];
        if (!vreg(dst) || !vreg(s1) || in->count < 4 || in->op[3].type != OP_IMM) return 0;
        const int half = (int)in->op[3].disp & 1;
        if (!vload(t, in, s2, 16, 0, 2)) return 0;
        vload(t, in, s1, 16, half ^ 1, 0);
        vstore_reg(t, in, dst->reg, 2, half, 0);
        vstore_reg(t, in, dst->reg, 0, half ^ 1, 0);
        return 1;
    }
    case M(VBROADCASTSS): case M(VPBROADCASTD): case M(VBROADCASTSD): case M(VPBROADCASTQ): {
        const BbOp *dst = &in->op[0], *src = &in->op[1];
        if (!vreg(dst)) return 0;
        const int es = m == M(VBROADCASTSS) || m == M(VPBROADCASTD) ? 4 : 8;
        if (src->type == OP_MEM) {
            address(t, in, src, TA);
            a64_v2(a, es == 4 ? V_LD1R_4S : V_LD1R_2D, 0, TA);
            if (vec_ordered()) a64_emit(a, 0xd50339bfu); /* dmb ishld */
        } else if (vreg(src)) {
            vget(t, src->reg, 0, 1, 16);
            a64_dup_elem(a, es, 0, 1, 0);
        } else {
            return 0;
        }
        vstore_reg(t, in, dst->reg, 0, 0, !wide);
        if (wide) vstore_reg(t, in, dst->reg, 0, 1, 0);
        return 1;
    }
    case M(MOVHLPS): case M(VMOVHLPS): case M(MOVLHPS): case M(VMOVLHPS): {
        const VOps o = vops3(in);
        if (!vreg(o.dst) || !vreg(o.s2)) return 0;
        vload(t, in, o.s1, 16, 0, 0);
        vload(t, in, o.s2, 16, 0, 1);
        if (m == M(MOVHLPS) || m == M(VMOVHLPS)) a64_ins_elem(a, 8, 0, 0, 1, 1);
        else a64_ins_elem(a, 8, 0, 1, 1, 0);
        vstore_reg(t, in, o.dst->reg, 0, 0, 1);
        return 1;
    }
    case M(PALIGNR): case M(VPALIGNR): {
        /* Per 128-bit lane: 16 bytes of s1:s2 (s2 low) from byte `shift`, zeros past the end */
        const VOps o = vops3(in);
        if (!vreg(o.dst) || !o.imm) return 0;
        const int shift = (int)o.imm->disp & 0xff;
        for (int h = 0; h <= wide; ++h) {
            if (!vload(t, in, o.s1, 16, h, 0) || !vload(t, in, o.s2, 16, h, 1)) return 0;
            if (shift < 16) a64_ext(a, 2, 1, 0, shift);
            else if (shift < 32) {
                a64_movi_16b(a, 1, 0);
                a64_ext(a, 2, 0, 1, shift - 16);
            } else a64_movi_16b(a, 2, 0);
            vstore_reg(t, in, o.dst->reg, 2, h, !wide);
        }
        return 1;
    }
    case M(MOVLPS): case M(VMOVLPS): case M(MOVLPD): case M(VMOVLPD): case M(MOVHPS):
    case M(VMOVHPS): case M(MOVHPD): case M(VMOVHPD): {
        const int high = m == M(MOVHPS) || m == M(VMOVHPS) || m == M(MOVHPD) || m == M(VMOVHPD);
        const BbOp *dst = &in->op[0];
        if (dst->type == OP_MEM) {
            if (!vreg(&in->op[1])) return 0;
            a64_ins_elem(a, 8, 0, 0, XR(in->op[1].reg), high);
            vstore_mem(t, in, dst, 8, 0, 0);
            return 1;
        }
        if (!vreg(dst)) return 0;
        vload(t, in, in->count == 3 ? &in->op[1] : dst, 16, 0, 0);
        if (!vload(t, in, &in->op[in->count - 1], 8, 0, 1)) return 0;
        a64_ins_elem(a, 8, 0, high, 1, 0);
        vstore_reg(t, in, dst->reg, 0, 0, 1);
        return 1;
    }
    case M(MOVMSKPS): case M(VMOVMSKPS): case M(MOVMSKPD): case M(VMOVMSKPD): case M(PMOVMSKB): case M(VPMOVMSKB): {
        const BbOp *dst = &in->op[0], *src = &in->op[1];
        if (dst->type != OP_REG || dst->kind != RK_GPR || !vreg(src)) return 0;
        const int ps = m == M(MOVMSKPS) || m == M(VMOVMSKPS), pd = m == M(MOVMSKPD) || m == M(VMOVMSKPD);
        const int lane_bits = ps ? 4 : pd ? 2 : 16;
        for (int h = 0; h <= wide; ++h) {
            vget(t, src->reg, h, 0, 16);
            if (pd) {
                a64_vushr(a, 8, 1, 0, 63);
                a64_umov(a, 8, T0, 1, 0);
                a64_umov(a, 8, T1, 1, 1);
                a64_logic(a, 1, 0, 0, T0, T0, T1, 0, 1);
            } else {
                /* sign bits gathered into the low byte of each 64-bit half */
                if (ps) {
                    a64_vushr(a, 4, 1, 0, 31);
                    a64_vusra(a, 8, 1, 1, 31);
                } else {
                    a64_vushr(a, 1, 1, 0, 7);
                    a64_vusra(a, 2, 1, 1, 7);
                    a64_vusra(a, 4, 1, 1, 14);
                    a64_vusra(a, 8, 1, 1, 28);
                }
                a64_umov(a, 1, T0, 1, 0);
                a64_umov(a, 1, T1, 1, 8);
                a64_logic(a, 1, 0, 0, T0, T0, T1, 0, ps ? 2 : 8);
            }
            if (h == 0) a64_mov(a, 0, T2, T0);
            else a64_logic(a, 1, 0, 0, T2, T2, T0, 0, lane_bits);
        }
        write_op(t, in, dst, dst->size < 4 ? 4 : dst->size, T2);
        return 1;
    }
    case M(BLENDPS): case M(VBLENDPS): case M(BLENDPD): case M(VBLENDPD): case M(PBLENDW): case M(VPBLENDW): {
        const VOps o = vops3(in);
        if (!vreg(o.dst) || !o.imm) return 0;
        const int es = (m == M(BLENDPS) || m == M(VBLENDPS)) ? 4 : (m == M(BLENDPD) || m == M(VBLENDPD)) ? 8 : 2;
        const int n = 16 / es, imm = (int)o.imm->disp;
        for (int h = 0; h <= wide; ++h) {
            if (!vload(t, in, o.s1, 16, h, 0) || !vload(t, in, o.s2, 16, h, 1)) return 0;
            for (int i = 0; i < n; ++i)
                if ((imm >> (es == 2 ? i : h * n + i)) & 1) a64_ins_elem(a, es, 0, i, 1, i);
            vstore_reg(t, in, o.dst->reg, 0, h, !wide);
        }
        return 1;
    }
    case M(BLENDVPS): case M(VBLENDVPS): case M(BLENDVPD): case M(VBLENDVPD): case M(PBLENDVB): case M(VPBLENDVB): {
        const BbOp *dst = &in->op[0];
        if (!vreg(dst)) return 0;
        const BbOp *x = in->vex ? &in->op[1] : dst, *y = in->vex ? &in->op[2] : &in->op[1];
        if (in->vex && (in->count < 4 || !vreg(&in->op[3]))) return 0;
        const uint32_t sign = (m == M(BLENDVPS) || m == M(VBLENDVPS)) ? V_CMLT0_4S
                            : (m == M(BLENDVPD) || m == M(VBLENDVPD)) ? V_CMLT0_2D : V_CMLT0_16B;
        for (int h = 0; h <= wide; ++h) {
            if (!vload(t, in, x, 16, h, 0) || !vload(t, in, y, 16, h, 1)) return 0;
            vget(t, in->vex ? in->op[3].reg : 0, h, 2, 16);
            a64_v2(a, sign, 2, 2);
            a64_bsl(a, 2, 1, 0);
            vstore_reg(t, in, dst->reg, 2, h, !wide);
        }
        return 1;
    }
    case M(SQRTPS): case M(VSQRTPS): case M(SQRTPD): case M(VSQRTPD): case M(RSQRTPS): case M(VRSQRTPS):
    case M(RCPPS): case M(VRCPPS): case M(CVTDQ2PS): case M(VCVTDQ2PS): case M(CVTTPS2DQ): case M(VCVTTPS2DQ): {
        const BbOp *dst = &in->op[0], *src = &in->op[1];
        if (!vreg(dst)) return 0;
        for (int h = 0; h <= wide; ++h) {
            if (!vload(t, in, src, 16, h, 0)) return 0;
            switch (m) {
            case M(SQRTPS): case M(VSQRTPS): a64_v2(a, V_FSQRT_4S, 2, 0); break;
            case M(SQRTPD): case M(VSQRTPD): a64_v2(a, V_FSQRT_2D, 2, 0); break;
            case M(RSQRTPS): case M(VRSQRTPS): /* 1 / sqrt(x), as the interpreter (exact, not an estimate) */
                a64_v2(a, V_FSQRT_4S, 1, 0);
                a64_fmov_one_4s(a, 2);
                a64_fop_v(a, 1, 0, 2, 2, 1);
                break;
            case M(RCPPS): case M(VRCPPS):
                a64_fmov_one_4s(a, 2);
                a64_fop_v(a, 1, 0, 2, 2, 0);
                break;
            case M(CVTDQ2PS): case M(VCVTDQ2PS): a64_v2(a, V_SCVTF_4S, 2, 0); break;
            default: /* cvttps2dq: NaN and out of range give 0x80000000 (fcvtzs: 0, 0x7fffffff) */
                a64_v2(a, V_FCVTZS_4S, 1, 0);
                a64_movi_4s(a, 0, 3, 0x80, 3);        /* 0x80000000 */
                a64_v3(a, V_FCMEQ_4S, 2, 0, 0);       /* not NaN */
                a64_bsl(a, 2, 1, 3);
                a64_movi_4s(a, 1, 4, 0x80, 3);        /* 0x7fffffff: positive overflow */
                a64_v3(a, V_CMEQ_4S, 4, 2, 4);
                a64_bsl(a, 4, 3, 2);
                a64_vmov(a, 2, 4);
                break;
            }
            vstore_reg(t, in, dst->reg, 2, h, !wide);
        }
        return 1;
    }
    case M(CVTPS2PD): case M(VCVTPS2PD): case M(CVTPD2PS): case M(VCVTPD2PS): {
        const BbOp *dst = &in->op[0], *src = &in->op[1];
        if (!vreg(dst) || wide) return 0;
        const int to_double = m == M(CVTPS2PD) || m == M(VCVTPS2PD);
        if (!vload(t, in, src, to_double && !vreg(src) ? 8 : 16, 0, 0)) return 0;
        a64_v2(a, to_double ? V_FCVTL_2D : V_FCVTN_2S, 2, 0);
        vstore_reg(t, in, dst->reg, 2, 0, 1);
        return 1;
    }
    case M(PSLLW): case M(VPSLLW): case M(PSLLD): case M(VPSLLD): case M(PSLLQ): case M(VPSLLQ):
    case M(PSRLW): case M(VPSRLW): case M(PSRLD): case M(VPSRLD): case M(PSRLQ): case M(VPSRLQ):
    case M(PSRAW): case M(VPSRAW): case M(PSRAD): case M(VPSRAD): {
        const BbOp *dst = &in->op[0], *src = in->vex ? &in->op[1] : dst, *count = &in->op[in->count - 1];
        if (!vreg(dst) || count->type != OP_IMM) return 0;
        const char *name = ZydisMnemonicGetString((ZydisMnemonic)m) + (in->vex ? 1 : 0); /* psllw ... */
        const int es = name[4] == 'w' ? 2 : name[4] == 'd' ? 4 : 8, bits = es * 8;
        const int left = !strncmp(name, "psll", 4), arith = !strncmp(name, "psra", 4);
        const unsigned c = (unsigned)count->disp & 0xff;
        for (int h = 0; h <= wide; ++h) {
            if (!vload(t, in, src, 16, h, 0)) return 0;
            if (c == 0) a64_vmov(a, 2, 0);
            else if (c >= (unsigned)bits && !arith) a64_vzero(a, 2);
            else if (left) a64_vshl(a, es, 2, 0, (int)c);
            else if (arith) a64_vsshr(a, es, 2, 0, c >= (unsigned)bits ? bits - 1 : (int)c);
            else a64_vushr(a, es, 2, 0, (int)c);
            vstore_reg(t, in, dst->reg, 2, h, !wide);
        }
        return 1;
    }
    case M(PSLLDQ): case M(VPSLLDQ): case M(PSRLDQ): case M(VPSRLDQ): {
        const BbOp *dst = &in->op[0], *src = in->vex ? &in->op[1] : dst, *count = &in->op[in->count - 1];
        if (!vreg(dst) || count->type != OP_IMM) return 0;
        const unsigned c = (unsigned)count->disp & 0xff;
        const int left = m == M(PSLLDQ) || m == M(VPSLLDQ);
        for (int h = 0; h <= wide; ++h) {
            if (!vload(t, in, src, 16, h, 0)) return 0;
            a64_vzero(a, 1);
            if (c == 0) a64_vmov(a, 2, 0);
            else if (c >= 16) a64_vzero(a, 2);
            else if (left) a64_ext(a, 2, 1, 0, 16 - (int)c);
            else a64_ext(a, 2, 0, 1, (int)c);
            vstore_reg(t, in, dst->reg, 2, h, !wide);
        }
        return 1;
    }
    case M(PSHUFB): case M(VPSHUFB): {
        const VOps o = vops3(in);
        if (!vreg(o.dst)) return 0;
        for (int h = 0; h <= wide; ++h) {
            if (!vload(t, in, o.s1, 16, h, 0) || !vload(t, in, o.s2, 16, h, 1)) return 0;
            a64_movi_16b(a, 3, 0x8f);                /* bit 7: zero; else the low 4 bits */
            a64_vlogic(a, 0, 1, 1, 3);
            a64_v3(a, V_TBL_16B, 2, 0, 1);
            vstore_reg(t, in, o.dst->reg, 2, h, !wide);
        }
        return 1;
    }
    case M(PACKSSDW): case M(VPACKSSDW): case M(PACKUSDW): case M(VPACKUSDW): case M(PACKSSWB): case M(VPACKSSWB):
    case M(PACKUSWB): case M(VPACKUSWB): {
        const VOps o = vops3(in);
        if (!vreg(o.dst)) return 0;
        uint32_t lo, hi;
        switch (m) {
        case M(PACKSSDW): case M(VPACKSSDW): lo = V_SQXTN_4H; hi = V_SQXTN2_8H; break;
        case M(PACKUSDW): case M(VPACKUSDW): lo = V_SQXTUN_4H; hi = V_SQXTUN2_8H; break;
        case M(PACKSSWB): case M(VPACKSSWB): lo = V_SQXTN_8B; hi = V_SQXTN2_16B; break;
        default: lo = V_SQXTUN_8B; hi = V_SQXTUN2_16B; break;
        }
        for (int h = 0; h <= wide; ++h) {
            if (!vload(t, in, o.s1, 16, h, 0) || !vload(t, in, o.s2, 16, h, 1)) return 0;
            a64_v2(a, lo, 2, 0);
            a64_v2(a, hi, 2, 1);
            vstore_reg(t, in, o.dst->reg, 2, h, !wide);
        }
        return 1;
    }
    case M(PMOVZXBW): case M(VPMOVZXBW): case M(PMOVZXWD): case M(VPMOVZXWD): case M(PMOVZXDQ): case M(VPMOVZXDQ):
    case M(PMOVSXBW): case M(VPMOVSXBW): case M(PMOVSXWD): case M(VPMOVSXWD): case M(PMOVSXDQ): case M(VPMOVSXDQ):
    case M(PMOVZXBD): case M(VPMOVZXBD): case M(PMOVSXBD): case M(VPMOVSXBD): {
        const BbOp *dst = &in->op[0], *src = &in->op[1];
        if (!vreg(dst) || wide) return 0;
        const char *name = ZydisMnemonicGetString((ZydisMnemonic)m) + (in->vex ? 1 : 0);
        const int sx = name[4] == 's';
        const char from = name[6], to = name[7];
        const int bytes = from == 'b' && to == 'd' ? 4 : 8;
        if (!vload(t, in, src, vreg(src) ? 16 : bytes, 0, 0)) return 0;
        if (from == 'b') a64_v2(a, sx ? V_SXTL_8H : V_UXTL_8H, 0, 0);
        if (from == 'w' || to == 'd') a64_v2(a, sx ? V_SXTL_4S : V_UXTL_4S, 0, 0);
        if (to == 'q') a64_v2(a, sx ? V_SXTL_2D : V_UXTL_2D, 0, 0);
        vstore_reg(t, in, dst->reg, 0, 0, 1);
        return 1;
    }
    case M(MOVSHDUP): case M(VMOVSHDUP): case M(MOVSLDUP): case M(VMOVSLDUP): case M(MOVDDUP): case M(VMOVDDUP): {
        const BbOp *dst = &in->op[0], *src = &in->op[1];
        if (!vreg(dst)) return 0;
        const int ddup = m == M(MOVDDUP) || m == M(VMOVDDUP);
        for (int h = 0; h <= wide; ++h) {
            if (!vload(t, in, src, ddup && !wide && !vreg(src) ? 8 : 16, h, 0)) return 0;
            if (ddup) a64_dup_elem(a, 8, 2, 0, 0);
            else a64_v3(a, (m == M(MOVSHDUP) || m == M(VMOVSHDUP)) ? V_TRN2_4S : V_TRN1_4S, 2, 0, 0);
            vstore_reg(t, in, dst->reg, 2, h, !wide);
        }
        return 1;
    }
    case M(PEXTRD): case M(VPEXTRD): case M(PEXTRQ): case M(VPEXTRQ): case M(EXTRACTPS): case M(VEXTRACTPS): {
        const BbOp *dst = &in->op[0], *src = &in->op[1];
        if (!vreg(src) || in->count < 3 || in->op[2].type != OP_IMM) return 0;
        const int es = (m == M(PEXTRQ) || m == M(VPEXTRQ)) ? 8 : 4;
        const int i = (int)in->op[2].disp & (es == 8 ? 1 : 3);
        a64_umov(a, es, T0, XR(src->reg), i);
        if (dst->type == OP_REG && dst->kind == RK_GPR) write_op(t, in, dst, dst->size < 4 ? 4 : dst->size, T0);
        else if (dst->type == OP_MEM) write_op(t, in, dst, es, T0);
        else return 0;
        return 1;
    }
    case M(PINSRD): case M(VPINSRD): case M(PINSRQ): case M(VPINSRQ): {
        const VOps o = vops3(in);
        if (!vreg(o.dst) || !o.imm || (o.s2->type == OP_REG && o.s2->kind != RK_GPR)) return 0;
        const int es = (m == M(PINSRQ) || m == M(VPINSRQ)) ? 8 : 4;
        const int v = read_op(t, in, o.s2, es, T0, NULL);
        if (!vload(t, in, o.s1, 16, 0, 0)) return 0;
        a64_ins_gpr(a, es, 0, (int)o.imm->disp & (es == 8 ? 1 : 3), v);
        vstore_reg(t, in, o.dst->reg, 0, 0, 1);
        return 1;
    }
    case M(VPERMILPS): {
        const BbOp *dst = &in->op[0], *src = &in->op[1];
        if (!vreg(dst) || in->count < 3 || in->op[2].type != OP_IMM) return 0;
        const int imm = (int)in->op[2].disp;
        for (int h = 0; h <= wide; ++h) {
            if (!vload(t, in, src, 16, h, 0)) return 0;
            for (int i = 0; i < 4; ++i) a64_ins_elem(a, 4, 1, i, 0, (imm >> (2 * i)) & 3);
            vstore_reg(t, in, dst->reg, 1, h, !wide);
        }
        return 1;
    }
    default:
        return 0;
    }
}

/* shl/shr/sar by cl: nothing changes (flags included) when the masked count is 0. */
static int shift_cl(Tx *t, const BbInsn *in, int flags_live) {
    A64 *a = &t->a;
    const int m = in->mnemonic;
    const BbOp *dst = &in->op[0];
    const int size = dst->size, bits = size * 8, sf = size == 8;
    if (dst->high8 || size < 4 || in->count < 2 || in->op[1].type != OP_REG || in->op[1].reg != RCX) return 0;
    a64_logic_imm(a, 0, 0, T5, G[RCX], 0, sf ? 6 : 5); /* count */
    uint32_t *skip = a64_here(a);
    a64_cbz(a, 0, T5, 0);
    const int va = read_op(t, in, dst, size, T0, NULL);
    const int r = T2;
    a64_shiftv(a, m == M(SHL) ? 0 : m == M(SHR) ? 1 : 2, sf, r, va, T5);
    if (flags_live) {
        a64_ands(a, sf, XZR, r, r);
        a64_mrs_nzcv(a, TB);
        /* CF: shl (a >> (bits - count)) & 1, shr/sar (a >> (count - 1)) & 1 */
        if (m == M(SHL)) {
            a64_movz(a, 0, T3, (uint16_t)bits, 0);
            a64_sub(a, 0, T3, T3, T5);
        } else {
            a64_sub_imm(a, 0, T3, T5, 1, 0);
        }
        a64_shiftv(a, m == M(SAR) ? 2 : 1, sf, T3, va, T3);
        a64_logic_imm(a, 0, 1, T3, T3, 0, 1);
        a64_eor_bit(a, T4, T3, 0);                 /* !CF */
        a64_logic(a, 1, 0, 1, TB, TB, T4, 0, 29);
        /* OF (defined for count 1): shl MSB(r) ^ CF, shr MSB(a), sar 0 */
        if (m == M(SHL)) { a64_ubfx(a, 1, T4, r, bits - 1, 1); a64_eor(a, 1, T4, T4, T3); }
        else if (m == M(SHR)) a64_ubfx(a, 1, T4, va, bits - 1, 1);
        else a64_movz(a, 0, T4, 0, 0);
        a64_logic(a, 1, 0, 1, TB, TB, T4, 0, 28);
        a64_lsr_imm(a, 1, TB, TB, 28);
        a64_bfi(a, 1, FL, TB, 28, 4);
        a64_mov(a, 1, FRES, r);
    }
    write_op(t, in, dst, size, r);
    a64_patch_bcond(skip, a64_here(a)); /* cbz shares the imm19 field layout */
    return 1;
}

/* bt reg, reg/imm: CF = the bit; the other flags are left as they were (undefined on x86). */
static int bit_test(Tx *t, const BbInsn *in) {
    A64 *a = &t->a;
    const BbOp *dst = &in->op[0], *src = &in->op[1];
    if (dst->type != OP_REG || dst->kind != RK_GPR || dst->high8 || dst->size < 4) return 0;
    const int size = dst->size, sf = size == 8;
    const int va = read_op(t, in, dst, size, T0, NULL);
    if (src->type == OP_IMM) {
        a64_ubfx(a, 1, T3, va, (int)(src->disp & (size * 8 - 1)), 1);
    } else {
        if (src->high8) return 0;
        const int vb = read_op(t, in, src, size, T1, NULL);
        a64_shiftv(a, 1, sf, T3, va, vb); /* lsrv masks the count like x86 */
        a64_logic_imm(a, 0, 1, T3, T3, 0, 1);
    }
    a64_eor_bit(a, T3, T3, 0);
    a64_bfi(a, 1, FL, T3, 29, 1);
    return 1;
}

static int plain_push(void) {
    static int on = -1;
    if (on < 0) on = env_flag("BB_JIT_PLAIN_PUSH", 1);
    return on;
}
static void push_reg(Tx *t, int reg) {
    a64_sub_imm(&t->a, 1, G[RSP], G[RSP], 8, 0);
    if (plain_push()) a64_str_uoff(&t->a, 3, reg, G[RSP], 0);
    else store(t, 8, reg, G[RSP]);
}
static void pop_to(Tx *t, int reg) {
    if (plain_push()) a64_ldr_uoff(&t->a, 3, reg, G[RSP], 0);
    else load(t, 8, reg, G[RSP]);
}

/* div/idiv r/m32 and r/m64 as one 64-bit division when the quotient fits (the common case:
 * edx:eax from cdq, rdx from cqo or zeroed); otherwise, and for a zero divisor (#DE), the
 * interpreter. The flags are undefined. */
static int divide(Tx *t, const BbInsn *in) {
    A64 *a = &t->a;
    const BbOp *src = &in->op[0];
    const int size = src->size, sgn = in->mnemonic == M(IDIV);
    if (size < 4 || in->count != 1) return 0;
    Slow s = {0};
    int d = read_op(t, in, src, size, T1, NULL);
    if (size == 4) {
        if (sgn) a64_sxt(a, 1, T1, d, 4);
        else if (d != T1) a64_mov(a, 0, T1, d);
        d = T1;
        slow_cbz(t, &s, 1, d);
        a64_mov(a, 0, T0, G[RAX]);           /* edx:eax */
        a64_bfi(a, 1, T0, G[RDX], 32, 32);
        if (sgn) {
            a64_sdiv(a, 1, T2, T0, d);
            a64_sxt(a, 1, T3, T2, 4);        /* the quotient fits 32 bits */
            a64_subs(a, 1, XZR, T3, T2);
            slow_if(t, &s, CC_NE);
        } else {
            a64_udiv(a, 1, T2, T0, d);
            a64_lsr_imm(a, 1, T3, T2, 32);
            slow_cbnz(t, &s, 1, T3);
        }
        a64_msub(a, 1, T3, T2, d, T0);       /* remainder */
        a64_mov(a, 0, G[RAX], T2);
        a64_mov(a, 0, G[RDX], T3);
    } else {
        if (d != T1) { a64_mov(a, 1, T1, d); d = T1; }
        slow_cbz(t, &s, 1, d);
        if (sgn) {
            a64_arith(a, 3, 1, XZR, G[RDX], G[RAX], 2, 63); /* rdx is rax's sign extension */
            slow_if(t, &s, CC_NE);
            a64_adds_imm(a, 1, XZR, d, 1);                   /* / -1: INT64_MIN overflows */
            slow_if(t, &s, CC_EQ);
            a64_sdiv(a, 1, T2, G[RAX], d);
        } else {
            slow_cbnz(t, &s, 1, G[RDX]);
            a64_udiv(a, 1, T2, G[RAX], d);
        }
        a64_msub(a, 1, T3, T2, d, G[RAX]);
        a64_mov(a, 1, G[RAX], T2);
        a64_mov(a, 1, G[RDX], T3);
    }
    slow_end(t, &s, in);
    return 1;
}

/* CF = OF = the overflow condition `cc` of the last compare (x8: C = !CF at bit 29, V at 28). */
static void flags_cf_of(Tx *t, int cc) {
    A64 *a = &t->a;
    a64_cset(a, 1, T5, cc);
    a64_eor_bit(a, T4, T5, 0);
    a64_logic(a, 1, 0, 1, T5, T5, T4, 0, 1); /* bit 0: V, bit 1: C */
    a64_bfi(a, 1, FL, T5, 28, 2);
}

/* mul/imul r/m32 and r/m64 (one operand): rdx:rax (edx:eax) = rax * src. CF = OF = the high half
 * is not the low half's extension; the other flags are undefined. */
static int multiply1(Tx *t, const BbInsn *in, int flags_live) {
    A64 *a = &t->a;
    const BbOp *src = &in->op[0];
    const int size = src->size, sgn = in->mnemonic == M(IMUL);
    if (size < 4 || in->count != 1) return 0;
    const int v = read_op(t, in, src, size, T1, NULL);
    if (size == 8) {
        a64_mul(a, 1, T2, G[RAX], v);
        if (sgn) a64_smulh(a, T3, G[RAX], v); else a64_umulh(a, T3, G[RAX], v);
        if (flags_live) {
            if (sgn) a64_arith(a, 3, 1, XZR, T3, T2, 2, 63); else a64_subs_imm(a, 1, XZR, T3, 0);
            flags_cf_of(t, CC_NE);
            a64_mov(a, 1, FRES, T2);
        }
        a64_mov(a, 1, G[RAX], T2);
        a64_mov(a, 1, G[RDX], T3);
    } else {
        if (sgn) a64_smull(a, T2, G[RAX], v); else a64_umull(a, T2, G[RAX], v);
        if (flags_live) {
            if (sgn) { a64_sxt(a, 1, T3, T2, 4); a64_subs(a, 1, XZR, T3, T2); }
            else { a64_lsr_imm(a, 1, T3, T2, 32); a64_subs_imm(a, 1, XZR, T3, 0); }
            flags_cf_of(t, CC_NE);
            a64_mov(a, 1, FRES, T2);
        }
        a64_lsr_imm(a, 1, T3, T2, 32);
        a64_mov(a, 0, G[RAX], T2);
        a64_mov(a, 0, G[RDX], T3);
    }
    return 1;
}

/* tzcnt/lzcnt/bsf/bsr/popcnt r32/r64, r/m. bsf/bsr leave the destination as it was for a zero
 * source (ZF set). */
static int bit_count(Tx *t, const BbInsn *in, int flags_live) {
    A64 *a = &t->a;
    const int m = in->mnemonic;
    const BbOp *dst = &in->op[0], *src = &in->op[1];
    const int size = dst->size, sf = size == 8, bits = size * 8;
    if (size < 4 || dst->type != OP_REG || dst->kind != RK_GPR) return 0;
    const int v = read_op(t, in, src, size, T0, NULL);
    switch (m) {
    case M(TZCNT): case M(BSF): a64_rbit(a, sf, T2, v); a64_clz(a, sf, T2, T2); break;
    case M(LZCNT): a64_clz(a, sf, T2, v); break;
    case M(BSR): a64_clz(a, sf, T2, v); a64_movz(a, 0, T3, (uint16_t)(bits - 1), 0); a64_sub(a, sf, T2, T3, T2); break;
    default: /* popcnt */
        a64_fmov_from_gpr(a, 1, 0, v);
        a64_v2(a, V_CNT_8B, 0, 0);
        a64_v2(a, V_ADDV_8B, 0, 0);
        a64_fmov_to_gpr(a, 0, T2, 0);
        break;
    }
    if (m == M(BSF) || m == M(BSR)) {
        a64_subs_imm(a, sf, XZR, v, 0);
        if (flags_live) { a64_cset(a, 1, T5, CC_EQ); a64_bfi(a, 1, FL, T5, 30, 1); }
        a64_csel(a, 1, G[dst->reg], G[dst->reg], T2, CC_EQ); /* T2 is zero-extended */
        return 1;
    }
    if (flags_live) {
        if (m == M(POPCNT)) {
            /* ZF = source zero; CF OF SF AF PF clear (C = !CF set, PF from an odd result byte) */
            a64_subs_imm(a, sf, XZR, v, 0);
            a64_cset(a, 1, T5, CC_EQ);
            a64_lsl_imm(a, 1, T5, T5, 2);
            a64_orr_bit(a, T5, T5, 1);
            a64_bfi(a, 1, FL, T5, 28, 4);
            a64_movz(a, 0, FRES, 1, 0);
            a64_mov(a, 1, FAB, FRES);
        } else {
            /* CF = source zero (C = source nonzero), ZF = result zero */
            a64_subs_imm(a, sf, XZR, v, 0);
            a64_cset(a, 1, T4, CC_NE);
            a64_subs_imm(a, sf, XZR, T2, 0);
            a64_cset(a, 1, T5, CC_EQ);
            a64_bfi(a, 1, FL, T4, 29, 1);
            a64_bfi(a, 1, FL, T5, 30, 1);
        }
    }
    write_op(t, in, dst, size, T2);
    return 1;
}

/* BMI1: andn, bextr, blsi, blsmsk, blsr (32/64-bit). */
static int bmi1(Tx *t, const BbInsn *in, int flags_live) {
    A64 *a = &t->a;
    const int m = in->mnemonic;
    const BbOp *dst = &in->op[0];
    const int size = dst->size, sf = size == 8, bits = size * 8;
    if (size < 4 || dst->type != OP_REG || dst->kind != RK_GPR) return 0;
    if (m == M(ANDN) || m == M(BEXTR)) {
        const int x = read_op(t, in, &in->op[1], size, T0, NULL);
        const int y = read_op(t, in, &in->op[2], size, T1, NULL);
        if (m == M(ANDN)) {
            a64_logic(a, 3, 1, sf, T2, y, x, 0, 0); /* bics: y & ~x */
        } else {
            a64_ubfx(a, 1, T4, y, 0, 8);             /* start */
            a64_ubfx(a, 1, T5, y, 8, 8);             /* length */
            a64_shiftv(a, 1, sf, T2, x, T4);
            a64_subs_imm(a, 1, XZR, T4, (uint32_t)bits);
            a64_csel(a, sf, T2, XZR, T2, CC_HS);
            a64_movz(a, 1, T3, 1, 0);
            a64_shiftv(a, 0, 1, T3, T3, T5);
            a64_sub_imm(a, 1, T3, T3, 1, 0);
            a64_subs_imm(a, 1, XZR, T5, (uint32_t)bits);
            a64_csinv(a, 1, T3, T3, XZR, CC_LO);
            a64_ands(a, sf, T2, T2, T3);
        }
        if (flags_live) { save_nzcv(t, 1); a64_mov(a, 1, FRES, T2); } /* CF = OF = 0 */
        write_op(t, in, dst, size, T2);
        return 1;
    }
    const int v = read_op(t, in, &in->op[1], size, T0, NULL);
    switch (m) {
    case M(BLSR): a64_sub_imm(a, sf, T2, v, 1, 0); a64_ands(a, sf, T2, T2, v); break;
    case M(BLSI): a64_sub(a, sf, T2, XZR, v); a64_ands(a, sf, T2, T2, v); break;
    default: a64_sub_imm(a, sf, T2, v, 1, 0); a64_eor(a, sf, T2, T2, v); a64_ands(a, sf, XZR, T2, T2); break;
    }
    if (flags_live) {
        save_nzcv(t, 0);                              /* N Z from the result, V = 0 */
        a64_subs_imm(a, sf, XZR, v, 0);               /* CF: blsi source nonzero, else zero */
        a64_cset(a, 1, T4, m == M(BLSI) ? CC_EQ : CC_NE);
        a64_bfi(a, 1, FL, T4, 29, 1);
        a64_mov(a, 1, FRES, T2);
    }
    write_op(t, in, dst, size, T2);
    return 1;
}

/* shl/shr/sar of 8/16-bit operands (counts below the width as immediates, any by cl): the value
 * shifted in a 32-bit register, the flags from it moved to the top. */
static int shift_small(Tx *t, const BbInsn *in, int flags_live) {
    A64 *a = &t->a;
    const int m = in->mnemonic;
    const BbOp *dst = &in->op[0];
    const int size = dst->size, bits = size * 8, top = 32 - bits;
    const int by_cl = in->count > 1 && in->op[1].type == OP_REG;
    unsigned count = 1;
    if (by_cl && (in->op[1].reg != RCX || in->op[1].high8)) return 0;
    if (!by_cl && in->count > 1) {
        if (in->op[1].type != OP_IMM) return 0;
        count = (unsigned)in->op[1].disp & 31;
        if (!count) return 1; /* nothing changes, the flags included */
        if (count >= (unsigned)bits) return 0;
    }
    uint32_t *skip = NULL;
    if (by_cl) {
        a64_logic_imm(a, 0, 0, T5, G[RCX], 0, 5);
        skip = a64_here(a);
        a64_cbz(a, 0, T5, 0);
    } else {
        a64_movz(a, 0, T5, (uint16_t)count, 0);
    }
    int va = read_op(t, in, dst, size, T0, NULL);
    if (m == M(SAR)) { a64_sxt(a, 0, T1, va, size); va = T1; }
    a64_shiftv(a, m == M(SHL) ? 0 : m == M(SHR) ? 1 : 2, 0, T2, va, T5);
    if (flags_live) {
        a64_lsl_imm(a, 0, T3, T2, top);
        a64_ands(a, 0, XZR, T3, T3);
        a64_mrs_nzcv(a, TB);                           /* N Z, C = V = 0 */
        /* CF: shl bit `bits` of the shifted value, shr/sar bit count - 1 of the operand */
        if (m == M(SHL)) a64_ubfx(a, 1, T3, T2, bits, 1);
        else {
            a64_sub_imm(a, 0, T4, T5, 1, 0);
            a64_shiftv(a, m == M(SAR) ? 2 : 1, 0, T3, va, T4);
            a64_logic_imm(a, 0, 0, T3, T3, 0, 1);
        }
        a64_eor_bit(a, T4, T3, 0);
        a64_logic(a, 1, 0, 1, TB, TB, T4, 0, 29);     /* C = !CF */
        if (by_cl || count == 1) {                     /* OF: shl MSB(r) ^ CF, shr MSB(a), sar 0 */
            if (m == M(SHL)) { a64_ubfx(a, 1, T4, T2, bits - 1, 1); a64_eor(a, 1, T4, T4, T3); }
            else if (m == M(SHR)) a64_ubfx(a, 1, T4, va, bits - 1, 1);
            else a64_movz(a, 0, T4, 0, 0);
            a64_logic(a, 1, 0, 1, TB, TB, T4, 0, 28);
        }
        a64_lsr_imm(a, 1, TB, TB, 28);
        a64_bfi(a, 1, FL, TB, 28, 4);
        a64_ubfx(a, 1, FRES, T2, 0, bits);
    }
    write_op(t, in, dst, size, T2);
    if (skip) a64_patch_bcond(skip, a64_here(a));
    return 1;
}

/* rol/ror by an immediate or cl: only CF and OF change, and nothing for a masked count of 0. 8 and
 * 16-bit operands are repeated across 32 bits, so a 32-bit rotation rotates each copy. */
static int rotate(Tx *t, const BbInsn *in, int flags_live) {
    A64 *a = &t->a;
    const BbOp *dst = &in->op[0];
    const int size = dst->size, bits = size * 8, sf = size == 8, left = in->mnemonic == M(ROL);
    const int by_cl = in->count > 1 && in->op[1].type == OP_REG;
    if (by_cl && (in->op[1].reg != RCX || in->op[1].high8)) return 0;
    if (!by_cl && in->count > 1 && in->op[1].type != OP_IMM) return 0;
    uint32_t *skip = NULL;
    if (by_cl) {
        a64_logic_imm(a, 0, 0, T5, G[RCX], 0, sf ? 6 : 5);
        skip = a64_here(a);
        a64_cbz(a, 0, T5, 0);
    } else {
        const unsigned count = (in->count > 1 ? (unsigned)in->op[1].disp : 1) & (sf ? 63 : 31);
        if (!count) return 1;
        a64_movz(a, 0, T5, (uint16_t)count, 0);
    }
    int v = read_op(t, in, dst, size, T0, NULL);
    if (size < 4) {
        a64_logic(a, 1, 0, 0, T1, v, v, 0, bits);
        if (size == 1) a64_logic(a, 1, 0, 0, T1, T1, T1, 0, 16);
        v = T1;
    }
    if (left) a64_sub(a, 0, T5, XZR, T5); /* rorv takes the count modulo the width */
    a64_shiftv(a, 3, sf, T2, v, T5);
    if (flags_live) {
        /* rol: CF = LSB(r), OF = MSB(r) ^ CF; ror: CF = MSB(r), OF = CF ^ bit bits - 2 of r */
        a64_ubfx(a, 1, T3, T2, left ? 0 : bits - 1, 1);
        a64_ubfx(a, 1, T4, T2, left ? bits - 1 : bits - 2, 1);
        a64_eor(a, 1, T4, T4, T3);
        a64_eor_bit(a, T3, T3, 0);
        a64_bfi(a, 1, FL, T3, 29, 1); /* C = !CF */
        a64_bfi(a, 1, FL, T4, 28, 1);
    }
    write_op(t, in, dst, size, T2);
    if (skip) a64_patch_bcond(skip, a64_here(a));
    return 1;
}

/* rdtsc: the guest's 1.6 GHz counter from the virtual counter, as bbcpu_tsc computes it. */
static int read_tsc(Tx *t) {
    A64 *a = &t->a;
    uint64_t num, den;
    bbcpu_tsc_scale(&num, &den);
    a64_mrs_cntvct(a, T0);
    a64_mov_imm(a, T1, num);
    a64_mul(a, 1, T0, T0, T1);
    a64_mov_imm(a, T1, den);
    a64_udiv(a, 1, T0, T0, T1);
    a64_mov(a, 0, G[RAX], T0);
    a64_lsr_imm(a, 1, G[RDX], T0, 32);
    return 1;
}

/* Locked read-modify-writes and xchg with memory as LSE atomics between full barriers (x86
 * locked instructions are full barriers; acquire-release atomics are not, and a failed CAS only
 * acquires). Misaligned addresses (split locks) take the interpreter. */
static int atomic_rmw(Tx *t, const BbInsn *in, int flags_live) {
    A64 *a = &t->a;
    const int m = in->mnemonic;
    const BbOp *mem = &in->op[0], *other = in->count > 1 ? &in->op[1] : NULL;
    if (m == M(XCHG) && other && mem->type != OP_MEM) { const BbOp *x = mem; mem = other; other = x; }
    if (mem->type != OP_MEM) return 0;
    if (!in->lock && m != M(XCHG)) return 0;
    const int size = mem->size, lg = size_log2(size);
    if (other && other->type == OP_REG && (other->kind != RK_GPR || other->high8)) return 0;
    switch (m) {
    case M(CMPXCHG): case M(XADD): case M(XCHG):
        if (!other || other->type != OP_REG) return 0;
        break;
    case M(ADD): case M(SUB): case M(AND): case M(OR): case M(XOR):
        if (!other || (other->type != OP_REG && other->type != OP_IMM)) return 0;
        break;
    case M(INC): case M(DEC):
        break;
    default:
        return 0;
    }
    Slow s = {0};
    /* The value operand first (it may use TA's scratch), then the address. */
    int v = T1;
    if (other) {
        v = read_op(t, in, other, size, T1, NULL);
        if (v != T1) { a64_mov(a, 1, T1, v); v = T1; }
    } else {
        a64_movz(a, 0, T1, 1, 0);
    }
    address(t, in, mem, TA);
    if (size > 1) {
        a64_logic_imm(a, 3, 1, XZR, TA, 0, lg); /* tst: misaligned */
        slow_if(t, &s, CC_NE);
    }
    a64_dmb_ish(a);
    switch (m) {
    case M(CMPXCHG): {
        /* T5 = the accumulator (expected), T0 = the old value */
        if (size == 8) a64_mov(a, 1, T5, G[RAX]);
        else if (size == 4) a64_mov(a, 0, T5, G[RAX]);
        else a64_uxt(a, T5, G[RAX], size);
        a64_mov(a, 1, T0, T5);
        a64_casal(a, lg, T0, T1, TA);
        a64_dmb_ish(a);
        alu_core(t, M(CMP), size, T5, T0, flags_live); /* flags as cmp accumulator, old; Z: equal */
        if (size >= 4) {
            a64_csel(a, 1, G[RAX], G[RAX], T0, CC_EQ); /* failed: the accumulator = old (zero-extended) */
        } else {
            a64_mov(a, 1, T3, G[RAX]);
            a64_bfi(a, 1, T3, T0, 0, size * 8);
            a64_csel(a, 1, G[RAX], G[RAX], T3, CC_EQ);
        }
        break;
    }
    case M(XCHG): case M(XADD): {
        if (m == M(XCHG)) a64_swpal(a, lg, T1, T0, TA);
        else a64_ldopal(a, 0, lg, T1, T0, TA);
        a64_dmb_ish(a);
        if (m == M(XADD) && flags_live) alu_core(t, M(ADD), size, T0, T1, 1);
        write_op(t, in, other, size, T0);
        break;
    }
    case M(INC): case M(DEC):
        if (m == M(DEC)) a64_sub(a, 1, T5, XZR, T1); else a64_mov(a, 1, T5, T1);
        a64_ldopal(a, 0, lg, T5, T0, TA);
        a64_dmb_ish(a);
        if (flags_live) unary_core(t, m, size, T0, 1);
        break;
    default: {
        /* add/sub: ldadd (of the negated value); and: ldclr of the complement; or: ldset; xor: ldeor */
        int op = 0;
        if (m == M(SUB)) a64_sub(a, 1, T5, XZR, T1);
        else if (m == M(AND)) { a64_mvn(a, 1, T5, T1); op = 1; }
        else { a64_mov(a, 1, T5, T1); op = m == M(OR) ? 3 : m == M(XOR) ? 2 : 0; }
        a64_ldopal(a, op, lg, T5, T0, TA);
        a64_dmb_ish(a);
        if (flags_live) alu_core(t, m, size, T0, T1, 1);
        break;
    }
    }
    slow_end(t, &s, in);
    return 1;
}

/* BB_JIT_DENY=mnemonic,...: those instructions interpreted (bisection); also the groups
 * "vector" (all SSE/AVX translations), "chain" (no block chaining), "icache" (no indirect
 * branch cache), "mem" (anything with a memory operand), "stack" (push/pop/leave and the
 * non-branch stack ops), "@<hex>" (the instruction at that guest address), "m:<mnemonic>"
 * (its memory operand forms), "r:<mnemonic>" (its other forms). */
static uint8_t denied[ZYDIS_MNEMONIC_MAX_VALUE + 1]; /* 1: all forms, 2: memory forms, 3: the others */
static int deny_vector, deny_chain, deny_mem, deny_stack;
static uint64_t deny_at[64];
static int deny_at_count;
static void init_deny(void) {
    const char *env = getenv("BB_JIT_DENY");
    if (!env || !*env) return;
    char buf[1024];
    snprintf(buf, sizeof(buf), "%s", env);
    for (char *save = NULL, *tok = strtok_r(buf, ", ", &save); tok; tok = strtok_r(NULL, ", ", &save)) {
        if (!strcmp(tok, "vector")) { deny_vector = 1; continue; }
        if (!strcmp(tok, "chain")) { deny_chain = 1; continue; }
        if (!strcmp(tok, "mem")) { deny_mem = 1; continue; }
        if (tok[0] == '@') { if (deny_at_count < 64) deny_at[deny_at_count++] = strtoull(tok + 1, NULL, 16); continue; }
        if (!strcmp(tok, "stack")) { deny_stack = 1; continue; }
        if (!strcmp(tok, "icache")) { deny_icache = 1; continue; }
        const int mem_only = !strncmp(tok, "m:", 2), reg_only = !strncmp(tok, "r:", 2);
        int found = 0;
        if (mem_only || reg_only) tok += 2;
        for (int m = 1; m <= ZYDIS_MNEMONIC_MAX_VALUE; ++m) {
            if (!strcmp(ZydisMnemonicGetString((ZydisMnemonic)m), tok)) { denied[m] = mem_only ? 2 : reg_only ? 3 : 1; found = 1; break; }
        }
        if (!found) printf("CPU: BB_JIT_DENY: unknown mnemonic %s\n", tok);
    }
    printf("CPU: BB_JIT_DENY=%s\n", env);
}

/* Translates one instruction; returns 1 when it ends the block (an exit was emitted). */
static int translate_insn(Tx *t, const BbInsn *in, int flags_live, int *handled) {
    A64 *a = &t->a;
    const int m = in->mnemonic;
    const uint64_t next = t->rip + in->length;
    *handled = 1;
    if (denied[m] == 1) { *handled = 0; return 0; }
    if (deny_mem || denied[m] >= 2) {
        int mem = 0;
        for (int i = 0; i < in->count; ++i) mem |= in->op[i].type == OP_MEM;
        if (mem ? deny_mem || denied[m] == 2 : denied[m] == 3) { *handled = 0; return 0; }
    }
    for (int i = 0; i < deny_at_count; ++i)
        if (deny_at[i] == t->rip) { *handled = 0; return 0; }
    if (deny_stack && (m == M(PUSH) || m == M(POP) || m == M(LEAVE))) { *handled = 0; return 0; }
    const int8_t cc = cc_of[m];
    if (cc >= 0) {
        const int acc = arm_cc(cc);
        if (cc_kind[m] == 1) {
            const uint64_t target = t->rip + (uint64_t)in->op[0].disp;
            uint32_t *skip;
            if (acc >= 0) {
                a64_msr_nzcv(a, FL);
                skip = a64_here(a);
                a64_bcond(a, acc ^ 1, 0); /* not taken: to the fallthrough exit */
            } else {
                /* jp/jnp: PF = even parity of the last result's low byte */
                a64_logic_imm(a, 0, 0, T0, FRES, 0, 8);  /* t = res & 0xff */
                a64_logic(a, 2, 0, 0, T0, T0, T0, 1, 4); /* t ^= t >> 4 */
                a64_logic(a, 2, 0, 0, T0, T0, T0, 1, 2); /* t ^= t >> 2 */
                a64_logic(a, 2, 0, 0, T0, T0, T0, 1, 1); /* t ^= t >> 1: bit 0 = odd parity */
                skip = a64_here(a);
                /* jp: taken when parity even (bit 0 == 0): skip when bit 0 set */
                if (m == M(JP)) a64_emit(a, 0x37000000u | (uint32_t)T0); /* tbnz w0, #0 */
                else a64_emit(a, 0x36000000u | (uint32_t)T0);            /* tbz w0, #0 */
            }
            exit_to(t, target);
            uint32_t *fall = a64_here(a);
            if (acc >= 0) a64_patch_bcond(skip, fall);
            else *skip = (*skip & 0xfff8001fu) | ((uint32_t)(fall - skip) & 0x3fff) << 5;
            exit_to(t, next);
            return 1;
        }
        if (acc < 0) { *handled = 0; return 0; }
        if (cc_kind[m] == 2) { /* setcc */
            a64_msr_nzcv(a, FL);
            a64_cset(a, 0, T0, acc);
            write_op(t, in, &in->op[0], 1, T0);
            return 0;
        }
        /* cmovcc: 32-bit destinations are zero-extended either way */
        const BbOp *dst = &in->op[0];
        const int size = dst->size;
        const int v = read_op(t, in, &in->op[1], size, T0, NULL);
        a64_msr_nzcv(a, FL);
        if (size == 2) { /* 16-bit: the rest of the register stays */
            a64_csel(a, 0, T2, v, G[dst->reg], acc);
            a64_bfi(a, 1, G[dst->reg], T2, 0, 16);
            return 0;
        }
        a64_csel(a, size == 8, G[dst->reg], v, G[dst->reg], acc);
        return 0;
    }
    /* Locked read-modify-writes, xchg with memory: atomics, or the interpreter. */
    if (in->lock || (m == M(XCHG) && (in->op[0].type == OP_MEM || in->op[1].type == OP_MEM))) {
        if (!atomic_rmw(t, in, flags_live)) *handled = 0;
        return 0;
    }
    switch (m) {
    case M(NOP): case M(PAUSE): case M(ENDBR64): case M(PREFETCHNTA): case M(PREFETCHT0):
    case M(PREFETCHT1): case M(PREFETCHT2): case M(PREFETCHW): case M(LFENCE): case M(SFENCE):
        return 0;
    case M(MFENCE): a64_dmb_ish(a); return 0;
    case M(MOV): {
        const BbOp *dst = &in->op[0], *src = &in->op[1];
        if (dst->type == OP_REG && dst->kind != RK_GPR) break;
        if (src->type == OP_REG && src->kind != RK_GPR) break;
        const int size = dst->size;
        const int v = read_op(t, in, src, size, T0, NULL);
        write_op(t, in, dst, size, v);
        return 0;
    }
    case M(MOVZX): {
        const BbOp *dst = &in->op[0], *src = &in->op[1];
        if (dst->type != OP_REG) break;
        const int v = read_op(t, in, src, src->size, T0, NULL);
        write_op(t, in, dst, dst->size, v);
        return 0;
    }
    case M(MOVSX): case M(MOVSXD): {
        const BbOp *dst = &in->op[0], *src = &in->op[1];
        if (dst->type != OP_REG || dst->size < 4 || src->high8) break;
        const int v = read_op(t, in, src, src->size, T0, NULL);
        a64_sxt(a, 1, T1, v, src->size);
        write_op(t, in, dst, dst->size, T1);
        return 0;
    }
    case M(LEA): {
        const BbOp *dst = &in->op[0];
        if (dst->size < 4) break;
        address(t, in, &in->op[1], T0);
        write_op(t, in, dst, dst->size, T0);
        return 0;
    }
    case M(ADD): case M(OR): case M(ADC): case M(SBB): case M(AND): case M(SUB): case M(XOR):
    case M(CMP): case M(TEST):
        if (alu(t, in, flags_live)) return 0;
        break;
    case M(INC): case M(DEC): case M(NEG): case M(NOT):
        if (unary(t, in, flags_live)) return 0;
        break;
    case M(SHL): case M(SHR): case M(SAR):
        if (in->op[0].size < 4) {
            if (shift_small(t, in, flags_live)) return 0;
        } else if (in->count > 1 && in->op[1].type == OP_REG) {
            if (shift_cl(t, in, flags_live)) return 0;
        } else if (shift_imm(t, in, flags_live)) {
            return 0;
        }
        break;
    case M(ROL): case M(ROR):
        if (rotate(t, in, flags_live)) return 0;
        break;
    case M(BT):
        if (bit_test(t, in)) return 0;
        break;
    case M(DIV): case M(IDIV):
        if (divide(t, in)) return 0;
        break;
    case M(MUL):
        if (multiply1(t, in, flags_live)) return 0;
        break;
    case M(TZCNT): case M(LZCNT): case M(BSF): case M(BSR): case M(POPCNT):
        if (bit_count(t, in, flags_live)) return 0;
        break;
    case M(ANDN): case M(BEXTR): case M(BLSI): case M(BLSMSK): case M(BLSR):
        if (bmi1(t, in, flags_live)) return 0;
        break;
    case M(RDTSC):
        if (read_tsc(t)) return 0;
        break;
    case M(IMUL): {
        if (in->count == 1) { if (multiply1(t, in, flags_live)) return 0; break; }
        if (in->count < 2 || in->op[0].size < 4) break;
        const BbOp *dst = &in->op[0];
        const int size = dst->size, sf = size == 8;
        const int x = read_op(t, in, in->count == 3 ? &in->op[1] : dst, size, T0, NULL);
        const int y = in->count == 3 ? (a64_mov_imm(a, T1, (uint64_t)in->op[2].disp), T1)
                                     : read_op(t, in, &in->op[1], size, T1, NULL);
        if (sf) {
            a64_mul(a, 1, T2, x, y);
            if (flags_live) {
                a64_smulh(a, T3, x, y);
                a64_asr_imm(a, 1, T4, T2, 63);
                a64_subs(a, 1, XZR, T3, T4);
            }
        } else {
            a64_sxt(a, 1, T3, x, 4);
            a64_sxt(a, 1, T4, y, 4);
            a64_mul(a, 1, T2, T3, T4);
            if (flags_live) {
                a64_sxt(a, 1, T3, T2, 4);
                a64_subs(a, 1, XZR, T2, T3);
            }
        }
        if (flags_live) {
            /* overflow: CF = OF = 1 (C = 0, V = 1); else C = 1, V = 0 */
            a64_cset(a, 1, T3, CC_NE);
            a64_eor_bit(a, T4, T3, 0);
            a64_lsl_imm(a, 1, T4, T4, 1);
            a64_orr(a, 1, T4, T4, T3); /* bits: C at 1, V at 0 */
            a64_bfi(a, 1, FL, T4, 28, 2);
            a64_mov(a, 1, FRES, T2);
        }
        write_op(t, in, dst, size, T2);
        return 0;
    }
    case M(XCHG): {
        const BbOp *x = &in->op[0], *y = &in->op[1];
        if (x->type != OP_REG || y->type != OP_REG || x->high8 || y->high8) break;
        const int size = x->size;
        const int vx = read_op(t, in, x, size, T0, NULL);
        const int vy = read_op(t, in, y, size, T1, NULL);
        a64_mov(a, 1, T2, vx);
        write_op(t, in, x, size, vy);
        write_op(t, in, y, size, T2);
        return 0;
    }
    case M(CDQE): a64_sxt(a, 1, G[RAX], G[RAX], 4); return 0;
    case M(CWDE): a64_sxt(a, 0, G[RAX], G[RAX], 2); return 0;
    case M(CQO): a64_asr_imm(a, 1, G[RDX], G[RAX], 63); return 0;
    case M(CDQ): a64_sxt(a, 1, T0, G[RAX], 4); a64_asr_imm(a, 0, G[RDX], T0, 31); return 0;
    case M(BSWAP): {
        const BbOp *d = &in->op[0];
        a64_rev(a, d->size == 8, G[d->reg], G[d->reg]);
        return 0;
    }
    case M(PUSH): {
        const BbOp *op = &in->op[0];
        if (op->size == 2 || (op->type == OP_REG && op->kind != RK_GPR)) break;
        int v;
        if (op->type == OP_IMM) { a64_mov_imm(a, T0, (uint64_t)op->disp); v = T0; }
        else v = read_op(t, in, op, 8, T0, NULL);
        if (v == G[RSP]) { a64_mov(a, 1, T0, v); v = T0; }
        push_reg(t, v);
        return 0;
    }
    case M(POP): {
        const BbOp *op = &in->op[0];
        if (op->type != OP_REG || op->size != 8 || op->kind != RK_GPR) break;
        pop_to(t, T0);
        a64_add_imm(a, 1, G[RSP], G[RSP], 8, 0);
        a64_mov(a, 1, G[op->reg], T0);
        return 0;
    }
    case M(LEAVE):
        a64_mov(a, 1, G[RSP], G[RBP]);
        pop_to(t, G[RBP]);
        a64_add_imm(a, 1, G[RSP], G[RSP], 8, 0);
        return 0;
    case M(JMP): {
        const BbOp *op = &in->op[0];
        if (op->type == OP_IMM) { exit_to(t, t->rip + (uint64_t)op->disp); return 1; }
        const int v = read_op(t, in, op, 8, T0, NULL);
        exit_to_reg(t, v);
        return 1;
    }
    case M(CALL): {
        const BbOp *op = &in->op[0];
        /* A game function with a native version (native.c): called from here, no dispatcher. */
        if (op->type == OP_IMM && bbcpu_native_count) {
            const uint64_t callee = t->rip + (uint64_t)op->disp;
            const void *fn = bbcpu_native_at(callee);
            if (fn && !(bbcpu_record_armed && bbcpu_record_target(callee))) {
                a64_mov_imm(a, TA, t->rip);
                a64_mov_imm(a, TB, (uint64_t)(uintptr_t)fn);
                a64_bl(a, (int32_t)(native_stub - a64_here(a)));
                exit_to(t, next);
                return 1;
            }
        }
        /* A recompiled function (recomp.c): its return address pushed, then called from here. */
        if (op->type == OP_IMM && bbcpu_recomp_count) {
            const uint64_t callee = t->rip + (uint64_t)op->disp;
            const RcFn fn = bbcpu_recomp_at(callee);
            if (fn && !(bbcpu_record_armed && bbcpu_record_target(callee))) {
                a64_mov_imm(a, T0, next);
                push_reg(t, T0);
                a64_mov_imm(a, TA, callee);
                a64_mov_imm(a, TB, (uint64_t)(uintptr_t)fn);
                a64_bl(a, (int32_t)(recomp_stub - a64_here(a)));
                exit_to(t, next);
                return 1;
            }
        }
        int target;
        if (op->type == OP_IMM) { a64_mov_imm(a, T1, t->rip + (uint64_t)op->disp); target = T1; }
        else target = read_op(t, in, op, 8, T1, NULL);
        if (target != T1) { a64_mov(a, 1, T1, target); target = T1; }
        a64_mov_imm(a, T0, next);
        push_reg(t, T0);
        exit_to_reg(t, target);
        return 1;
    }
    case M(RET):
        if (in->count) break; /* ret imm16: interpreted */
        pop_to(t, T0);
        a64_add_imm(a, 1, G[RSP], G[RSP], 8, 0);
        exit_to_reg(t, T0);
        return 1;
    default:
        if (!deny_vector && (vector(t, in, flags_live) || vector2(t, in))) return 0;
        break;
    }
    *handled = 0;
    return 0;
}

/* BB_JIT_DUMP=file: every distinct instruction translated natively (hex bytes per line), for
 * tests/fuzz_jit.c. Called under jit_lock. */
static void dump_insn(uint64_t rip, int length) {
    static FILE *out;
    static int state;
    static uint64_t *seen;
    enum { SEEN_BITS = 22 };
    if (state < 0) return;
    if (!state) {
        const char *path = getenv("BB_JIT_DUMP");
        out = path && *path ? fopen(path, "w") : NULL;
        seen = out ? calloc((size_t)1 << SEEN_BITS, 2 * sizeof(uint64_t)) : NULL;
        state = out && seen ? 1 : -1;
        if (state < 0) return;
    }
    uint64_t k[2] = {0, 0};
    memcpy(k, (const void *)rip, (size_t)length);
    k[1] ^= (uint64_t)length << 56;
    uint64_t h = (k[0] * 0x9E3779B97F4A7C15ull) ^ (k[1] * 0xC2B2AE3D27D4EB4Full);
    for (uint64_t i = h >> (64 - SEEN_BITS);; i = (i + 1) & ((1u << SEEN_BITS) - 1)) {
        uint64_t *e = &seen[2 * i];
        if (e[0] == k[0] && e[1] == k[1]) return;
        if (!e[0] && !e[1]) { e[0] = k[0]; e[1] = k[1]; break; /* k[1] != 0: the length is in its top byte */ }
    }
    for (int i = 0; i < length; ++i) fprintf(out, "%02x", ((const uint8_t *)rip)[i]);
    fputc('\n', out);
    fflush(out);
}

/* Instructions bbcpu_step may run from translated code: anything that is not a branch. */
static int interpretable(const BbInsn *in) { return !in->branch; }

/* ---- flag liveness ---- */
enum { FLAGS_ALL = 0x8d5 }; /* CF PF AF ZF SF OF (RFLAGS bits) */

/* Shifts and rotates by cl: a count of 0 leaves the flags as they were. */
static int keeps_flags(const BbInsn *in) {
    const int m = in->mnemonic;
    return in->count > 1 && in->op[1].type == OP_REG && in->op[1].kind == RK_GPR &&
           (m == M(SHL) || m == M(SHR) || m == M(SAR) || m == M(ROL) || m == M(ROR) ||
            m == M(RCL) || m == M(RCR) || m == M(SHLD) || m == M(SHRD));
}
/* The flags an instruction reads: Zydis's tested set, and all of them for those that read the
 * whole register or hand control elsewhere (traps). */
static uint32_t flags_read(const BbInsn *in) {
    switch (in->mnemonic) {
    case M(PUSHFQ): case M(PUSHF): case M(PUSHFD): case M(LAHF): case M(INT3): case M(INT):
    case M(INT1): case M(UD2): case M(HLT): case M(SYSCALL): case M(INVALID):
        return FLAGS_ALL;
    default:
        return in->freads & FLAGS_ALL;
    }
}
/* The flags it certainly replaces (written or left undefined). */
static uint32_t flags_written(const BbInsn *in) {
    return keeps_flags(in) ? 0 : in->fwrites & FLAGS_ALL;
}
/* BB_JIT_FLAGS_ABI=0: flags taken as live across calls and returns (the x86-64 ABI does not keep
 * them, and compiled code never passes values in them). */
static int flags_abi(void) {
    static int on = -1;
    if (on < 0) on = env_flag("BB_JIT_FLAGS_ABI", 1);
    return on;
}
static uint32_t flags_in(uint64_t rip, int depth);
/* The flags read after the branch `in` at `at` before being written: its targets' (one level of
 * blocks is decoded), none across a call or return, all for indirect jumps. */
static uint32_t flags_after_branch(const BbInsn *in, uint64_t at, int depth) {
    const int m = in->mnemonic;
    if ((m == M(CALL) || m == M(RET)) && flags_abi()) return 0;
    if (depth > 0) return FLAGS_ALL;
    const uint64_t next = at + in->length;
    if (m == M(JMP) && in->op[0].type == OP_IMM) return flags_in(at + (uint64_t)in->op[0].disp, depth + 1);
    if (cc_of[m] >= 0 && cc_kind[m] == 1) {
        return flags_in(at + (uint64_t)in->op[0].disp, depth + 1) | flags_in(next, depth + 1);
    }
    return FLAGS_ALL;
}
/* The flags the code at rip may read before writing them. */
static uint32_t flags_in(uint64_t rip, int depth) {
    if (!bbcpu_is_guest_code(rip)) return FLAGS_ALL;
    const BbBlock *b = bbcpu_block(rip);
    uint32_t written = 0, read = 0;
    uint64_t at = b->start;
    for (uint32_t i = 0; i < b->count; ++i) {
        const BbInsn *in = &b->insn[i];
        read |= flags_read(in) & ~written;
        written |= flags_written(in);
        if (written == FLAGS_ALL) return read;
        if (in->branch) return read | (flags_after_branch(in, at, depth) & ~written);
        at += in->length;
    }
    return read | (FLAGS_ALL & ~written); /* a block cut at its size limit: unknown */
}
/* The flags live at the end of a block. */
static uint32_t block_flags_out(const BbBlock *b) {
    uint64_t at = b->start;
    for (uint32_t i = 0; i + 1 < b->count; ++i) at += b->insn[i].length;
    const BbInsn *last = &b->insn[b->count - 1];
    if (last->branch) return flags_after_branch(last, at, 0);
    return flags_in(at + last->length, 1);
}

/* BB_JIT_MAP=file: "host_start host_end guest_rip" per translated block (hex), for attributing
 * CPU samples of translated code to guest code (tools/sample_guest.py). Called under jit_lock. */
static void map_block(const uint32_t *start, const uint32_t *end, uint64_t guest) {
    static FILE *out;
    static int state;
    static unsigned pending;
    if (state < 0) return;
    if (!state) {
        const char *path = getenv("BB_JIT_MAP");
        out = path && *path ? fopen(path, "w") : NULL;
        state = out ? 1 : -1;
        if (!out) return;
    }
    fprintf(out, "%llx %llx %llx\n", (unsigned long long)(uintptr_t)start, (unsigned long long)(uintptr_t)end,
            (unsigned long long)guest);
    if (++pending >= 256) { fflush(out); pending = 0; }
}

static void *translate(BbBlock *block) {
    pthread_mutex_lock(&jit_lock);
    if (block->jit_code || block->jit_failed) {
        void *code = block->jit_code;
        pthread_mutex_unlock(&jit_lock);
        return code;
    }
    /* Flag liveness, per flag: computed only where a later instruction, or a block it continues
     * to, may read them (flags_in_after). */
    uint16_t live_after[64 + 1];
    uint32_t live = block_flags_out(block);
    for (int i = (int)block->count - 1; i >= 0; --i) {
        const BbInsn *in = &block->insn[i];
        live_after[i] = (uint16_t)live;
        live = (live & ~flags_written(in)) | flags_read(in);
    }
    pthread_jit_write_protect_np(0);
    Tx t = {.a = {code_next, code_next, code_end}, .block = block};
    /* Translation metadata only: no instructions or callbacks added to guest execution. */
    static int fault_map_enabled = -1;
    if (fault_map_enabled < 0) {
        const char *env = getenv("BB_JIT_FAULT_MAP");
        fault_map_enabled = env && env[0] == '1';
    }
    if (fault_map_enabled) {
        t.fault_map = calloc(64 + MAX_SLOW, sizeof(*t.fault_map));
        if (!t.fault_map) abort();
    }
    uint32_t *start = code_next;
    uint64_t rip = block->start;
    int ended = 0, translated = 0;
    /* BB_RECORD, native and recompiled functions (record.c, native.c, recomp.c): their first
     * instruction goes through bbcpu_step. A probed block runs its probes first. */
    const int recorded = (bbcpu_record_armed && bbcpu_record_target(block->start)) ||
                         (bbcpu_native_count && bbcpu_native_at(block->start)) ||
                         (bbcpu_recomp_count && bbcpu_recomp_at(block->start));
    if (!recorded && bbcpu_probe_count && bbcpu_probe_at(block->start)) {
        a64_mov_imm(&t.a, TA, block->start);
        a64_bl(&t.a, (int32_t)(probe_stub - a64_here(&t.a)));
    }
    for (uint32_t i = 0; i < block->count && !ended && !recorded; ++i) {
        const BbInsn *in = &block->insn[i];
        t.rip = rip;
        int handled;
        uint32_t *mark = a64_here(&t.a);
        const int slow_mark = t.slow_count;
        ended = translate_insn(&t, in, live_after[i], &handled);
        if (handled) dump_insn(rip, in->length);
        if (!handled) {
            t.a.at = mark;
            t.slow_count = slow_mark; /* its out-of-line accesses went with it */
            if (!interpretable(in)) {
                if (i == 0) break; /* the dispatcher interprets it */
                exit_to(&t, rip);
                ended = 1;
                break;
            }
            fallback(&t, in);
        }
        if (t.fault_map)
            t.fault_map[t.fault_count++] = (InstructionRange){mark, a64_here(&t.a), rip};
        ++translated;
        rip += in->length;
    }
    void *code = NULL;
    if (translated == 0) {
        block->jit_failed = 1;
        free(t.fault_map);
    } else {
        if (!ended) exit_to(&t, rip);
        emit_slow_accesses(&t);
        if (t.a.at > t.a.end) {
            fputs("STOP: bbcpu JIT: code buffer full\n", stderr);
            abort();
        }
        code_next = t.a.at;
        sys_icache_invalidate(start, (size_t)(code_next - start) * 4);
        code = start;
        ++translated_blocks;
        if (t.fault_map) {
            InstructionRange *compact = realloc(t.fault_map, t.fault_count * sizeof(*compact));
            if (compact) t.fault_map = compact;
        }
        CodeRangeChunk *chunk = __atomic_load_n(&range_chunks, __ATOMIC_RELAXED);
        int fresh = !chunk || chunk->count == RANGE_CHUNK_SIZE;
        if (fresh) {
            CodeRangeChunk *next = calloc(1, sizeof(*next));
            if (!next) abort();
            next->next = chunk;
            chunk = next;
        }
        const size_t index = chunk->count;
        chunk->entries[index] = (CodeRange){start, code_next, block, t.fault_map, t.fault_count};
        __atomic_store_n(&chunk->count, index + 1, __ATOMIC_RELEASE);
        if (fresh) __atomic_store_n(&range_chunks, chunk, __ATOMIC_RELEASE);
        map_block(start, code_next, block->start);
    }
    pthread_jit_write_protect_np(1);
    __atomic_store_n(&block->jit_code, code, __ATOMIC_RELEASE);
    pthread_mutex_unlock(&jit_lock);
    return code;
}

/* Block chaining: the exit's branch now goes straight to the next block's code. */
static void link_exit(uint32_t *site, const void *code) {
    const int64_t offset = (const uint32_t *)code - site;
    if (offset < -(1 << 25) || offset >= (1 << 25)) return;
    pthread_mutex_lock(&jit_lock);
    pthread_jit_write_protect_np(0);
    __atomic_store_n(site, 0x14000000u | ((uint32_t)offset & 0x3ffffff), __ATOMIC_RELEASE);
    pthread_jit_write_protect_np(1);
    sys_icache_invalidate(site, 4);
    pthread_mutex_unlock(&jit_lock);
}

int bbcpu_jit_run(BbCpu *cpu, uint64_t stop) {
    if (!jit_init()) return 0;
    static int cc_ready;
    if (!__atomic_load_n(&cc_ready, __ATOMIC_ACQUIRE)) {
        pthread_mutex_lock(&jit_lock);
        if (!cc_ready) { init_cc(); init_deny(); __atomic_store_n(&cc_ready, 1, __ATOMIC_RELEASE); }
        pthread_mutex_unlock(&jit_lock);
    }
    const EnterFn enter = (EnterFn)(void *)enter_stub;
    if (!cpu->jit_cache) {
        cpu->jit_cache = calloc((size_t)1 << CACHE_BITS, sizeof(*cpu->jit_cache));
        if (!cpu->jit_cache) abort();
    }
    x86_to_jit(cpu);
    cpu->jit_link = 0;
    while (cpu->rip != stop) {
        if (__builtin_expect(cpu->rip == BB_GUARD_RETURN, 0)) {
            cpu->jit_link = 0;
            jit_to_x86(cpu);
            bbcpu_guard_return(cpu);
            x86_to_jit(cpu);
            *cache_entry(cpu, BB_GUARD_RETURN) = (struct BbJitCacheEntry){BB_GUARD_RETURN, guard_return_stub};
            continue;
        }
        if (!bbcpu_is_guest_code(cpu->rip)) {
            if (!cpu->rip) {
                const uint64_t site = cpu->jit_link;
                fprintf(stderr, "STOP: bbcpu: null guest branch at %#llx (guest offset %#llx), rsp=%#llx\n",
                        (unsigned long long)site,
                        (unsigned long long)(site ? site - bb_image_base_for_reports() : 0),
                        (unsigned long long)cpu->r[RSP]);
                _Exit(30);
            }
            cpu->jit_link = 0;
            jit_to_x86(cpu);
            if (bbcpu_return_hook) bbcpu_return_hook(cpu);
            const uint64_t function = cpu->rip;
            bbcpu_call_host_at_rip(cpu);
            x86_to_jit(cpu);
            *cache_entry(cpu, function) = (struct BbJitCacheEntry){function, host_function_stub};
            continue;
        }
        BbBlock *block = (BbBlock *)bbcpu_block(cpu->rip);
        void *code = __atomic_load_n(&block->jit_code, __ATOMIC_ACQUIRE);
        if (!code && !block->jit_failed) code = translate(block);
        if (code) {
            if (cpu->jit_link && !deny_chain) link_exit((uint32_t *)(uintptr_t)cpu->jit_link, code);
            *cache_entry(cpu, cpu->rip) = (struct BbJitCacheEntry){cpu->rip, code};
            cpu->jit_link = 0;
            enter(cpu, code);
        } else {
            cpu->jit_link = 0;
            jit_to_x86(cpu);
            cpu->rip = bbcpu_step(cpu, &block->insn[0]);
            x86_to_jit(cpu);
            profile_note(&block->insn[0]);
        }
    }
    jit_to_x86(cpu);
    return 1;
}

void bbcpu_jit_stats(uint64_t *blocks, uint64_t *fallbacks) {
    *blocks = translated_blocks;
    *fallbacks = fallback_insns;
}

/* Whether the JIT translates the instruction itself (tests/fuzz_jit.c): a translation into a
 * scratch buffer that never runs. */
int bbcpu_jit_translates(const BbInsn *in, uint64_t rip) {
    if (!jit_init()) return 0;
    pthread_mutex_lock(&jit_lock);
    static int cc_done;
    if (!cc_done) { init_cc(); init_deny(); cc_done = 1; }
    static uint32_t scratch[8192];
    Tx t = {.a = {scratch, scratch, scratch + 8192}, .block = NULL, .rip = rip};
    int handled;
    translate_insn(&t, in, 1, &handled);
    pthread_mutex_unlock(&jit_lock);
    return handled;
}

static const CodeRange *fault_range(void *context) {
    ucontext_t *uc = context;
    const uint64_t pc = uc->uc_mcontext->__ss.__pc;
    if (!code_base || pc < (uint64_t)(uintptr_t)code_base || pc >= (uint64_t)(uintptr_t)code_next) return NULL;
    for (const CodeRangeChunk *chunk = __atomic_load_n(&range_chunks, __ATOMIC_ACQUIRE);
         chunk; chunk = chunk->next) {
        size_t lo = 0, hi = __atomic_load_n(&chunk->count, __ATOMIC_ACQUIRE);
        const size_t count = hi;
        if (!count || pc < (uint64_t)(uintptr_t)chunk->entries[0].start ||
            pc >= (uint64_t)(uintptr_t)chunk->entries[count - 1].end) continue;
        while (lo < hi) {
            const size_t mid = (lo + hi) / 2;
            if ((uint64_t)(uintptr_t)chunk->entries[mid].end <= pc) lo = mid + 1;
            else hi = mid;
        }
        return lo < count && (uint64_t)(uintptr_t)chunk->entries[lo].start <= pc ?
               &chunk->entries[lo] : NULL;
    }
    return NULL;
}

int bbcpu_fault_guest_regs(void *context, BbGuestRegs *regs) {
    const CodeRange *range = fault_range(context);
    if (!range) return 0;
    ucontext_t *uc = context;
    const uint64_t *x = uc->uc_mcontext->__ss.__x;
    for (unsigned i = 0; i < 16; ++i) regs->r[i] = x[G[i]];
    regs->rip = 0; /* No exact location without translation metadata. */
    const uint64_t pc = uc->uc_mcontext->__ss.__pc;
    for (size_t i = 0; i < range->instruction_count; ++i) {
        const InstructionRange *in = &range->instructions[i];
        if (pc >= (uint64_t)(uintptr_t)in->start && pc < (uint64_t)(uintptr_t)in->end) {
            regs->rip = in->rip;
            break;
        }
    }
    return 1;
}

/* A fault in translated code: the guest block, and the guest registers (fault reports). */
int bbcpu_describe_fault(void *context, char *out, size_t size) {
    const CodeRange *range = fault_range(context);
    if (!range) return 0;
    bbcpu_watch_dump();
    ucontext_t *uc = context;
    const uint64_t pc = uc->uc_mcontext->__ss.__pc;
    BbGuestRegs regs;
    bbcpu_fault_guest_regs(context, &regs);
    const uint64_t *g = regs.r;
    const BbBlock *b = range->block;
    snprintf(out, size,
             "  translated guest block %#llx (guest offset %#llx), +%llu host instructions\n"
             "  exact guest instruction %#llx (guest offset %#llx; zero means unavailable)\n"
             "  rax=%#llx rcx=%#llx rdx=%#llx rbx=%#llx rsp=%#llx rbp=%#llx rsi=%#llx rdi=%#llx\n"
             "  r8=%#llx r9=%#llx r10=%#llx r11=%#llx r12=%#llx r13=%#llx r14=%#llx r15=%#llx\n",
             b ? (unsigned long long)b->start : 0ull, b ? (unsigned long long)(b->start - bb_image_base_for_reports()) : 0ull,
             b ? (unsigned long long)((pc - (uint64_t)(uintptr_t)range->start) / 4) : 0ull,
             (unsigned long long)regs.rip, regs.rip ? (unsigned long long)(regs.rip - bb_image_base_for_reports()) : 0ull,
             (unsigned long long)g[0], (unsigned long long)g[1], (unsigned long long)g[2], (unsigned long long)g[3],
             (unsigned long long)g[4], (unsigned long long)g[5], (unsigned long long)g[6], (unsigned long long)g[7],
             (unsigned long long)g[8], (unsigned long long)g[9], (unsigned long long)g[10], (unsigned long long)g[11],
             (unsigned long long)g[12], (unsigned long long)g[13], (unsigned long long)g[14], (unsigned long long)g[15]);
    return 1;
}

/* Guest accesses use ldapr/stlr (x86 ordering), which fault on Apple Silicon when they cross a
 * 16-byte boundary; x86 code does such accesses freely. The fault handler emulates them: the
 * access as bytes between full barriers, then the next instruction. */
int bbcpu_handle_alignment_fault(void *context) {
    ucontext_t *uc = context;
    const uint64_t pc = uc->uc_mcontext->__ss.__pc;
    if (!code_base || pc < (uint64_t)(uintptr_t)code_base || pc >= (uint64_t)(uintptr_t)code_next) return 0;
    if ((uc->uc_mcontext->__es.__esr & 0x3f) != 0x21) return 0; /* DFSC: alignment fault */
    const uint32_t insn = *(const uint32_t *)pc;
    const int size = 1 << (insn >> 30), rn = (insn >> 5) & 31, rt = insn & 31;
    const int is_load = (insn & 0x3ffffc00u) == 0x38bfc000u, is_store = (insn & 0x3ffffc00u) == 0x089ffc00u;
    if ((!is_load && !is_store) || rn == 31) return 0;
    uint64_t *x = uc->uc_mcontext->__ss.__x; /* x0-x28 */
    const uint64_t address = rn == 29 ? uc->uc_mcontext->__ss.__fp : rn == 30 ? uc->uc_mcontext->__ss.__lr : x[rn];
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
    if (is_load) {
        uint64_t value = 0;
        memcpy(&value, (const void *)address, (size_t)size);
        if (rt < 29) x[rt] = value;
        else if (rt == 29) uc->uc_mcontext->__ss.__fp = value;
    } else {
        const uint64_t value = rt == 31 ? 0 : rt < 29 ? x[rt] : rt == 29 ? uc->uc_mcontext->__ss.__fp : uc->uc_mcontext->__ss.__lr;
        memcpy((void *)address, &value, (size_t)size);
    }
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
    uc->uc_mcontext->__ss.__pc = pc + 4;
    return 1;
}

#else
int bbcpu_fault_guest_regs(void *context, BbGuestRegs *regs) {
    (void)context; (void)regs; return 0;
}
int bbcpu_describe_fault(void *context, char *out, size_t size) {
    (void)context; (void)out; (void)size;
    return 0;
}
int bbcpu_handle_alignment_fault(void *context) {
    (void)context;
    return 0;
}
int bbcpu_jit_run(BbCpu *cpu, uint64_t stop) {
    (void)cpu;
    (void)stop;
    return 0;
}
void bbcpu_jit_stats(uint64_t *blocks, uint64_t *fallbacks) {
    *blocks = 0;
    *fallbacks = 0;
}
#endif
