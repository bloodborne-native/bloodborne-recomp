/* bbcpu: recompiled game functions (docs/RECOMPILATION.md). BB_RECOMP_LIB names a library of C
 * written by tools/recomp/bbrecomp.c from the player's own eboot (src/recomp/rc.h): its functions
 * replace the translated ones. A call of one reaches the dispatcher (the JIT leaves its first
 * instruction untranslated) and bbcpu_step runs it. BB_RECOMP_OFF=OFFSET,... leaves chosen ones to
 * the translator (to find a wrong one by bisection). */
#include <setjmp.h>
#include <Zydis/Zydis.h>
#include <string.h>
#include "trace.h"
#include "../recomp/rc.h"
#include <dlfcn.h>
#include <pthread.h>
#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>

/* The recompiled functions by address: open addressing with linear probing, at most half full
 * (every indirect call looks one up; chained buckets of 241,000 functions cost a cache miss per
 * link). Filled once by bbcpu_recomp_load before guest code runs; address 0 is an empty slot. */
typedef struct { uint64_t address; RcFn fn; } Recomp;
static Recomp *table;
static uint64_t table_mask;
static int table_shift;
int bbcpu_recomp_count;
uint64_t bbcpu_recomp_calls;

static uint64_t slot_of(uint64_t address) { return (address * 0x9e3779b97f4a7c15ull) >> table_shift; }

RcFn bbcpu_recomp_at(uint64_t address) {
    if (!table) return NULL;
    for (uint64_t i = slot_of(address);; i = (i + 1) & table_mask) {
        if (table[i].address == address) return table[i].fn;
        if (!table[i].address) return NULL;
    }
}

static void reserve(size_t count) {
    int bits = 10;
    while ((UINT64_C(1) << bits) < 2 * (uint64_t)count) ++bits;
    table = calloc(UINT64_C(1) << bits, sizeof(*table));
    if (!table) abort();
    table_mask = (UINT64_C(1) << bits) - 1;
    table_shift = 64 - bits;
}

static void add(uint64_t address, RcFn fn) {
    uint64_t i = slot_of(address);
    while (table[i].address && table[i].address != address) i = (i + 1) & table_mask;
    if (!table[i].address) ++bbcpu_recomp_count;
    table[i] = (Recomp){address, fn};
}

int bbcpu_is_tcb_load(const BbInsn *in) {
    if (in->mnemonic != ZYDIS_MNEMONIC_MOV || in->count != 2) return 0;
    const BbOp *d = &in->op[0], *s = &in->op[1];
    return d->type == OP_REG && d->kind == RK_GPR && d->size == 8 && s->type == OP_MEM && s->size == 8 &&
           s->base == 0xff && s->index == 0xff && ((s->segment == 1 && s->disp == 0) || s->segment == 2);
}

static uint64_t hash_more(uint64_t h, uint64_t start, uint64_t size) {
#define MIX(byte) (h = (h ^ (uint8_t)(byte)) * 0x100000001b3ull)
    for (uint64_t at = start; at < start + size;) {
        BbBlock *block = bbcpu_block_uncached(at);
        if (!block->count) { free(block); break; }
        for (uint32_t i = 0; i < block->count && at < start + size; ++i) {
            const BbInsn *in = &block->insn[i];
            if (bbcpu_is_tcb_load(in)) {
                MIX(0xf5);
                MIX(in->op[0].reg);
            } else {
                for (int b = 0; b < in->length; ++b) MIX(((const uint8_t *)at)[b]);
            }
            at += in->length;
        }
        free(block);
    }
#undef MIX
    return h;
}

uint64_t bbcpu_recomp_hash(uint64_t start, uint64_t size) { return hash_more(0xcbf29ce484222325ull, start, size); }

/* Functions whose entry the runtime hooked (bloodborne-runtime's guest code hooks): the entry
 * jumps to a stub `int3; the replaced instructions; jmp [rip + 0] back`. Such a function stays
 * recompiled when its code with the replaced instructions put back is the generated one: a
 * wrapper of its own runs the trap handlers, as the int3 would, then the recompiled function (or
 * returns at once when a handler sent the guest back to the caller). Without it a hooked function
 * ran in the translator, as did everything it called that a translated caller reaches. */
