/* tests/fuzz_recomp.c: differential fuzzing of the recompiler's C (tools/recomp/bbrecomp.c)
 * against the interpreter, one instruction at a time, as tests/fuzz_jit.c does for the JIT.
 * Input: instructions (hex bytes per line: tools/cpu/encodings.c) and the library bbrecomp
 * --snippets made of them (a function per line it translates natively). Each runs from random
 * register, flag, vector and memory states through bbcpu_step_insn and through its generated
 * function; registers, the flags x86 defines for it, vector registers and memory must match.
 * Usage: out/recomp/fuzz_recomp FILE LIB [trials] [max reports]. tools/recomp/fuzz.sh builds and runs it. */
#include <Zydis/Zydis.h>
#include <inttypes.h>
#include <setjmp.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>

#include "cpu/bbcpu.h"
#include "cpu/cpu_internal.h"
#include "cpu/trace.h"
#include "recomp/rc.h"
#include <dlfcn.h>

uint64_t bb_image_base; /* platform.c in the game: fault reports */

void *runtime_low_map(size_t size, int prot) {
    void *p = mmap(NULL, size, prot, MAP_PRIVATE | MAP_ANON, -1, 0);
    return p == MAP_FAILED ? NULL : p;
}

#define CODE_AT UINT64_C(0x30000000000)
#define SCRATCH_AT UINT64_C(0x31000000000)
enum { SCRATCH = 1 << 16, RIP_SIZE = 1 << 15 };
/* Memory for a rip-relative operand: 32 KiB at its target page (rip_at), random like the scratch. */
static uint8_t *rip_mem;
static uint64_t rip_at;
static int uses_rip;

static int map_rip_target(uint64_t target) {
    const uint64_t page = target & ~UINT64_C(0x3fff);
    const uint64_t code_end = UINT64_C(0x30000000000) + 0x4000, scratch_end = UINT64_C(0x31000000000) + SCRATCH;
    if (page < code_end && page + RIP_SIZE > UINT64_C(0x30000000000)) return 0;
    if (page < scratch_end && page + RIP_SIZE > UINT64_C(0x31000000000)) return 0;
    if (target + 32 > page + RIP_SIZE) return 0;
    if (rip_mem && rip_at == page) return 1;
    if (rip_mem) munmap(rip_mem, RIP_SIZE);
    rip_mem = mmap((void *)page, RIP_SIZE, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON | MAP_FIXED, -1, 0);
    if (rip_mem == MAP_FAILED) { rip_mem = NULL; return 0; }
    rip_at = page;
    return 1;
}

static uint64_t rng = 0x9E3779B97F4A7C15ull;
static uint64_t rnd(void) { rng ^= rng << 13; rng ^= rng >> 7; rng ^= rng << 17; return rng; }

static uint64_t interesting(void) {
    static const uint64_t v[] = {0, 1, 2, 7, 8, 31, 32, 63, 64, 0x7f, 0x80, 0xff, 0x100, 0x7fff, 0x8000,
                                 0xffff, 0x7fffffff, 0x80000000, 0xffffffff, 0x100000000ull,
                                 0x7fffffffffffffffull, 0x8000000000000000ull, ~0ull, ~0ull - 1};
    switch (rnd() % 4) {
    case 0: return v[rnd() % (sizeof(v) / sizeof(*v))];
    case 1: return rnd() & 0xff;
    case 2: return rnd() & 0xffffffff;
    default: return rnd();
    }
}
static uint32_t random_float_bits(void) {
    static const uint32_t v[] = {0, 0x80000000, 0x3f800000, 0xbf800000, 0x7f800000, 0xff800000,
                                 0x7fc00000, 0xffc00000, 0x7f800001, 0x00000001, 0x00800000,
                                 0x4f000000, 0xcf000000, 0x5f000000, 0x3f000000};
    switch (rnd() % 3) {
    case 0: return v[rnd() % (sizeof(v) / sizeof(*v))];
    case 1: { float f = (float)((int64_t)(rnd() % 2000001) - 1000000) / 64.0f; uint32_t u; memcpy(&u, &f, 4); return u; }
    default: return (uint32_t)rnd();
    }
}
static uint64_t random_double_bits(void) {
    switch (rnd() % 3) {
    case 0: { double d = (double)((int64_t)(rnd() % 2000001) - 1000000) / 64.0; uint64_t u; memcpy(&u, &d, 8); return u; }
    case 1: { static const uint64_t v[] = {0, 0x8000000000000000ull, 0x3ff0000000000000ull, 0x7ff0000000000000ull,
                                           0x7ff8000000000000ull, 0x41e0000000000000ull, 0xc3e0000000000000ull, 1};
              return v[rnd() % 8]; }
    default: return rnd();
    }
}

