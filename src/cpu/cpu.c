/* bbcpu: per-thread guest CPU state and host -> guest calls. */
#include "cpu_internal.h"
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <unistd.h>
#if defined(__aarch64__) && defined(__APPLE__)
#include <sys/sysctl.h>
#elif defined(__aarch64__) && defined(__linux__)
#include <sys/auxv.h>
#endif

/* The guest's stacks stay below 1 TiB like the PS4's (runtime_memory.c). */
void *runtime_low_map(size_t size, int prot);

enum { GUEST_STACK = 1 << 20, RED_ZONE = 128 };
/* Return addresses of host -> guest calls: never guest code, never a host function. */
#define SENTINEL(depth) (UINT64_C(0x0000700000000000) + (uint64_t)(depth) * 16)

static _Thread_local BbCpu *tls_cpu;
static _Thread_local uint64_t tls_thread_pointer;
static uint32_t tls_displacement;
static uint64_t cpus_created;

int bbcpu_enabled(void) {
#if defined(__aarch64__)
    return 1;
#else
    static int enabled = -1;
    if (enabled < 0) {
        const char *env = getenv("BB_CPU");
        enabled = env && env[0] == 'i';
    }
    return enabled;
#endif
}

void bbcpu_set_tls_displacement(uint32_t displacement) { tls_displacement = displacement; }

static void apply_thread_pointer(BbCpu *cpu) {
    cpu->tcb = tls_thread_pointer;
    cpu->fs_base = tls_thread_pointer;
    /* The loader rewrote `mov rax, fs:[0]` into `mov rax, gs:[displacement]`. */
    cpu->gs_base = (uint64_t)(uintptr_t)&cpu->tcb - tls_displacement;
}

void bbcpu_set_thread_pointer(uintptr_t tcb) {
    tls_thread_pointer = tcb;
    if (tls_cpu) apply_thread_pointer(tls_cpu);
}

/* FEAT_AFP: FPCR.FIZ flushes denormal inputs on its own (Apple M3 and later; not M1/M2). */
static int host_has_afp(void) {
    static int afp = -1;
    if (afp < 0) {
        afp = 0;
#if defined(__aarch64__) && defined(__APPLE__)
        int v = 0;
        size_t len = sizeof(v);
        if (sysctlbyname("hw.optional.arm.FEAT_AFP", &v, &len, NULL, 0) == 0) afp = v != 0;
#elif defined(__aarch64__) && defined(__linux__) && defined(HWCAP2_AFP)
        afp = (getauxval(AT_HWCAP2) & HWCAP2_AFP) != 0;
#endif
    }
    return afp;
}

/* Guest MXCSR FTZ (bit 15, flush denormal results) and DAZ (bit 6, denormal inputs read as zero)
 * -> this host thread's FPCR, so the native float code the translator emits flushes denormals
 * as the guest asked. FPCR.FZ (bit 24) flushes inputs and outputs together (FPCR.AH = 0).
 * Exact mappings, on by default:
 *   - FTZ and DAZ: FZ.
 *   - DAZ only, with FEAT_AFP (Apple M3 and later): FIZ (bit 0) flushes inputs alone.
 * Approximate mappings, only with BBCPU_FTZ=1 (neither choice is exact, so by default the thread
 * keeps the host's behaviour):
 *   - FTZ only (Bloodborne's Havok workers, inFAMOUS: MXCSR 0x9f80): FZ. The thread's results are
 *     flushed as on x86, and denormals it loads are flushed too, where x86 would use them.
 *     Without it, its denormal results are kept, which x86 flushes.
 *   - DAZ only, without FEAT_AFP (M1/M2): FZ; results that underflow are flushed too.
 * Exact FTZ needs FPCR.AH = 1, which also changes NaN results and FMIN/FMAX for the thread's
 * host code: a follow-up, switching FPCR at the guest/host boundaries. The MXCSR rounding
 * mode (RC) is not mapped either.
 * BBCPU_FTZ=0 leaves FPCR alone. */