#define HOOKED_MAX 64
typedef struct { uint64_t entry, trap; RcFn fn; } Hooked;
static Hooked hooked[HOOKED_MAX];
static int hooked_count;

static void run_hooked(BbCpu *cpu, int i) {
    const Hooked *h = &hooked[i];
    BbGuestRegs regs;
    memcpy(regs.r, cpu->r, sizeof(regs.r));
    regs.rip = h->trap + 1;
    if (!bbcpu_trap(cpu, &regs)) {
        fprintf(stderr, "bbcpu: a hooked function's trap at %#llx not handled\n", (unsigned long long)h->trap);
        abort();
    }
    memcpy(cpu->r, regs.r, sizeof(regs.r));
    if (regs.rip != h->trap + 1) { /* the handler returned from the function (rsp past the return address) */
        cpu->rip = regs.rip;
        return;
    }
    cpu->rip = h->entry;
    h->fn(cpu);
}
#define HOOKED(a, b) static void hooked_##a##b(BbCpu *c) { run_hooked(c, (a) * 8 + (b)); }
#define HOOKED8(a) HOOKED(a, 0) HOOKED(a, 1) HOOKED(a, 2) HOOKED(a, 3) HOOKED(a, 4) HOOKED(a, 5) HOOKED(a, 6) HOOKED(a, 7)
HOOKED8(0) HOOKED8(1) HOOKED8(2) HOOKED8(3) HOOKED8(4) HOOKED8(5) HOOKED8(6) HOOKED8(7)
#define HOOKED_NAMES(a) hooked_##a##0, hooked_##a##1, hooked_##a##2, hooked_##a##3, hooked_##a##4, hooked_##a##5, hooked_##a##6, hooked_##a##7
static const RcFn hooked_fns[HOOKED_MAX] = {HOOKED_NAMES(0), HOOKED_NAMES(1), HOOKED_NAMES(2), HOOKED_NAMES(3),
                                            HOOKED_NAMES(4), HOOKED_NAMES(5), HOOKED_NAMES(6), HOOKED_NAMES(7)};

/* The wrapper for a function at `entry` whose code differs from the generated one only by such a
 * hook, or NULL. Every byte of the shape is checked. */
static RcFn hooked_entry(uint64_t entry, uint64_t size, uint64_t hash, RcFn fn) {
    const uint8_t *site = (const uint8_t *)(uintptr_t)entry;
    if (hooked_count == HOOKED_MAX || size < 5 || site[0] != 0xe9) return NULL;
    int32_t rel;
    memcpy(&rel, site + 1, 4);
    const uint64_t trap = entry + 5 + (uint64_t)(int64_t)rel;
    if (!bbcpu_is_guest_code(trap)) return NULL;
    const uint8_t *stub = (const uint8_t *)(uintptr_t)trap;
    if (stub[0] != 0xcc) return NULL;
    static const uint8_t jmp_rip[] = {0xff, 0x25, 0, 0, 0, 0};
    for (uint64_t n = 5; n <= 32 && n <= size; ++n) { /* n: the replaced instructions' length */
        if (memcmp(stub + 1 + n, jmp_rip, sizeof(jmp_rip)) != 0) continue;
        uint64_t back;
        memcpy(&back, stub + 1 + n + sizeof(jmp_rip), 8);
        if (back != entry + n) continue;
        for (uint64_t k = 5; k < n; ++k)
            if (site[k] != 0x90) return NULL;
        const uint64_t h = hash_more(hash_more(0xcbf29ce484222325ull, trap + 1, n), entry + n, size - n);
        if (h != hash) return NULL;
        hooked[hooked_count] = (Hooked){entry, trap, fn};
        return hooked_fns[hooked_count++];
    }
    return NULL;
}

/* BB_RECOMP_THREADS / BB_RECOMP_NOT_THREADS=NAME,... (diagnostics): recompiled functions only on
 * the threads whose name contains one of the first, and none of the second (host thread names). */