static int reg_index(ZydisRegister r) {
    if (r == ZYDIS_REGISTER_NONE) return -1;
    const ZydisRegister l = ZydisRegisterGetLargestEnclosing(ZYDIS_MACHINE_MODE_LONG_64, r);
    if (l >= ZYDIS_REGISTER_RAX && l <= ZYDIS_REGISTER_R15) return (int)(l - ZYDIS_REGISTER_RAX);
    return -2;
}

static sigjmp_buf crash_jump;
static volatile sig_atomic_t guarded;
static void on_crash(int sig) {
    if (guarded) siglongjmp(crash_jump, sig);
    signal(sig, SIG_DFL);
    raise(sig);
}

typedef struct {
    ZydisDecodedInstruction zi;
    ZydisDecodedOperand zo[ZYDIS_MAX_OPERAND_COUNT];
    uint32_t defined_flags;
} Insn;

/* Points every memory operand into the scratch area (registers chosen to make the address).
 * Returns 0 when that cannot be done (rip-relative, absolute, conflicting registers). */
static int place_memory(const Insn *x, BbCpu *cpu) {
    int seen_base = -1;
    for (int i = 0; i < x->zi.operand_count_visible; ++i) {
        const ZydisDecodedOperand *o = &x->zo[i];
        if (o->type != ZYDIS_OPERAND_TYPE_MEMORY || o->mem.type == ZYDIS_MEMOP_TYPE_AGEN) continue;
        if (o->mem.base == ZYDIS_REGISTER_RIP) {
            const int64_t disp = o->mem.disp.has_displacement ? o->mem.disp.value : 0;
            if (!map_rip_target(CODE_AT + x->zi.length + (uint64_t)disp)) return 0;
            uses_rip = 1;
            continue;
        }
        const int base = reg_index(o->mem.base), index = reg_index(o->mem.index);
        if (base == -2 || index == -2) return 0;
        if (o->mem.segment == ZYDIS_REGISTER_FS || o->mem.segment == ZYDIS_REGISTER_GS) {
            cpu->fs_base = cpu->gs_base = 0;
        }
        const int64_t disp = o->mem.disp.has_displacement ? o->mem.disp.value : 0;
        const uint64_t scale = o->mem.scale ? o->mem.scale : 1;
        uint64_t target = SCRATCH_AT + 0x2000 + rnd() % 0x6000;
        /* Atomics are naturally aligned in real code (split locks are rare and slow on x86 too). */
        const int atomic = (x->zi.attributes & ZYDIS_ATTRIB_HAS_LOCK) || x->zi.mnemonic == ZYDIS_MNEMONIC_XCHG ||
                           x->zi.mnemonic == ZYDIS_MNEMONIC_CMPXCHG || x->zi.mnemonic == ZYDIS_MNEMONIC_XADD;
        if (atomic && o->size >= 16) target &= ~(uint64_t)(o->size / 8 - 1);
        if (base >= 0 && seen_base >= 0 && base != seen_base) return 0;
        if (base >= 0 && base == index) {
            target -= (target - (uint64_t)disp) % (scale + 1);
            cpu->r[base] = (target - (uint64_t)disp) / (scale + 1);
        } else if (base >= 0) {
            if (index >= 0) cpu->r[index] = rnd() % 64;
            cpu->r[base] = target - (uint64_t)disp - (index >= 0 ? cpu->r[index] * scale : 0);
        } else if (index >= 0) {
            target -= (target - (uint64_t)disp) % scale;
            cpu->r[index] = (target - (uint64_t)disp) / scale;
        } else {
            return 0;
        }
        seen_base = base;
    }
    /* The stack must stay in the scratch area (push/pop, call). */
    if (cpu->r[RSP] < SCRATCH_AT + 0x100 || cpu->r[RSP] > SCRATCH_AT + SCRATCH - 0x100) return 0;
    return 1;
}