void bbcpu_apply_host_fp(uint32_t mxcsr) {
#if defined(__aarch64__)
    static int mode = -1; /* 0: off, 1: exact mappings (default), 2: approximate ones too */
    if (mode < 0) {
        const char *env = getenv("BBCPU_FTZ");
        mode = env && env[0] == '0' ? 0 : env && env[0] == '1' ? 2 : 1;
    }
    if (!mode) return;
    enum { FIZ = 1u << 0, FZ = 1u << 24 };
    const int ftz = (mxcsr & 0x8000) != 0, daz = (mxcsr & 0x40) != 0;
    uint64_t fpcr, want;
    __asm__ volatile("mrs %0, fpcr" : "=r"(fpcr));
    want = fpcr & ~(uint64_t)(FZ | (host_has_afp() ? FIZ : 0));
    if ((ftz && daz) || (mode == 2 && (ftz || (daz && !host_has_afp())))) want |= FZ;
    else if (daz && host_has_afp()) want |= FIZ;
    if (want != fpcr) __asm__ volatile("msr fpcr, %0" : : "r"(want));
#else
    (void)mxcsr;
#endif
}

/* MXCSR and FCW given to this thread before its BbCpu exists (bbcpu_set_fp_control). */
static _Thread_local struct { int set; uint32_t mxcsr; uint16_t fcw; } tls_fp;
void bbcpu_get_fp_control(uint32_t *mxcsr, uint16_t *fcw) {
    if (tls_cpu) { *mxcsr = tls_cpu->mxcsr; *fcw = tls_cpu->fcw; }
    else if (tls_fp.set) { *mxcsr = tls_fp.mxcsr; *fcw = tls_fp.fcw; }
    else { *mxcsr = 0x1f80; *fcw = 0x37f; }
}
void bbcpu_set_fp_control(uint32_t mxcsr, uint16_t fcw) {
    tls_fp.set = 1; tls_fp.mxcsr = mxcsr; tls_fp.fcw = fcw;
    if (tls_cpu) { tls_cpu->mxcsr = mxcsr; tls_cpu->fcw = fcw; }
    bbcpu_apply_host_fp(mxcsr);
}

static BbWatch *watch_threads;
static _Thread_local char thread_label[32];
static uint64_t watch_target;
void bbcpu_set_thread_label(const char *name) {
    snprintf(thread_label, sizeof(thread_label), "%s", name ? name : "unnamed");
    if (tls_cpu && tls_cpu->watch)
        memcpy(tls_cpu->watch->name, thread_label, sizeof(thread_label));
}

/* Per-thread storage is retained with the CPU for terminating diagnostics. */
static void watch_init(BbCpu *cpu) {
    const char *env = getenv("BB_JIT_WATCH");
    if (!env || !*env) return;
    BbWatch *watch = calloc(1, sizeof(*watch));
    if (!watch) abort();
    snprintf(watch->name, sizeof(watch->name), "%s", *thread_label ? thread_label : "unnamed");
    BbWatch *head = __atomic_load_n(&watch_threads, __ATOMIC_ACQUIRE);
    do { watch->next = head; }
    while (!__atomic_compare_exchange_n(&watch_threads, &head, watch, 0,
                                        __ATOMIC_RELEASE, __ATOMIC_ACQUIRE));
    cpu->watch = watch;
    __atomic_store_n(&watch_target, strtoull(env, NULL, 0), __ATOMIC_RELAXED);
}

void bbcpu_watch_write(uintptr_t address, size_t size) {
#if defined(__aarch64__) && defined(__APPLE__)
    if (!tls_cpu || !tls_cpu->watch) return;
    const uint64_t target = __atomic_load_n(&watch_target, __ATOMIC_RELAXED);
    if (target < address || target - address > size || size - (target - address) < 8) return;
    BbWatch *w = tls_cpu->watch;
    const uint64_t n = w->count;
    BbWatchRecord *r = &w->records[n % WATCH_RECORDS];
    __atomic_store_n(&r->sequence, 0, __ATOMIC_RELEASE);
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
    r->address = target;
    r->value = __atomic_load_n((const uint64_t *)(uintptr_t)target, __ATOMIC_RELAXED);
    r->rip = bb_load(tls_cpu->r[RSP], 8); /* guest return address of the host import */
    __asm__ volatile("mrs %0, cntvct_el0" : "=r"(r->tick));
    __atomic_store_n(&r->sequence, (n + 1) | (UINT64_C(1) << 63), __ATOMIC_RELEASE);
    __atomic_store_n(&w->count, n + 1, __ATOMIC_RELEASE);
#else
    (void)address; (void)size;
#endif
}