static int name_matches(const char *list, const char *name) {
    if (!list) return 0;
    char copy[512];
    snprintf(copy, sizeof(copy), "%s", list);
    for (char *part = strtok(copy, ","); part; part = strtok(NULL, ","))
        if (*part && strstr(name, part)) return 1;
    return 0;
}
int bbcpu_recomp_threads_filtered;
int bbcpu_recomp_thread_named(void) {
    static _Thread_local int ok = -1;
    if (ok < 0) {
        const char *only = getenv("BB_RECOMP_THREADS"), *not = getenv("BB_RECOMP_NOT_THREADS");
        if (!only && !not) {
            ok = 1;
        } else {
            char name[64] = "";
            pthread_getname_np(pthread_self(), name, sizeof(name));
            ok = (!only || name_matches(only, name)) && !name_matches(not, name);
        }
    }
    return ok;
}

/* ---- the runtime the generated code calls (RcApi) ---- */

/* Calls the runtime makes in the translator get a return address of their own, never guest code
 * (FRAME_BASE + 16 * depth): bbcpu_run stops there. A longjmp (or an exception) in the guest can
 * leave several such frames at once: the guest then returns to an outer frame's address, and
 * return_hook goes back to that frame on the host too (the C frames between are left: recompiled
 * code holds nothing). One address for all of them returned into the innermost frame instead. */
#define FRAME_BASE UINT64_C(0x0000700100000000)
/* `back`: the guest return address the frame's own one replaced on the guest stack, for code that
 * reads the caller from there (bbcpu_guest_return). */
typedef struct { jmp_buf jump; uint64_t back; } Frame; /* _setjmp: no signal mask */
static _Thread_local Frame *frames;
static _Thread_local uint32_t frame_depth, frame_capacity;

/* Statistics for the report: calls into the translator, longjmps to an outer frame, deepest. */
static uint64_t translated_runs, frame_jumps, deepest_frame, recomp_image_base, host_calls;
/* Adds to a shared counter in batches of 4096 per thread, as bbcpu_recomp_counted does: every
 * thread adding one at a time to the same line costs as much as a short call. */
static void count_batched(uint64_t *total, uint32_t *pending) {
    if (++*pending == 4096) {
        __atomic_add_fetch(total, *pending, __ATOMIC_RELAXED);
        *pending = 0;
    }
}
static _Thread_local uint32_t pending_runs, pending_host_calls;
/* BB_RECOMP_TARGETS=1: the targets of calls into the translator, and the host functions the
 * import stubs reach, counted (sampled 1 in 64). */
enum { TARGET_SLOTS = 4096 };
typedef struct { uint64_t rip, count; } Target;
static Target targets[TARGET_SLOTS], host_targets[TARGET_SLOTS];
static int count_targets(void) {
    static int value = -1;
    if (value < 0) value = getenv("BB_RECOMP_TARGETS") && getenv("BB_RECOMP_TARGETS")[0] == '1';
    return value;
}
static void note_target_in(Target *table, uint64_t rip) {
    for (uint32_t i = 0, h = (uint32_t)((rip * 0x9e3779b97f4a7c15ull) >> 52); i < 16; ++i) {
        uint64_t *slot = &table[(h + i) % TARGET_SLOTS].rip, expected = 0;
        if (__atomic_load_n(slot, __ATOMIC_RELAXED) == rip ||
            __atomic_compare_exchange_n(slot, &expected, rip, 0, __ATOMIC_RELAXED, __ATOMIC_RELAXED)) {
            __atomic_add_fetch(&table[(h + i) % TARGET_SLOTS].count, 1, __ATOMIC_RELAXED);
            return;
        }
    }
}
static void note_target(uint64_t rip) {
    static _Thread_local uint32_t tick;
    if ((++tick & 63) || !count_targets()) return;
    note_target_in(targets, rip);
}
static void note_host_target(uint64_t host) {
    static _Thread_local uint32_t tick;
    if ((++tick & 63) || !count_targets()) return;
    note_target_in(host_targets, host);
}
/* The ten busiest entries of a table, counts taken (each sample stands for 64). */
static void take_best(Target *table, uint64_t best[10][2]) {
    memset(best, 0, 10 * 2 * sizeof(uint64_t));
    for (int i = 0; i < TARGET_SLOTS; ++i) {
        const uint64_t c = __atomic_exchange_n(&table[i].count, 0, __ATOMIC_RELAXED);
        for (int k = 0; k < 10; ++k)
            if (c > best[k][1]) { memmove(best[k + 1], best[k], (size_t)(9 - k) * sizeof(best[0])); best[k][0] = table[i].rip; best[k][1] = c; break; }
    }
}