static void random_state(BbCpu *cpu, uint8_t *scratch) {
    for (int i = 0; i < 16; ++i) cpu->r[i] = interesting();
    cpu->r[RSP] = SCRATCH_AT + 0x8000 + (rnd() % 0x100) * 8;
    cpu->flags = 2 | (rnd() & 0x8d5);
    for (int i = 0; i < 16; ++i) {
        const int kind = (int)(rnd() % 3);
        for (int k = 0; k < 8; ++k) cpu->v[i].d[k] = kind == 0 ? random_float_bits() : (uint32_t)rnd();
        if (kind == 2) for (int k = 0; k < 4; ++k) cpu->v[i].q[k] = random_double_bits();
    }
    for (int i = 0; i < SCRATCH; i += 8) { uint64_t v = rnd(); memcpy(scratch + i, &v, 8); }
}

static int compare(const BbCpu *a, const BbCpu *b, const uint8_t *ma, const uint8_t *mb, uint32_t flag_mask,
                   char *out, size_t size) {
    size_t n = 0;
    for (int i = 0; i < 16; ++i)
        if (a->r[i] != b->r[i])
            n += (size_t)snprintf(out + n, size - n, " r%d: interp %#" PRIx64 " recomp %#" PRIx64 ";", i, a->r[i], b->r[i]);
    if ((a->flags ^ b->flags) & flag_mask)
        n += (size_t)snprintf(out + n, size - n, " flags: interp %#" PRIx64 " recomp %#" PRIx64 " (mask %#x);",
                              a->flags & flag_mask, b->flags & flag_mask, flag_mask);
    for (int i = 0; i < 16 && n < size - 200; ++i)
        if (memcmp(&a->v[i], &b->v[i], 32))
            n += (size_t)snprintf(out + n, size - n, " ymm%d: interp %016" PRIx64 "%016" PRIx64 "%016" PRIx64 "%016" PRIx64
                                  " recomp %016" PRIx64 "%016" PRIx64 "%016" PRIx64 "%016" PRIx64 ";", i,
                                  a->v[i].q[3], a->v[i].q[2], a->v[i].q[1], a->v[i].q[0],
                                  b->v[i].q[3], b->v[i].q[2], b->v[i].q[1], b->v[i].q[0]);
    for (int i = 0; i < SCRATCH && n < size - 100; ++i)
        if (ma[i] != mb[i]) {
            n += (size_t)snprintf(out + n, size - n, " mem[+%#x]: interp %02x recomp %02x;", i, ma[i], mb[i]);
            break;
        }
    return n != 0;
}