void bbcpu_watch_dump(void) {
    for (const BbWatch *w = __atomic_load_n(&watch_threads, __ATOMIC_ACQUIRE); w; w = w->next) {
        const uint64_t count = __atomic_load_n(&w->count, __ATOMIC_ACQUIRE);
        for (uint64_t n = count > WATCH_RECORDS ? count - WATCH_RECORDS : 0; n < count; ++n) {
            const BbWatchRecord *r = &w->records[n % WATCH_RECORDS];
            const uint64_t seq = __atomic_load_n(&r->sequence, __ATOMIC_ACQUIRE);
            if ((seq & ~(UINT64_C(1) << 63)) != n + 1) continue;
            const uint64_t tick = __atomic_load_n(&r->tick, __ATOMIC_RELAXED);
            const uint64_t rip = __atomic_load_n(&r->rip, __ATOMIC_RELAXED);
            const uint64_t value = __atomic_load_n(&r->value, __ATOMIC_RELAXED);
            const uint64_t address = __atomic_load_n(&r->address, __ATOMIC_RELAXED);
            __atomic_thread_fence(__ATOMIC_ACQUIRE);
            if (__atomic_load_n(&r->sequence, __ATOMIC_ACQUIRE) != seq) continue;
            char line[224];
            const int len = snprintf(line, sizeof(line),
                "  JIT watch thread %s tick %llu %s rip %#llx address %#llx value %#llx seq %llu\n",
                w->name, (unsigned long long)tick, seq >> 63 ? "store" : "load",
                (unsigned long long)rip, (unsigned long long)address, (unsigned long long)value, (unsigned long long)(n + 1));
            if (len > 0) { ssize_t written = write(2, line, (size_t)len); (void)written; }
        }
    }
}

static BbCpu *current_cpu(void) {
    if (tls_cpu) return tls_cpu;
    BbCpu *cpu = calloc(1, sizeof(*cpu));
    if (!cpu) abort();
    cpu->mxcsr = tls_fp.set ? tls_fp.mxcsr : 0x1f80;
    bbcpu_apply_host_fp(cpu->mxcsr);
    cpu->fcw = tls_fp.set ? tls_fp.fcw : 0x37f;
    cpu->flags = 2;
    apply_thread_pointer(cpu);
    __atomic_add_fetch(&cpus_created, 1, __ATOMIC_RELAXED);
    watch_init(cpu);
    tls_cpu = cpu;
    return cpu;
}

uint64_t bbcpu_guest_rip(void) { return tls_cpu ? tls_cpu->rip : 0; }

uint64_t bbcpu_import_return(void) {
    return tls_cpu && tls_cpu->r[RSP] ? bbcpu_guest_return(bb_load(tls_cpu->r[RSP], 8)) : 0;
}

static uint64_t run_call(BbCpu *cpu, uintptr_t fn, int count, const uint64_t *args, uint64_t rsp) {
    static const int arg_regs[6] = {RDI, RSI, RDX, RCX, R8, R9};
    if (count > 6) {
        fputs("bbcpu: more than 6 arguments to a guest call\n", stderr);
        abort();
    }
    for (int i = 0; i < count; ++i) cpu->r[arg_regs[i]] = args[i];
    const uint64_t stop = SENTINEL(cpu->depth);
    rsp &= ~UINT64_C(15);
    rsp -= 8;
    bb_store(rsp, 8, stop); /* the return address: rsp % 16 == 8 at the callee's entry */
    cpu->r[RSP] = rsp;
    cpu->rip = fn;
    bbcpu_run(cpu, stop);
    return cpu->r[RAX];
}

/* Nested calls (a guest -> host call calling back into the guest) keep the outer state. */
uint64_t bbcpu_call(uintptr_t fn, int count, const uint64_t *args) {
    BbCpu *cpu = current_cpu();
    if (!bbcpu_is_guest_code(fn)) {
        fprintf(stderr, "bbcpu: call of %p, not guest code\n", (void *)fn);
        abort();
    }
    BbCpu saved;
    uint64_t rsp;
    if (cpu->depth == 0) {
        static _Thread_local uint8_t *stack;
        if (!stack) {
            stack = runtime_low_map(GUEST_STACK, PROT_READ | PROT_WRITE);
            if (!stack) abort();
        }
        rsp = (uint64_t)(uintptr_t)(stack + GUEST_STACK - 64);
        cpu->stack_top = (uint64_t)(uintptr_t)(stack + GUEST_STACK);
    } else {
        saved = *cpu;
        rsp = cpu->r[RSP] - RED_ZONE - 64;
    }
    ++cpu->depth;
    const uint64_t result = run_call(cpu, fn, count, args, rsp);
    --cpu->depth;
    if (cpu->depth) {
        const uint64_t instructions = cpu->instructions;
        const uint32_t recomp_pending = cpu->recomp_pending;
        const uint64_t rax = cpu->r[RAX];
        const BbVec xmm0 = cpu->v[0];
        *cpu = saved;
        cpu->instructions = instructions;
        cpu->recomp_pending = recomp_pending;
        (void)rax;
        (void)xmm0;
    }
    return result;
}