uint64_t bbcpu_guest_return(uint64_t value) {
    if (value < FRAME_BASE || value >= FRAME_BASE + (uint64_t)frame_depth * 16 || (value & 15)) return value;
    return frames[(value - FRAME_BASE) / 16].back;
}

static void return_hook(BbCpu *cpu) {
    const uint64_t rip = cpu->rip;
    if (rip < FRAME_BASE || rip >= FRAME_BASE + (uint64_t)frame_depth * 16 || (rip & 15)) return;
    __atomic_add_fetch(&frame_jumps, 1, __ATOMIC_RELAXED);
    _longjmp(frames[(rip - FRAME_BASE) / 16].jump, 1);
}

/* The translator from cpu->rip until the guest returns through `slot` (the return address the
 * caller left there, replaced by this frame's). */
static void run_translated(BbCpu *cpu, uint64_t slot) {
    if (frame_depth == frame_capacity) {
        frame_capacity = frame_capacity ? frame_capacity * 2 : 64;
        frames = realloc(frames, frame_capacity * sizeof(*frames));
        if (!frames) abort();
    }
    const volatile uint32_t depth = frame_depth++;
    count_batched(&translated_runs, &pending_runs);
    note_target(cpu->rip);
    if (depth + 1 > __atomic_load_n(&deepest_frame, __ATOMIC_RELAXED)) __atomic_store_n(&deepest_frame, depth + 1, __ATOMIC_RELAXED);
    const uint64_t stop = FRAME_BASE + (uint64_t)depth * 16;
    frames[depth].back = bb_load(slot, 8);
    bb_store(slot, 8, stop);
    if (_setjmp(frames[depth].jump) == 0) bbcpu_run(cpu, stop);
    frame_depth = depth;
}

/* BB_RECOMP_TARGETS=1 also counts the instructions generated code leaves to the interpreter
 * (rc->step), by mnemonic. */
enum { STEP_MNEMONICS = 2048 };
static uint64_t step_counts[STEP_MNEMONICS];
static uint64_t rc_step(BbCpu *cpu) {
    const BbBlock *block = bbcpu_block(cpu->rip);
    if (count_targets() && block->count && (unsigned)block->insn[0].mnemonic < STEP_MNEMONICS)
        __atomic_add_fetch(&step_counts[block->insn[0].mnemonic], 1, __ATOMIC_RELAXED);
    return bbcpu_step_insn(cpu, &block->insn[0]);
}

/* BB_RECOMP_NODIRECT=1 (diagnostics): calls and tail calls between recompiled functions go
 * through the translator's dispatcher instead of being made directly. */
static int no_direct(void) {
    static int value = -1;
    if (value < 0) {
        const char *env = getenv("BB_RECOMP_NODIRECT");
        value = env && env[0] == '1';
    }
    return value;
}

/* An import stub (jmp [rip+slot]) whose slot holds a host function (the runtime's libraries):
 * that function at once, with the return address on top of the guest stack, without the
 * translator (a _setjmp and its dispatch loop). 1: done, cpu->rip is where it returned to. */
static int call_import(BbCpu *cpu, uint64_t target) {
    if (!bbcpu_is_guest_code(target)) return 0;
    const uint8_t *code = (const uint8_t *)(uintptr_t)target;
    if (code[0] != 0xff || code[1] != 0x25) return 0;
    int32_t disp;
    memcpy(&disp, code + 2, 4);
    const uint64_t host = bb_load(target + 6 + (uint64_t)(int64_t)disp, 8);
    if (!host || bbcpu_is_guest_code(host)) return 0;
    count_batched(&host_calls, &pending_host_calls);
    note_host_target(host);
    cpu->rip = host;
    bbcpu_call_host_at_rip(cpu); /* pops the return address into rip */
    return 1;
}