int main(int argc, char **argv) {
    if (argc < 3) { fprintf(stderr, "usage: fuzz_recomp FILE LIB [trials] [max reports]\n"); return 2; }
    const int trials = argc > 3 ? atoi(argv[3]) : 200, max_reports = argc > 4 ? atoi(argv[4]) : 50;
    static RcApi api;
    api = (RcApi){.version = BB_RECOMP_API_VERSION, .image_base = CODE_AT};
    void *library = dlopen(argv[2], RTLD_NOW | RTLD_LOCAL);
    const RcInit init = library ? (RcInit)dlsym(library, BB_RECOMP_INIT) : NULL;
    if (!init) { fprintf(stderr, "%s: %s\n", argv[2], dlerror()); return 2; }
    size_t snippet_count = 0;
    const RcFunction *snippets = init(&api, &snippet_count);
    static BbCpu warm;
    bbcpu_run(&warm, 0); /* the interpreter's tables are made on its first run */
    FILE *f = fopen(argv[1], "r");
    if (!f) { perror(argv[1]); return 2; }
    uint8_t *code = mmap((void *)CODE_AT, 0x4000, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON | MAP_FIXED, -1, 0);
    uint8_t *scratch = mmap((void *)SCRATCH_AT, SCRATCH, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON | MAP_FIXED, -1, 0);
    uint8_t *mem_jit = malloc(SCRATCH), *mem_start = malloc(SCRATCH);
    uint8_t *rip_jit = malloc(RIP_SIZE), *rip_start = malloc(RIP_SIZE);
    if (code == MAP_FAILED || scratch == MAP_FAILED || !mem_jit || !mem_start) { perror("mmap"); return 2; }
    bbcpu_add_guest_code(CODE_AT, 0x4000);
    signal(SIGSEGV, on_crash);
    signal(SIGBUS, on_crash);
    signal(SIGILL, on_crash);
    signal(SIGTRAP, on_crash);
    ZydisDecoder decoder;
    ZydisDecoderInit(&decoder, ZYDIS_MACHINE_MODE_LONG_64, ZYDIS_STACK_WIDTH_64);
    ZydisFormatter formatter;
    ZydisFormatterInit(&formatter, ZYDIS_FORMATTER_STYLE_ATT);
    BbCpu *ci = calloc(1, sizeof(BbCpu)), *cj = calloc(1, sizeof(BbCpu)), *c0 = calloc(1, sizeof(BbCpu));
    char line[128];
    int tested = 0, skipped = 0, failed = 0, crashed = 0, reports = 0, native = 0;
    uint64_t line_number = 0;
    size_t next_snippet = 0;
    while (fgets(line, sizeof(line), f)) {
        ++line_number;
        /* Only the lines bbrecomp translated natively (the table is in line order). */
        while (next_snippet < snippet_count && snippets[next_snippet].offset < line_number) ++next_snippet;
        if (next_snippet >= snippet_count || snippets[next_snippet].offset != line_number) continue;
        const RcFn generated = snippets[next_snippet].fn;
        uint8_t bytes[16];
        int len = 0;
        for (const char *p = line; p[0] && p[1] && p[0] != '\n' && len < 15; p += 2) {
            unsigned v;
            if (sscanf(p, "%2x", &v) != 1) break;
            bytes[len++] = (uint8_t)v;
        }
        Insn x;
        if (!len || !ZYAN_SUCCESS(ZydisDecoderDecodeFull(&decoder, bytes, (ZyanUSize)len, &x.zi, x.zo))) continue;
        char text[96];
        ZydisFormatterFormatInstruction(&formatter, &x.zi, x.zo, x.zi.operand_count_visible, text, sizeof(text), CODE_AT, NULL);
        const ZydisMnemonic mn = x.zi.mnemonic;
        if (x.zi.meta.branch_type != ZYDIS_BRANCH_TYPE_NONE || mn == ZYDIS_MNEMONIC_RET ||
            ((mn == ZYDIS_MNEMONIC_DIV || mn == ZYDIS_MNEMONIC_IDIV) && x.zo[0].size < 32) || mn == ZYDIS_MNEMONIC_RDTSC ||
            mn == ZYDIS_MNEMONIC_RDTSCP || mn == ZYDIS_MNEMONIC_CPUID || mn == ZYDIS_MNEMONIC_INT3 ||
            mn == ZYDIS_MNEMONIC_UD2 || mn == ZYDIS_MNEMONIC_HLT || mn == ZYDIS_MNEMONIC_SYSCALL ||
            mn == ZYDIS_MNEMONIC_INT || mn == ZYDIS_MNEMONIC_LEAVE || mn == ZYDIS_MNEMONIC_POPFQ ||
            mn == ZYDIS_MNEMONIC_PUSHFQ || mn == ZYDIS_MNEMONIC_XGETBV) { ++skipped; continue; }
        x.defined_flags = 0x8d5;
        if (x.zi.cpu_flags) x.defined_flags &= ~(uint32_t)x.zi.cpu_flags->undefined;
        /* Shifts by cl: CF is undefined past the operand width; the count may also be 0. */
        if ((x.zi.mnemonic == ZYDIS_MNEMONIC_SHL || x.zi.mnemonic == ZYDIS_MNEMONIC_SHR || x.zi.mnemonic == ZYDIS_MNEMONIC_SAR) &&
            x.zo[1].type == ZYDIS_OPERAND_TYPE_REGISTER && x.zo[0].size < 32)
            x.defined_flags &= ~1u;
        /* The instruction, then a jump to `stop` (block end). */
        memset(code, 0x90, 64);
        memcpy(code, bytes, (size_t)len);
        const uint64_t stop = CODE_AT + (uint64_t)len + 5 + 0x40;
        code[len] = 0xe9;
        const int32_t rel = 0x40;
        memcpy(code + len + 1, &rel, 4);
        /* At stop (never run): pushfq, so the JIT's flag liveness keeps every flag of the
         * instruction (it looks at the code a block continues to). */
        code[len + 5 + 0x40] = 0x9c;
        bbcpu_invalidate(CODE_AT, 64);
        const BbBlock *block = bbcpu_block(CODE_AT);

        int bad = 0, placed = 1;
        for (int t = 0; t < trials && !bad; ++t) {
            memset(ci, 0, offsetof(BbCpu, jf_flags));
            random_state(ci, scratch);
            ci->mxcsr = 0x1f80;
            ci->fcw = 0x37f;
            ci->stack_top = SCRATCH_AT + SCRATCH;
            uses_rip = 0;
            if (!place_memory(&x, ci)) { placed = 0; break; }
            if (uses_rip) {
                for (int i = 0; i < RIP_SIZE; i += 8) { uint64_t v = rnd(); memcpy(rip_mem + i, &v, 8); }
            }
            if (mn == ZYDIS_MNEMONIC_DIV || mn == ZYDIS_MNEMONIC_IDIV) {
                /* No #DE (the interpreter stops the process): a nonzero divisor, rdx the extension of
                 * rax, or (unsigned, sometimes) below the divisor: the quotient fits. */
                const int bytes = x.zo[0].size / 8, sgn = mn == ZYDIS_MNEMONIC_IDIV;
                const uint64_t mask = bytes == 8 ? ~UINT64_C(0) : (UINT64_C(1) << (bytes * 8)) - 1;
                uint64_t d = interesting() & mask;
                if (!d || (sgn && d == mask)) d = 7;
                const BbOp *op = &block->insn[0].op[0];
                if (op->type == OP_REG) {
                    if (op->reg == RAX || op->reg == RDX) { placed = 0; break; }
                    ci->r[op->reg] = (ci->r[op->reg] & ~mask) | d;
                } else {
                    if (op->base == RAX || op->base == RDX || op->index == RAX || op->index == RDX) { placed = 0; break; }
                    const uint64_t at = bbcpu_addr(ci, &block->insn[0], op);
                    if (at < SCRATCH_AT || at + 8 > SCRATCH_AT + SCRATCH) { placed = 0; break; }
                    memcpy((void *)at, &d, (size_t)bytes);
                }
                const uint64_t ax = ci->r[RAX] & mask;
                uint64_t dx;
                if (sgn) dx = ((ax >> (bytes * 8 - 1)) & 1) ? mask : 0;
                else dx = rnd() % 4 ? 0 : (rnd() % d) & mask;
                if (sgn && bytes == 8 && ax == UINT64_C(0x8000000000000000) && d == mask) ci->r[RAX] = 1;
                if (bytes == 4) { ci->r[RDX] = (ci->r[RDX] & ~mask) | dx; }
                else ci->r[RDX] = dx;
            }
            if (uses_rip) memcpy(rip_start, rip_mem, RIP_SIZE);
            ci->rip = CODE_AT;
            memcpy(mem_start, scratch, SCRATCH);
            memcpy(c0, ci, sizeof(*c0));
            memcpy(cj, ci, sizeof(*cj));
            const char *stage = "generated code";
            guarded = 1;
            int sig = sigsetjmp(crash_jump, 1);
            if (!sig) {
                if (t == 0) ++native;
                generated(cj);
                memcpy(mem_jit, scratch, SCRATCH);
                memcpy(scratch, mem_start, SCRATCH);
                if (uses_rip) { memcpy(rip_jit, rip_mem, RIP_SIZE); memcpy(rip_mem, rip_start, RIP_SIZE); }
                stage = "interpreter";
                bbcpu_step_insn(ci, &block->insn[0]);
            }
            guarded = 0;
            if (sig) {
                ++crashed;
                if (reports++ < max_reports) printf("CRASH %-40s (%s) signal %d in the %s\n", text, line[0] ? strtok(line, "\n") : "", sig, stage);
                bad = 1;
                break;
            }
            ci->rip = CODE_AT + (uint64_t)len;
            char diff[1024];
            int differs = compare(ci, cj, scratch, mem_jit, x.defined_flags, diff, sizeof(diff));
            if (uses_rip && memcmp(rip_mem, rip_jit, RIP_SIZE)) {
                strncat(diff, " rip-relative memory differs;", sizeof(diff) - strlen(diff) - 1);
                differs = 1;
            }
            if (differs) {
                bad = 1;
                ++failed;
                if (reports++ < max_reports) {
                    printf("FAIL %-40s (%s) trial %d:%s\n", text, strtok(line, "\n"), t, diff);
                    printf("     in:");
                    for (int i = 0; i < 16; ++i) printf(" r%d=%#" PRIx64, i, c0->r[i]);
                    printf(" flags=%#" PRIx64 "\n", c0->flags);
                }
            }
        }
        if (!placed) { ++skipped; continue; }
        ++tested;
    }
    printf("%d instructions tested, %d failed, %d crashed, %d skipped\n", tested, failed, crashed, skipped);
    (void)native;
    return failed || crashed;
}