void bbcpu_call_full(uintptr_t fn, const uint64_t gpr[6], const uint8_t xmm[8][16], uint64_t out_gpr[2],
                     uint8_t out_xmm[2][16]) {
    BbCpu *cpu = current_cpu();
    /* bbcpu_call sets the integer arguments; the vector ones go in first (a nested call keeps
     * the outer state, vectors included, and restores it after). */
    if (cpu->depth) {
        BbCpu saved = *cpu;
        for (int i = 0; i < 8; ++i) memcpy(cpu->v[i].b, xmm[i], 16);
        ++cpu->depth;
        out_gpr[0] = run_call(cpu, fn, 6, gpr, cpu->r[RSP] - RED_ZONE - 64);
        --cpu->depth;
        out_gpr[1] = cpu->r[RDX];
        memcpy(out_xmm[0], cpu->v[0].b, 16);
        memcpy(out_xmm[1], cpu->v[1].b, 16);
        const uint64_t instructions = cpu->instructions;
        const uint32_t recomp_pending = cpu->recomp_pending;
        *cpu = saved;
        cpu->instructions = instructions;
        cpu->recomp_pending = recomp_pending;
        return;
    }
    for (int i = 0; i < 8; ++i) memcpy(cpu->v[i].b, xmm[i], 16);
    out_gpr[0] = bbcpu_call(fn, 6, gpr);
    out_gpr[1] = cpu->r[RDX];
    memcpy(out_xmm[0], cpu->v[0].b, 16);
    memcpy(out_xmm[1], cpu->v[1].b, 16);
}

uint64_t bbcpu_call_on_stack(uintptr_t fn, int count, const uint64_t *args, void *stack, size_t size) {
    BbCpu *cpu = current_cpu();
    ++cpu->depth;
    cpu->stack_top = (uint64_t)(uintptr_t)stack + size;
    const uint64_t result = run_call(cpu, fn, count, args, (uint64_t)(uintptr_t)stack + size - 64);
    --cpu->depth;
    return result;
}

_Noreturn void bbcpu_fatal(BbCpu *cpu, const BbInsn *in, const char *what) {
    char bytes[64] = {0};
    const uint8_t *code = (const uint8_t *)cpu->rip;
    for (int i = 0; i < (in ? in->length : 8) && i < 15; ++i)
        snprintf(bytes + i * 3, sizeof(bytes) - (size_t)i * 3, "%02x ", code[i]);
    fprintf(stderr, "STOP: bbcpu: %s: %s at guest %#" PRIx64 " (bytes %s)\n", what,
            in ? ZydisMnemonicGetString((ZydisMnemonic)in->mnemonic) : "?", cpu->rip, bytes);
    fprintf(stderr, "  rax=%#" PRIx64 " rbx=%#" PRIx64 " rcx=%#" PRIx64 " rdx=%#" PRIx64
                    " rsi=%#" PRIx64 " rdi=%#" PRIx64 " rbp=%#" PRIx64 " rsp=%#" PRIx64 "\n",
            cpu->r[RAX], cpu->r[RBX], cpu->r[RCX], cpu->r[RDX], cpu->r[RSI], cpu->r[RDI],
            cpu->r[RBP], cpu->r[RSP]);
    fflush(NULL);
    _exit(30);
}

void bbcpu_decode_stats(uint64_t *blocks, uint64_t *insns);
void bbcpu_jit_stats(uint64_t *blocks, uint64_t *fallbacks);
void bbcpu_report(void) {
    uint64_t blocks, insns, translated, fallbacks;
    bbcpu_decode_stats(&blocks, &insns);
    bbcpu_jit_stats(&translated, &fallbacks);
    printf("bbcpu: %" PRIu64 " threads, %" PRIu64 " blocks / %" PRIu64 " instructions decoded, %"
           PRIu64 " blocks translated, %" PRIu64 " instructions interpreted from translated code\n",
           cpus_created, blocks, insns, translated, fallbacks);

}