/* A native version of a game function (native.c), the same way: no translator in between (the
 * game's mutex on iOS, about 130,000 calls a second in play). 1: done, as for call_import. */
static int call_native(BbCpu *cpu, uint64_t target) {
    if (!bbcpu_native_count || bbcpu_record_armed) return 0;
    const void *fn = bbcpu_native_at(target);
    if (!fn) return 0;
    __atomic_add_fetch(&bbcpu_native_calls, 1, __ATOMIC_RELAXED);
    cpu->rip = (uint64_t)(uintptr_t)fn;
    bbcpu_call_host_at_rip(cpu);
    return 1;
}

/* The rest of rc_call and rc_tail, out of line: their recompiled path then needs no stack frame. */
static __attribute__((noinline)) void call_other(BbCpu *cpu, uint64_t target, uint64_t next) {
    if (call_import(cpu, target) || call_native(cpu, target)) return;
    /* The translator (and natives, imports: bbcpu_run calls them). */
    cpu->rip = target;
    run_translated(cpu, cpu->r[RSP]);
    cpu->rip = next;
}
static __attribute__((noinline)) void tail_other(BbCpu *cpu) {
    if (call_import(cpu, cpu->rip) || call_native(cpu, cpu->rip)) return; /* it returns for us */
    const uint64_t back = bb_load(cpu->r[RSP], 8);
    run_translated(cpu, cpu->r[RSP]);
    cpu->rip = back;
}

static void rc_call(BbCpu *cpu, uint64_t target, uint64_t next) {
    const RcFn fn = bbcpu_recomp_at(target);
    cpu->r[RSP] -= 8;
    bb_store(cpu->r[RSP], 8, next);
    if (!fn) {
        call_other(cpu, target, next);
        return;
    }
    bbcpu_recomp_counted(cpu);
    cpu->rip = target;
    fn(cpu);
}

static void rc_tail(BbCpu *cpu) {
    const RcFn fn = bbcpu_recomp_at(cpu->rip);
    if (!fn) {
        tail_other(cpu);
        return;
    }
    bbcpu_recomp_counted(cpu);
    fn(cpu);
}

/* rc_call and rc_tail with BB_RECOMP_NODIRECT or a thread filter: bbcpu_recomp_load gives these
 * to the library only then, so the calls of a normal run (11.5 million a second at the Sickroom
 * lamp) check neither. */
static int recomp_here(void) {
    return !no_direct() && bbcpu_recomp_thread_ok();
}
static void rc_call_checked(BbCpu *cpu, uint64_t target, uint64_t next) {
    if (recomp_here()) {
        rc_call(cpu, target, next);
        return;
    }
    cpu->r[RSP] -= 8;
    bb_store(cpu->r[RSP], 8, next);
    call_other(cpu, target, next);
}
static void rc_tail_checked(BbCpu *cpu) {
    if (recomp_here()) rc_tail(cpu);
    else tail_other(cpu);
}

static void rc_bail(BbCpu *cpu, uint64_t entry_rsp) {
    const uint64_t back = bb_load(entry_rsp, 8);
    run_translated(cpu, entry_rsp);
    cpu->rip = back;
}

/* Calls of recompiled functions per second, every 10 s while there are any. */
static void *report(void *unused) {
    (void)unused;
    for (uint64_t last = 0;;) {
        sleep(10);
        const uint64_t now = __atomic_load_n(&bbcpu_recomp_calls, __ATOMIC_RELAXED);
        static uint64_t last_runs;
        const uint64_t runs = __atomic_load_n(&translated_runs, __ATOMIC_RELAXED);
        if (now != last)
            printf("Recompiled: %.0f calls/s; %.0f/s into the translator, %.0f/s straight to host imports, "
                   "%llu longjmps to outer frames, frames up to %llu deep\n", (double)(now - last) / 10.0,
                   (double)(runs - last_runs) / 10.0, (double)(__atomic_exchange_n(&host_calls, 0, __ATOMIC_RELAXED)) / 10.0,
                   (unsigned long long)__atomic_load_n(&frame_jumps, __ATOMIC_RELAXED),
                   (unsigned long long)__atomic_load_n(&deepest_frame, __ATOMIC_RELAXED));
        last = now;
        last_runs = runs;
        if (count_targets()) {
            uint64_t best[10][2];
            take_best(targets, best);
            {
                uint64_t top[15][2] = {{0}};
                for (int m = 0; m < STEP_MNEMONICS; ++m) {
                    const uint64_t c = __atomic_exchange_n(&step_counts[m], 0, __ATOMIC_RELAXED);
                    for (int k = 0; k < 15; ++k)
                        if (c > top[k][1]) { memmove(top[k + 1], top[k], (size_t)(14 - k) * sizeof(top[0])); top[k][0] = (uint64_t)m; top[k][1] = c; break; }
                }
                printf("Recompiled: interpreted instructions per second:");
                for (int k = 0; k < 15 && top[k][1]; ++k)
                    printf(" %s:%.0f", ZydisMnemonicGetString((ZydisMnemonic)top[k][0]), (double)top[k][1] / 10.0);
                printf("\n");
            }
            printf("Recompiled: calls into the translator by target (x64 samples):");
            for (int k = 0; k < 10 && best[k][1]; ++k)
                printf(" %#llx:%llu", (unsigned long long)(best[k][0] - recomp_image_base), (unsigned long long)best[k][1] * 64);
            printf("\n");
            /* The host functions behind the import stubs: image+offset (atos resolves a static
             * function; dladdr names only exported ones, given when exact). */
            take_best(host_targets, best);
            printf("Recompiled: host imports by function (x64 samples):");
            for (int k = 0; k < 10 && best[k][1]; ++k) {
                Dl_info info;
                if (dladdr((void *)(uintptr_t)best[k][0], &info) && info.dli_fname) {
                    const char *file = strrchr(info.dli_fname, '/');
                    printf(" %s+%#llx", file ? file + 1 : info.dli_fname,
                           (unsigned long long)(best[k][0] - (uintptr_t)info.dli_fbase));
                    if (info.dli_sname && (uintptr_t)info.dli_saddr == best[k][0]) printf("(%s)", info.dli_sname);
                } else {
                    printf(" %#llx", (unsigned long long)best[k][0]);
                }
                printf(":%llu", (unsigned long long)best[k][1] * 64);
            }
            printf("\n");
        }
    }
    return NULL;
}

/* A set of image offsets from an environment variable: "OFFSET,OFFSET..." or "@FILE" (offsets
 * separated by commas, spaces or lines). */
typedef struct { uint64_t *v; size_t n; int given; } OffsetSet;
static int by_value(const void *a, const void *b) {
    const uint64_t x = *(const uint64_t *)a, y = *(const uint64_t *)b;
    return x < y ? -1 : x > y;
}
static OffsetSet offset_set(const char *name) {
    OffsetSet set = {0};
    const char *value = getenv(name);
    if (!value || !*value) return set;
    set.given = 1;
    char *text = NULL;
    if (value[0] == '@') {
        FILE *f = fopen(value + 1, "r");
        if (!f) { fprintf(stderr, "Recompiled: %s: cannot read %s\n", name, value + 1); return set; }
        fseek(f, 0, SEEK_END);
        const long size = ftell(f);
        fseek(f, 0, SEEK_SET);
        text = calloc((size_t)size + 1, 1);
        if (text && fread(text, 1, (size_t)size, f) != (size_t)size) text[0] = 0;
        fclose(f);
    } else {
        text = strdup(value);
    }
    size_t capacity = 0;
    for (char *p = text; p && *p;) {
        char *end;
        const uint64_t v = strtoull(p, &end, 0);
        if (end == p) { ++p; continue; }
        if (set.n == capacity) { capacity = capacity ? capacity * 2 : 256; set.v = realloc(set.v, capacity * sizeof(*set.v)); }
        set.v[set.n++] = v;
        p = end;
    }
    free(text);
    if (set.n) qsort(set.v, set.n, sizeof(*set.v), by_value);
    return set;
}
static int in_set(const OffsetSet *set, uint64_t offset) {
    return set->n && bsearch(&offset, set->v, set->n, sizeof(*set->v), by_value) != NULL;
}

static size_t recomp_used;

/* Code the runtime rewrites while the game runs (live switches): the functions that hold it stay
 * with the translator, whose blocks bbcpu_invalidate drops. */
enum { MAX_EXCLUDED = 64 };
static struct { uint64_t start, end; } excluded[MAX_EXCLUDED];
static int excluded_count;
void bbcpu_recomp_exclude(uint64_t address, size_t size) {
    if (excluded_count == MAX_EXCLUDED) {
        fputs("Recompiled: too many excluded ranges\n", stderr);
        abort();
    }
    excluded[excluded_count].start = address;
    excluded[excluded_count].end = address + size;
    ++excluded_count;
}
static int is_excluded(uint64_t start, uint64_t size) {
    for (int i = 0; i < excluded_count; ++i)
        if (excluded[i].start < start + size && start < excluded[i].end) return 1;
    return 0;
}

size_t bbcpu_recomp_functions(void) {
    return recomp_used;
}

void bbcpu_recomp_load(uint64_t image_base) {
    recomp_image_base = image_base;
    const char *path = getenv("BB_RECOMP_LIB");
    if (!path || !*path) return;
    void *library = dlopen(path, RTLD_NOW | RTLD_LOCAL);
    const RcInit init = library ? (RcInit)dlsym(library, BB_RECOMP_INIT) : NULL;
    if (!init) {
        fprintf(stderr, "Recompiled: %s: %s\n", path, dlerror());
        return;
    }
    bbcpu_return_hook = return_hook;
    bbcpu_recomp_threads_filtered = getenv("BB_RECOMP_THREADS") || getenv("BB_RECOMP_NOT_THREADS");
    const int checked = no_direct() || bbcpu_recomp_threads_filtered;
    static RcApi api;
    api = (RcApi){.version = BB_RECOMP_API_VERSION, .image_base = image_base, .step = rc_step,
                  .call = checked ? rc_call_checked : rc_call, .tail = checked ? rc_tail_checked : rc_tail,
                  .bail = rc_bail};
    size_t count = 0;
    const RcFunction *functions = init(&api, &count);
    reserve(count);
    /* BB_RECOMP_OFF: these not; BB_RECOMP_ONLY: only these (bisecting). */
    const OffsetSet off = offset_set("BB_RECOMP_OFF"), only = offset_set("BB_RECOMP_ONLY");
    size_t used = 0, changed = 0, kept = 0;
    for (size_t i = 0; i < count; ++i) {
        if (in_set(&off, functions[i].offset) || (only.given && !in_set(&only, functions[i].offset))) continue;
        if (is_excluded(image_base + functions[i].offset, functions[i].size)) {
            ++kept;
            continue;
        }
        RcFn fn = functions[i].fn;
        if (bbcpu_recomp_hash(image_base + functions[i].offset, functions[i].size) != functions[i].hash) {
            fn = hooked_entry(image_base + functions[i].offset, functions[i].size, functions[i].hash, fn);
            if (!fn) {
                ++changed; /* patched since (game patches, hooks elsewhere): the translator runs it */
                continue;
            }
        }
        add(image_base + functions[i].offset, fn);
        if (functions[i].direct && !no_direct()) *functions[i].direct = fn;
        ++used;
    }
    recomp_used = used;
    printf("Recompiled: %zu of %zu game functions replaced (%d with a hook at the entry), %zu changed by "
           "patches, %zu rewritten live (%s)\n", used, count, hooked_count, changed, kept, path);
    pthread_t thread;
    if (used && pthread_create(&thread, NULL, report, NULL) == 0) pthread_detach(thread);
}
