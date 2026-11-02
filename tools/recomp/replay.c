/* tools/recomp/replay.c RECORDS [--elf ELF] [--only N] [--verbose]: replays recorded calls of a game
 * function (BB_RECORD, src/cpu/record.c) and checks the result (docs/RECOMPILATION.md).
 *
 * Each record runs in a child process: the game image is mapped where it was, memory is rebuilt
 * from the record's observations, epoch by epoch, and the function runs in the interpreter. Its
 * calls are not run: each must match the recorded one (target and argument registers) and is
 * answered with the recorded registers and the memory observed after it. At the end the
 * registers and the bytes the function wrote must match the record.
 *
 * Replaying the original function checks the record itself. With --native LIB the library's
 * version of the function runs instead (natively): its calls are answered the same way, and what it
 * wrote is found by comparing memory with a copy (the original's stack frame aside: the native
 * function has its own; a callee's writes to a buffer in the frame go to the buffer it passed).
 * With --recomp LIB the recompiled version runs (tools/recomp/recomp.sh): the same, compared as
 * strictly as the original (it uses the guest's stack and registers as the original does).
 * Build: tools/recomp/replay.sh. */
#include "cpu/trace.h"
#include "../../src/native/bbnative.h"
#include "recomp/rc.h"
#include <dlfcn.h>
#include <inttypes.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <signal.h>
#include <sys/mman.h>
#include <sys/ucontext.h>
#include <sys/wait.h>
#include <unistd.h>

/* The game image's base: the record's (record.c and the JIT read it). */
uint64_t bb_image_base;

void *runtime_low_map(size_t size, int prot) {
    void *p = mmap(NULL, size, prot, MAP_PRIVATE | MAP_ANON, -1, 0);
    return p == MAP_FAILED ? NULL : p;
}

static int verbose, no_fork;

/* A crash during a replay: where, for the report. */
static void on_crash(int signal, siginfo_t *info, void *context) {
    const ucontext_t *uc = context;
    char line[160];
    const int n = snprintf(line, sizeof(line), "    crashed (signal %d) at %p, pc %#llx\n", signal, info->si_addr,
                           (unsigned long long)uc->uc_mcontext->__ss.__pc);
    write(1, line, (size_t)n);
    _exit(3);
}

/* ---- the record ---- */

typedef struct {
    BbRecHeader header;
    BbRecRegs entry;
    const uint8_t *events; /* header.bytes bytes */
} Record;

static const BbRecEvent *event_at(const Record *r, uint64_t offset) {
    return offset < r->header.bytes ? (const BbRecEvent *)(r->events + offset) : NULL;
}
static uint64_t event_next(const Record *r, uint64_t offset) {
    return offset + sizeof(BbRecEvent) + event_at(r, offset)->length;
}
static const void *event_data(const BbRecEvent *e) { return e + 1; }

/* ---- bytes written (address -> last value), open addressing ---- */

typedef struct { uint64_t *keys; uint8_t *values; size_t capacity, count; } ByteMap;

static void map_put(ByteMap *m, uint64_t address, uint8_t value);
static void map_grow(ByteMap *m) {
    ByteMap bigger = {calloc(m->capacity ? m->capacity * 2 : 4096, 8), NULL, m->capacity ? m->capacity * 2 : 4096, 0};
    bigger.values = calloc(bigger.capacity, 1);
    if (!bigger.keys || !bigger.values) abort();
    for (size_t i = 0; i < m->capacity; ++i)
        if (m->keys[i]) map_put(&bigger, m->keys[i] - 1, m->values[i]);
    free(m->keys);
    free(m->values);
    *m = bigger;
}
static size_t map_slot(const ByteMap *m, uint64_t address) {
    size_t i = (size_t)((address * 0x9e3779b97f4a7c15ull) >> 20) & (m->capacity - 1);
    while (m->keys[i] && m->keys[i] != address + 1) i = (i + 1) & (m->capacity - 1);
    return i;
}
static void map_put(ByteMap *m, uint64_t address, uint8_t value) {
    if ((m->count + 1) * 2 > m->capacity) map_grow(m);
    const size_t i = map_slot(m, address);
    if (!m->keys[i]) {
        m->keys[i] = address + 1;
        ++m->count;
    }
    m->values[i] = value;
}
static int map_get(const ByteMap *m, uint64_t address, uint8_t *value) {
    if (!m->capacity) return 0;
    const size_t i = map_slot(m, address);
    if (!m->keys[i]) return 0;
    *value = m->values[i];
    return 1;
}

/* ---- the replay ---- */

typedef struct {
    BbTrace trace;
    const Record *record;
    uint64_t cursor; /* the next event not consumed */
    ByteMap written;
    ByteMap kept; /* bytes of written operands left as they were (the recorder may have seen them change) */
    uint8_t pre[8][64];
    uint32_t pre_count, post_count;
    int failures;
} Replay;

static void fail(Replay *p, const char *format, ...) __attribute__((format(printf, 2, 3)));
static void fail(Replay *p, const char *format, ...) {
    if (p->failures++ < 8 || verbose) {
        va_list args;
        va_start(args, format);
        printf("    ");
        vprintf(format, args);
        printf("\n");
        va_end(args);
    }
}

/* The memory the function finds in the epoch starting at `offset`: its observations, up to the
 * next call or the end. */
static void apply_epoch(Replay *p, uint64_t offset) {
    const Record *r = p->record;
    const uint64_t tcb = r->header.tcb_address;
    for (const BbRecEvent *e; (e = event_at(r, offset)); offset = event_next(r, offset)) {
        if (e->kind == BBREC_CALL || e->kind == BBREC_EXIT) break;
        if (e->kind != BBREC_OBSERVE) continue;
        if (e->address + e->length > tcb && e->address < tcb + 8) continue; /* the TCB slot: ours */
        memcpy((void *)e->address, event_data(e), e->length);
    }
}

static void replay_access(BbTrace *t, int kind, uint64_t address, uint32_t length) {
    Replay *p = (Replay *)t;
    if (kind == BB_TRACE_PRE) {
        if (p->pre_count < 8) memcpy(p->pre[p->pre_count], (const void *)address, length);
        ++p->pre_count;
        return;
    }
    if (kind == BB_TRACE_POST) {
        const uint32_t i = p->post_count++;
        if (p->post_count == p->pre_count) p->pre_count = p->post_count = 0;
        /* Like the recorder: an operand with a changed byte is written whole. */
        if (i < 8 && !memcmp(p->pre[i], (const void *)address, length)) {
            for (uint32_t b = 0; b < length; ++b) map_put(&p->kept, address + b, ((const uint8_t *)address)[b]);
            return;
        }
        for (uint32_t b = 0; b < length; ++b) map_put(&p->written, address + b, ((const uint8_t *)address)[b]);
        return;
    }
    if (kind == BB_TRACE_WRITE)
        for (uint32_t b = 0; b < length; ++b) map_put(&p->written, address + b, ((const uint8_t *)address)[b]);
}

static const char *const reg_names[16] = {"rax", "rcx", "rdx", "rbx", "rsp", "rbp", "rsi", "rdi",
                                          "r8", "r9", "r10", "r11", "r12", "r13", "r14", "r15"};

static void replay_call(BbTrace *t, const BbInsn *in, uint64_t target, int how) {
    Replay *p = (Replay *)t;
    const Record *r = p->record;
    BbCpu *cpu = t->cpu;
    (void)in;
    p->pre_count = p->post_count = 0;
    const BbRecEvent *e;
    while ((e = event_at(r, p->cursor)) && e->kind != BBREC_CALL && e->kind != BBREC_EXIT)
        p->cursor = event_next(r, p->cursor);
    if (!e || e->kind != BBREC_CALL) {
        fail(p, "call of %#" PRIx64 " that the record does not have", target);
        cpu->rip = 0;
        return;
    }
    const BbRecRegs *call = event_data(e);
    if (e->address != target) fail(p, "call of %#" PRIx64 ", recorded %#" PRIx64, target, e->address);
    if ((how == BB_TRACE_TAIL) != e->tail) fail(p, "call of %#" PRIx64 ": tail call differs", target);
    static const int args[6] = {RDI, RSI, RDX, RCX, R8, R9};
    for (int i = 0; i < 6; ++i)
        if (cpu->r[args[i]] != call->r[args[i]])
            fail(p, "call of %#" PRIx64 ": %s %#" PRIx64 ", recorded %#" PRIx64, target,
                 reg_names[args[i]], cpu->r[args[i]], call->r[args[i]]);
    p->cursor = event_next(r, p->cursor);
    e = event_at(r, p->cursor);
    if (!e || e->kind != BBREC_RETURN) {
        fail(p, "record: no return after the call of %#" PRIx64, target);
        cpu->rip = 0;
        return;
    }
    bbcpu_trace_set_regs(cpu, event_data(e));
    p->cursor = event_next(r, p->cursor);
    apply_epoch(p, p->cursor);
}

static void replay_unsupported(BbTrace *t, const BbInsn *in) {
    (void)in;
    fail((Replay *)t, "an instruction the tracer cannot follow");
}

/* The game image's mapping (map_image) and its writable part. */
static uint64_t image_lo, image_hi, image_data_lo, image_data_hi;
/* The pages map_pages mapped (native replays compare them before and after). */
static uint64_t *tracked;
static size_t tracked_count, tracked_capacity;

/* Maps the pages the record touches; returns an address whose page this process already uses for
 * itself (the record cannot be replayed here), or 0. */
static uint64_t map_pages(const Record *r) {
    ByteMap mine = {0}; /* page numbers mapped here */
    for (uint64_t offset = 0; event_at(r, offset); offset = event_next(r, offset)) {
        const BbRecEvent *e = event_at(r, offset);
        if (e->kind != BBREC_OBSERVE && e->kind != BBREC_WRITE) continue;
        if (e->address + e->length > r->header.tcb_address && e->address < r->header.tcb_address + 8) continue;
        for (uint64_t page = e->address & ~UINT64_C(16383); page < e->address + e->length; page += 16384) {
            uint8_t seen;
            if (map_get(&mine, page >> 14, &seen)) continue;
            if (page >= image_lo && page < image_hi) continue; /* the image, mapped already */
            void *want = (void *)page;
            void *got = mmap(want, 16384, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
            if (got != want) {
                if (got != MAP_FAILED) munmap(got, 16384);
                return e->address;
            }
            map_put(&mine, page >> 14, 1);
            if (tracked_count == tracked_capacity) {
                tracked_capacity = tracked_capacity ? tracked_capacity * 2 : 256;
                tracked = realloc(tracked, tracked_capacity * sizeof(*tracked));
                if (!tracked) abort();
            }
            tracked[tracked_count++] = page;
        }
    }
    return 0;
}

/* The bytes the function wrote, in their final state: its last write, or what a callee left there
 * after it (observed later). Bytes for which `skip` says so are left out. */
/* BB_REPLAY_WATCH=ADDRESS: what the replay sees happen to one byte (debugging the replay). */
static uint64_t watch_address(void) {
    static uint64_t value = 1;
    if (value == 1) value = getenv("BB_REPLAY_WATCH") ? strtoull(getenv("BB_REPLAY_WATCH"), NULL, 0) : 0;
    return value;
}

static void expected_writes(const Record *r, ByteMap *expected, uint64_t skip_lo, uint64_t skip_hi) {
    for (uint64_t offset = 0; event_at(r, offset); offset = event_next(r, offset)) {
        const BbRecEvent *e = event_at(r, offset);
        if (e->kind != BBREC_WRITE && e->kind != BBREC_OBSERVE) continue;
        for (uint32_t b = 0; b < e->length; ++b) {
            const uint64_t address = e->address + b;
            if (address >= skip_lo && address < skip_hi) continue;
            if (address == watch_address())
                printf("    watch: %s %#x in the record (event at %#" PRIx64 ")\n",
                       e->kind == BBREC_WRITE ? "written" : "observed", ((const uint8_t *)event_data(e))[b], offset);
            uint8_t old;
            if (e->kind == BBREC_WRITE || map_get(expected, address, &old))
                map_put(expected, address, ((const uint8_t *)event_data(e))[b]);
        }
    }
}

/* ---- native versions (--native LIB) ---- */

static const BbNativeFunction *native_function;

/* The native replay: memory compared with a copy (the baseline) to see what the function wrote. */
typedef struct {
    const Record *record;
    uint64_t cursor;
    uint64_t frame_lo, frame_hi; /* the original's stack frame: below its entry rsp */
    uint8_t **baseline;          /* per tracked page, then the image's data pages */
    size_t pages;
    ByteMap written;
    struct { uint64_t recorded, native; } pointers[16]; /* arguments into the frame, and ours */
    int pointer_count;
    int failures;
    int same_frame; /* recompiled code: the guest's own stack, as the original */
} NativeReplay;
static NativeReplay *active;

static uint64_t page_address(const NativeReplay *n, size_t i) {
    return i < tracked_count ? tracked[i] : image_data_lo + (uint64_t)(i - tracked_count) * 16384;
}

/* What changed since the last call: the function's writes. */
static void collect(NativeReplay *n) {
    for (size_t i = 0; i < n->pages; ++i) {
        uint8_t *now = (uint8_t *)page_address(n, i), *was = n->baseline[i];
        if (!memcmp(now, was, 16384)) continue;
        for (uint32_t b = 0; b < 16384; ++b)
            if (now[b] != was[b]) {
                if ((uint64_t)(uintptr_t)now + b == watch_address())
                    printf("    watch: written %#x (was %#x) by the function\n", now[b], was[b]);
                map_put(&n->written, (uint64_t)(uintptr_t)now + b, now[b]);
            }
        memcpy(was, now, 16384);
    }
}

static uint8_t *baseline_of(NativeReplay *n, uint64_t address) {
    const uint64_t page = address & ~UINT64_C(16383);
    if (page >= image_data_lo && page < image_data_hi)
        return n->baseline[tracked_count + (page - image_data_lo) / 16384] + (address - page);
    for (size_t i = 0; i < tracked_count; ++i)
        if (tracked[i] == page) return n->baseline[i] + (address - page);
    return NULL;
}

/* The memory of the epoch at `offset` (as apply_epoch), into memory and baseline. Memory in the
 * original's frame goes to the native function's buffer passed for it, when there is one. */
static void native_epoch(NativeReplay *n, uint64_t offset) {
    const Record *r = n->record;
    for (const BbRecEvent *e; (e = event_at(r, offset)); offset = event_next(r, offset)) {
        if (e->kind == BBREC_CALL || e->kind == BBREC_EXIT) break;
        if (e->kind != BBREC_OBSERVE) continue;
        if (e->address + e->length > r->header.tcb_address && e->address < r->header.tcb_address + 8) continue;
        const uint8_t *data = event_data(e);
        for (uint32_t b = 0; b < e->length; ++b) {
            uint64_t address = e->address + b;
            if (!n->same_frame && address >= n->frame_lo && address < n->frame_hi) {
                int best = -1;
                for (int i = 0; i < n->pointer_count; ++i)
                    if (n->pointers[i].recorded <= address && address - n->pointers[i].recorded < 4096 &&
                        (best < 0 || n->pointers[i].recorded > n->pointers[best].recorded))
                        best = i;
                if (best < 0) continue; /* the original's own frame: not the native function's */
                *(uint8_t *)(n->pointers[best].native + (address - n->pointers[best].recorded)) = data[b];
                continue;
            }
            if (address == watch_address())
                printf("    watch: %#x applied from the record (baseline %s)\n", data[b], baseline_of(n, address) ? "kept" : "MISSING");
            *(uint8_t *)address = data[b];
            uint8_t *base = baseline_of(n, address);
            if (base) *base = data[b];
            /* A byte the function wrote that a callee changed: its state now. */
            uint8_t old;
            if (map_get(&n->written, address, &old)) map_put(&n->written, address, data[b]);
        }
    }
}

static void native_fail(NativeReplay *n, const char *format, ...) __attribute__((format(printf, 2, 3)));
static void native_fail(NativeReplay *n, const char *format, ...) {
    if (n->failures++ < 8 || verbose) {
        va_list args;
        va_start(args, format);
        printf("    ");
        vprintf(format, args);
        printf("\n");
        va_end(args);
    }
}

/* api.call: a call the native function makes, answered from the record. */
static void native_call(uint64_t fn, const BbCallIn *in, BbCallOut *out) {
    NativeReplay *n = active;
    const Record *r = n->record;
    collect(n);
    memset(out, 0, sizeof(*out));
    const BbRecEvent *e;
    while ((e = event_at(r, n->cursor)) && e->kind != BBREC_CALL && e->kind != BBREC_EXIT)
        n->cursor = event_next(r, n->cursor);
    if (!e || e->kind != BBREC_CALL) {
        native_fail(n, "call of %#" PRIx64 " that the record does not have", fn);
        return;
    }
    const BbRecRegs *call = event_data(e);
    if (e->address != fn) native_fail(n, "call of %#" PRIx64 ", recorded %#" PRIx64, fn, e->address);
    static const int args[6] = {RDI, RSI, RDX, RCX, R8, R9};
    n->pointer_count = 0;
    for (uint32_t i = 0; i < in->gpr_count && i < 6; ++i) {
        const uint64_t recorded = call->r[args[i]];
        if (recorded >= n->frame_lo && recorded < n->frame_hi) { /* a pointer into the frame */
            if (n->pointer_count < 16) n->pointers[n->pointer_count++] = (typeof(n->pointers[0])){recorded, in->gpr[i]};
            continue;
        }
        if (in->gpr[i] != recorded)
            native_fail(n, "call of %#" PRIx64 ": argument %u %#" PRIx64 ", recorded %#" PRIx64, fn, i + 1, in->gpr[i], recorded);
    }
    for (uint32_t i = 0; i < in->xmm_count && i < 8; ++i)
        if (memcmp(in->xmm[i], call->v[i], 16)) native_fail(n, "call of %#" PRIx64 ": xmm%u differs", fn, i);
    n->cursor = event_next(r, n->cursor);
    e = event_at(r, n->cursor);
    if (!e || e->kind != BBREC_RETURN) {
        native_fail(n, "record: no return after the call of %#" PRIx64, fn);
        return;
    }
    const BbRecRegs *ret = event_data(e);
    out->rax = ret->r[RAX];
    out->rdx = ret->r[RDX];
    memcpy(out->xmm0, ret->v[0], 16);
    memcpy(out->xmm1, ret->v[1], 16);
    n->cursor = event_next(r, n->cursor);
    native_epoch(n, n->cursor);
}

static int replay_native(const Record *r) {
    NativeReplay n = {.record = r, .frame_lo = r->entry.r[RSP] - (1 << 20), .frame_hi = r->entry.r[RSP]};
    active = &n;
    n.pages = tracked_count + (size_t)((image_data_hi - image_data_lo) / 16384);
    n.baseline = calloc(n.pages, sizeof(*n.baseline));
    for (size_t i = 0; i < n.pages; ++i) n.baseline[i] = malloc(16384);
    native_epoch(&n, 0);
    for (size_t i = 0; i < n.pages; ++i) memcpy(n.baseline[i], (const void *)page_address(&n, i), 16384);
    /* The arguments: as the game's bridge passes them (BbHostArgs). */
    BbHostArgs a;
    memset(&a, 0, sizeof(a));
    static const int args[6] = {RDI, RSI, RDX, RCX, R8, R9};
    for (int i = 0; i < 6; ++i) a.gpr[i] = r->entry.r[args[i]];
    const uint64_t stack_args = r->entry.r[RSP] + 8;
    if (baseline_of(&n, stack_args) && baseline_of(&n, stack_args + sizeof(a.stack) - 1))
        memcpy(a.stack, (const void *)stack_args, sizeof(a.stack));
    a.gpr[6] = a.stack[0];
    a.gpr[7] = a.stack[1];
    for (int i = 0; i < 8; ++i) memcpy(a.xmm[i].b, r->entry.v[i], 16);
    bbcpu_hostcall(native_function->function, &a);
    collect(&n);

    const BbRecEvent *e;
    while ((e = event_at(r, n.cursor)) && e->kind != BBREC_EXIT && e->kind != BBREC_CALL)
        n.cursor = event_next(r, n.cursor);
    if (!e || e->kind != BBREC_EXIT) {
        native_fail(&n, "returned before the record's calls");
    } else {
        const BbRecRegs *exit = event_data(e);
        switch (native_function->returns) {
        case BB_RETURNS_I32:
            if ((uint32_t)a.rax != (uint32_t)exit->r[RAX])
                native_fail(&n, "returned %#x, recorded %#x", (uint32_t)a.rax, (uint32_t)exit->r[RAX]);
            break;
        case BB_RETURNS_I64:
            if (a.rax != exit->r[RAX]) native_fail(&n, "returned %#" PRIx64 ", recorded %#" PRIx64, a.rax, exit->r[RAX]);
            break;
        case BB_RETURNS_F32:
            if (memcmp(a.xmm0.b, exit->v[0], 4)) native_fail(&n, "returned float differs");
            break;
        case BB_RETURNS_F64:
            if (memcmp(a.xmm0.b, exit->v[0], 8)) native_fail(&n, "returned double differs");
            break;
        default:
            break;
        }
    }
    /* The bytes the original wrote (outside its frame) against the native function's. */
    ByteMap expected = {0};
    expected_writes(r, &expected, n.frame_lo, n.frame_hi + 8);
    uint64_t wrong = 0, extra = 0;
    for (size_t i = 0; i < expected.capacity; ++i) {
        if (!expected.keys[i]) continue;
        const uint64_t address = expected.keys[i] - 1;
        uint8_t value;
        if (!map_get(&n.written, address, &value)) value = *(const uint8_t *)address; /* unchanged */
        if (value != expected.values[i] && wrong++ < 4)
            native_fail(&n, "%#" PRIx64 " = %#x, recorded %#x", address, value, expected.values[i]);
    }
    for (size_t i = 0; i < n.written.capacity; ++i) {
        uint8_t value;
        if (n.written.keys[i] && !map_get(&expected, n.written.keys[i] - 1, &value) && extra++ < 4)
            native_fail(&n, "written, not in the record: %#" PRIx64, n.written.keys[i] - 1);
    }
    if (wrong + extra)
        native_fail(&n, "%" PRIu64 " bytes wrong, %" PRIu64 " extra (of %zu)", wrong, extra, expected.count);
    if (verbose || !n.failures) printf("    native: %u calls, %zu bytes written\n", r->header.calls, expected.count);
    return n.failures ? 1 : 0;
}

/* ---- recompiled code (--recomp LIB) ---- */

static RcFn recomp_function;

/* --lockstep: the original's and the generated code's paths (offset and registers before each
 * instruction), compared; the first difference is reported. */
typedef struct { uint64_t off; uint64_t r[16]; uint8_t v[16][32]; } Step;
enum { MAX_STEPS = 200000 };
static int lockstep;
static Step *original_steps; /* shared with the child that runs the original */
static uint64_t *original_count;
static Step *generated_steps;
static uint64_t generated_count, lockstep_base;

static void original_before(BbTrace *t, uint64_t rip) {
    if (*original_count < MAX_STEPS) {
        Step *s = &original_steps[(*original_count)++];
        s->off = rip - lockstep_base;
        memcpy(s->r, t->cpu->r, sizeof(s->r));
        for (int i = 0; i < 16; ++i) memcpy(s->v[i], t->cpu->v[i].b, 32);
    }
}
static void generated_trace(uint64_t offset, const uint64_t r[16], const BbCpu *cpu) {
    if (generated_count < MAX_STEPS) {
        Step *s = &generated_steps[generated_count++];
        s->off = offset;
        memcpy(s->r, r, sizeof(s->r));
        for (int i = 0; i < 16; ++i) memcpy(s->v[i], cpu->v[i].b, 32);
    }
}

/* The first step where the two paths differ. */
static void compare_steps(void) {
    const uint64_t n = generated_count < *original_count ? generated_count : *original_count;
    for (uint64_t i = 0; i < n; ++i) {
        const Step *a = &original_steps[i], *b = &generated_steps[i];
        if (a->off != b->off) {
            printf("    lockstep: step %" PRIu64 ": the original is at %#" PRIx64 ", the generated code at %#" PRIx64
                   " (previous instruction %#" PRIx64 ")\n", i, a->off, b->off, i ? a[-1].off : 0);
            return;
        }
        for (int k = 0; k < 16; ++k)
            if (a->r[k] != b->r[k]) {
                printf("    lockstep: step %" PRIu64 " at %#" PRIx64 ": %s %#" PRIx64 ", generated %#" PRIx64
                       " (written by the instruction at %#" PRIx64 ")\n", i, a->off, reg_names[k], a->r[k], b->r[k],
                       i ? a[-1].off : 0);
                return;
            }
        for (int k = 0; k < 16; ++k)
            if (memcmp(a->v[k], b->v[k], 32)) {
                uint64_t x[4], y[4];
                memcpy(x, a->v[k], 32);
                memcpy(y, b->v[k], 32);
                printf("    lockstep: step %" PRIu64 " at %#" PRIx64 ": ymm%d %016" PRIx64 "%016" PRIx64 "%016" PRIx64 "%016" PRIx64
                       ", generated %016" PRIx64 "%016" PRIx64 "%016" PRIx64 "%016" PRIx64 " (written by the instruction at %#" PRIx64 ")\n",
                       i, a->off, k, x[3], x[2], x[1], x[0], y[3], y[2], y[1], y[0], i ? a[-1].off : 0);
                return;
            }
    }
    printf("    lockstep: the same %" PRIu64 " steps (original %" PRIu64 ", generated %" PRIu64 ")\n", n,
           *original_count, generated_count);
}

/* RcApi.call/tail: answered from the record, as replay_call does for the tracer. */
static void recomp_call_how(BbCpu *cpu, uint64_t target, uint64_t next, int tail) {
    NativeReplay *n = active;
    const Record *r = n->record;
    collect(n);
    const BbRecEvent *e;
    while ((e = event_at(r, n->cursor)) && e->kind != BBREC_CALL && e->kind != BBREC_EXIT)
        n->cursor = event_next(r, n->cursor);
    if (!e || e->kind != BBREC_CALL) {
        native_fail(n, "call of %#" PRIx64 " that the record does not have", target);
        cpu->rip = 0;
        return;
    }
    const BbRecRegs *call = event_data(e);
    if (e->address != target) native_fail(n, "call of %#" PRIx64 ", recorded %#" PRIx64, target, e->address);
    if (tail != e->tail) native_fail(n, "call of %#" PRIx64 ": tail call differs", target);
    static const int args[6] = {RDI, RSI, RDX, RCX, R8, R9};
    for (int i = 0; i < 6; ++i)
        if (cpu->r[args[i]] != call->r[args[i]])
            native_fail(n, "call of %#" PRIx64 ": %s %#" PRIx64 ", recorded %#" PRIx64, target,
                        reg_names[args[i]], cpu->r[args[i]], call->r[args[i]]);
    n->cursor = event_next(r, n->cursor);
    e = event_at(r, n->cursor);
    if (!e || e->kind != BBREC_RETURN) {
        native_fail(n, "record: no return after the call of %#" PRIx64, target);
        cpu->rip = 0;
        return;
    }
    bbcpu_trace_set_regs(cpu, event_data(e));
    if (!tail) cpu->rip = next;
    n->cursor = event_next(r, n->cursor);
    native_epoch(n, n->cursor);
}
static void recomp_call(BbCpu *cpu, uint64_t target, uint64_t next) { recomp_call_how(cpu, target, next, 0); }
static void recomp_tail(BbCpu *cpu) { recomp_call_how(cpu, cpu->rip, 0, 1); }
/* RcApi.bail: the rest of the function in the tracer, its calls answered from the record as the
 * generated code's are (what it writes is found by comparing memory, as for the generated code). */
static uint64_t function_end;
static void bail_access(BbTrace *t, int kind, uint64_t address, uint32_t length) {
    (void)t; (void)kind; (void)address; (void)length;
}
static void bail_call(BbTrace *t, const BbInsn *in, uint64_t target, int how) {
    (void)in;
    if (how == BB_TRACE_TRAP) {
        native_fail(active, "an int3 hook at %#" PRIx64 " (not replayable)", t->cpu->rip);
        t->cpu->rip = 0;
        return;
    }
    recomp_call_how(t->cpu, target, t->cpu->rip, how == BB_TRACE_TAIL);
}
static void bail_unsupported(BbTrace *t, const BbInsn *in) {
    (void)t; (void)in;
}
static void recomp_bail(BbCpu *cpu, uint64_t entry_rsp) {
    BbTrace t = {.cpu = cpu, .start = active->record->entry.rip, .end = function_end, .entry_rsp = entry_rsp,
                 .access = bail_access, .call = bail_call, .unsupported = bail_unsupported,
                 .limit = active->record->header.instructions * 4 + 10000};
    bbcpu_trace(&t);
    if (t.stopped) native_fail(active, "still running in the translator after %" PRIu64 " instructions", t.instructions);
}
static uint64_t recomp_step(BbCpu *cpu) {
    const BbBlock *block = bbcpu_block(cpu->rip);
    return bbcpu_step_insn(cpu, &block->insn[0]);
}

static int replay_recomp(const Record *r) {
    NativeReplay n = {.record = r, .frame_lo = 1, .frame_hi = 0, .same_frame = 1};
    /* The function's end for the tracer (bail): before the first tail-call target above it. */
    function_end = UINT64_MAX;
    for (uint64_t offset = 0; event_at(r, offset); offset = event_next(r, offset)) {
        const BbRecEvent *e = event_at(r, offset);
        if (e->kind == BBREC_CALL && e->tail && e->address > r->entry.rip && e->address < function_end)
            function_end = e->address;
    }
    active = &n;
    n.pages = tracked_count + (size_t)((image_data_hi - image_data_lo) / 16384);
    n.baseline = calloc(n.pages, sizeof(*n.baseline));
    for (size_t i = 0; i < n.pages; ++i) n.baseline[i] = malloc(16384);
    native_epoch(&n, 0);
    for (size_t i = 0; i < n.pages; ++i) memcpy(n.baseline[i], (const void *)page_address(&n, i), 16384);
    static BbCpu cpu;
    memset(&cpu, 0, sizeof(cpu));
    bbcpu_trace_set_regs(&cpu, &r->entry);
    cpu.fs_base = r->entry.fs_base;
    cpu.tcb = r->entry.tcb;
    cpu.gs_base = (uint64_t)(uintptr_t)&cpu.tcb - (r->header.tcb_address - r->entry.gs_base);
    if (watch_address()) printf("    watch: entry rsp %#" PRIx64 "\n", r->entry.r[RSP]);
    recomp_function(&cpu);
    collect(&n);

    const BbRecEvent *e;
    while ((e = event_at(r, n.cursor)) && e->kind != BBREC_EXIT && e->kind != BBREC_CALL)
        n.cursor = event_next(r, n.cursor);
    if (!e || e->kind != BBREC_EXIT) {
        native_fail(&n, "returned before the record's calls");
    } else {
        const BbRecRegs *exit = event_data(e);
        for (int i = 0; i < 16; ++i)
            if (cpu.r[i] != exit->r[i])
                native_fail(&n, "at the return: %s %#" PRIx64 ", recorded %#" PRIx64, reg_names[i], cpu.r[i], exit->r[i]);
        if (cpu.rip != exit->rip) native_fail(&n, "returned to %#" PRIx64 ", recorded %#" PRIx64, cpu.rip, exit->rip);
        if (memcmp(cpu.v[0].b, exit->v[0], 16) || memcmp(cpu.v[1].b, exit->v[1], 16))
            native_fail(&n, "at the return: xmm0 or xmm1 differs");
    }
    ByteMap expected = {0};
    expected_writes(r, &expected, 1, 0);
    uint64_t wrong = 0, extra = 0;
    for (size_t i = 0; i < expected.capacity; ++i) {
        if (!expected.keys[i]) continue;
        const uint64_t address = expected.keys[i] - 1;
        uint8_t value;
        if (!map_get(&n.written, address, &value)) value = *(const uint8_t *)address;
        if (value != expected.values[i] && wrong++ < 4)
            native_fail(&n, "%#" PRIx64 " = %#x, recorded %#x", address, value, expected.values[i]);
    }
    for (size_t i = 0; i < n.written.capacity; ++i) {
        uint8_t value;
        if (n.written.keys[i] && !map_get(&expected, n.written.keys[i] - 1, &value) && extra++ < 4)
            native_fail(&n, "written, not in the record: %#" PRIx64, n.written.keys[i] - 1);
    }
    if (wrong + extra)
        native_fail(&n, "%" PRIu64 " bytes wrong, %" PRIu64 " extra (of %zu)", wrong, extra, expected.count);
    if (verbose || !n.failures) printf("    recompiled: %u calls, %zu bytes written\n", r->header.calls, expected.count);
    return n.failures ? 1 : 0;
}

static void on_timeout(int signal) {
    (void)signal;
    static const char line[] = "    still running after 10 s (waits for another thread?)\n";
    write(1, line, sizeof(line) - 1);
    _exit(1);
}

static int replay(const Record *r) {
    /* --lockstep: the original first, in a child of its own (its path into shared memory). */
    if (recomp_function && lockstep && !original_steps) {
        original_steps = mmap(NULL, MAX_STEPS * sizeof(Step) + 8, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANON, -1, 0);
        original_count = (uint64_t *)(original_steps + MAX_STEPS);
        generated_steps = malloc(MAX_STEPS * sizeof(Step));
        lockstep_base = r->header.image_base;
        const pid_t child = fork();
        if (child == 0) {
            const RcFn saved = recomp_function;
            recomp_function = NULL;
            (void)saved;
            freopen("/dev/null", "w", stdout);
            _exit(replay(r));
        }
        int status;
        waitpid(child, &status, 0);
    }
    struct sigaction action = {.sa_sigaction = on_crash, .sa_flags = SA_SIGINFO};
    sigaction(SIGSEGV, &action, NULL);
    sigaction(SIGBUS, &action, NULL);
    signal(SIGALRM, on_timeout);
    alarm(10);
    const uint64_t in_use = map_pages(r);
    if (in_use) {
        printf("    not replayable here: %#" PRIx64 " is this process's memory\n", in_use);
        return 2;
    }
    /* The image here is the file's: its TCB loads read fs:[0] (the game's loader made them gs
     * loads of the TCB pointer, recorded elsewhere): the TCB's self pointer, as on the PS4. */
    if (r->entry.fs_base) {
        const uint64_t page = r->entry.fs_base & ~UINT64_C(16383);
        void *got = mmap((void *)page, 16384, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
        if (got != (void *)page && got != MAP_FAILED) munmap(got, 16384); /* mapped already (recorded) */
        memcpy((void *)r->entry.fs_base, &r->entry.tcb, 8);
    }
    if (native_function) return replay_native(r);
    if (recomp_function && lockstep) {
        const int result = replay_recomp(r);
        compare_steps();
        return result;
    }
    if (recomp_function) return replay_recomp(r);
    Replay p = {.record = r};
    static BbCpu cpu;
    memset(&cpu, 0, sizeof(cpu));
    bbcpu_trace_set_regs(&cpu, &r->entry);
    cpu.fs_base = r->entry.fs_base;
    cpu.tcb = r->entry.tcb;
    cpu.gs_base = (uint64_t)(uintptr_t)&cpu.tcb - (r->header.tcb_address - r->entry.gs_base);
    p.trace = (BbTrace){.cpu = &cpu,
                        .start = r->entry.rip,
                        .end = r->entry.rip + 1,
                        .access = replay_access,
                        .call = replay_call,
                        .unsupported = replay_unsupported};
    apply_epoch(&p, 0);
    /* The function's end: tail calls are jumps out of it. Its size is not in the record, so
     * any jump the record has as a call is one. */
    p.trace.end = UINT64_MAX;
    for (uint64_t offset = 0; event_at(r, offset); offset = event_next(r, offset)) {
        const BbRecEvent *e = event_at(r, offset);
        if (e->kind == BBREC_CALL && e->tail && e->address > r->entry.rip && e->address < p.trace.end)
            p.trace.end = e->address;
    }
    if (lockstep) p.trace.before = original_before;
    p.trace.limit = r->header.instructions * 4 + 10000;
    bbcpu_trace(&p.trace);
    if (p.trace.stopped) fail(&p, "still running after %" PRIu64 " instructions (recorded %" PRIu64 ")",
                              p.trace.instructions, r->header.instructions);

    const BbRecEvent *e;
    while ((e = event_at(r, p.cursor)) && e->kind != BBREC_EXIT && e->kind != BBREC_CALL)
        p.cursor = event_next(r, p.cursor);
    if (!e || e->kind != BBREC_EXIT) {
        fail(&p, "returned before the record's calls (%s)", e ? "a call left" : "no exit");
    } else {
        const BbRecRegs *exit = event_data(e);
        for (int i = 0; i < 16; ++i)
            if (cpu.r[i] != exit->r[i])
                fail(&p, "at the return: %s %#" PRIx64 ", recorded %#" PRIx64, reg_names[i], cpu.r[i], exit->r[i]);
        if (cpu.rip != exit->rip) fail(&p, "returned to %#" PRIx64 ", recorded %#" PRIx64, cpu.rip, exit->rip);
        if (memcmp(cpu.v[0].b, exit->v[0], 16) || memcmp(cpu.v[1].b, exit->v[1], 16))
            fail(&p, "at the return: xmm0 or xmm1 differs");
    }
    /* The bytes the function wrote: last values. */
    ByteMap expected = {0};
    for (uint64_t offset = 0; event_at(r, offset); offset = event_next(r, offset)) {
        e = event_at(r, offset);
        if (e->kind != BBREC_WRITE) continue;
        for (uint32_t b = 0; b < e->length; ++b) map_put(&expected, e->address + b, ((const uint8_t *)event_data(e))[b]);
    }
    uint64_t missing = 0, wrong = 0, extra = 0;
    for (size_t i = 0; i < expected.capacity; ++i) {
        if (!expected.keys[i]) continue;
        uint8_t value;
        if (!map_get(&p.written, expected.keys[i] - 1, &value)) {
            /* Written with the value it had here: memory the replay does not know before (only
             * what the function read is in the record) can already hold it. */
            if (map_get(&p.kept, expected.keys[i] - 1, &value) &&
                *(const uint8_t *)(uintptr_t)(expected.keys[i] - 1) == expected.values[i]) continue;
            if (missing++ < 4) fail(&p, "not written: %#" PRIx64, expected.keys[i] - 1);
        } else if (value != expected.values[i]) {
            if (wrong++ < 4) fail(&p, "written %#" PRIx64 " = %#x, recorded %#x", expected.keys[i] - 1, value, expected.values[i]);
        }
    }
    for (size_t i = 0; i < p.written.capacity; ++i) {
        uint8_t value;
        if (p.written.keys[i] && !map_get(&expected, p.written.keys[i] - 1, &value))
            if (extra++ < 4) fail(&p, "written, not in the record: %#" PRIx64, p.written.keys[i] - 1);
    }
    if (missing + wrong + extra)
        fail(&p, "%" PRIu64 " bytes not written, %" PRIu64 " wrong, %" PRIu64 " extra (of %zu)", missing, wrong, extra, expected.count);
    if (verbose || !p.failures)
        printf("    %" PRIu64 " instructions, %u calls, %zu bytes written\n", p.trace.instructions, r->header.calls, expected.count);
    return p.failures ? 1 : 0;
}

/* ---- the image ---- */

typedef struct { unsigned char e_ident[16]; uint16_t e_type, e_machine; uint32_t e_version;
    uint64_t e_entry, e_phoff, e_shoff; uint32_t e_flags; uint16_t e_ehsize, e_phentsize, e_phnum,
    e_shentsize, e_shnum, e_shstrndx; } Elf64_Ehdr;
typedef struct { uint32_t p_type, p_flags; uint64_t p_offset, p_vaddr, p_paddr, p_filesz, p_memsz,
    p_align; } Elf64_Phdr;

static void map_image(const char *path, uint64_t base) {
    FILE *f = fopen(path, "rb");
    if (!f) { perror(path); exit(1); }
    fseek(f, 0, SEEK_END);
    const long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *data = malloc((size_t)size);
    if (!data || fread(data, 1, (size_t)size, f) != (size_t)size) exit(1);
    fclose(f);
    const Elf64_Ehdr *eh = (const Elf64_Ehdr *)data;
    for (int i = 0; i < eh->e_phnum; ++i) {
        const Elf64_Phdr *ph = (const Elf64_Phdr *)(data + eh->e_phoff + (uint64_t)i * eh->e_phentsize);
        if (ph->p_type != 1) continue;
        const uint64_t start = (base + ph->p_vaddr) & ~UINT64_C(16383);
        const uint64_t end = (base + ph->p_vaddr + ph->p_memsz + 16383) & ~UINT64_C(16383);
        void *got = mmap((void *)start, end - start, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
        if (got != (void *)start) {
            fprintf(stderr, "cannot map the image at %#" PRIx64 "\n", start);
            exit(1);
        }
        memcpy((void *)(base + ph->p_vaddr), data + ph->p_offset, ph->p_filesz);
        if (ph->p_flags & 1) bbcpu_add_guest_code(base + ph->p_vaddr, ph->p_memsz);
        if (!image_lo || start < image_lo) image_lo = start;
        if (end > image_hi) image_hi = end;
        if (ph->p_flags & 2) {
            image_data_lo = start;
            image_data_hi = end;
        }
    }
    free(data);
}

int main(int argc, char **argv) {
    const char *records = NULL, *elf = "out/eboot.elf", *native_library = NULL, *recomp_library = NULL;
    long only = -1;
    for (int i = 1; i < argc; ++i) {
        if (!strcmp(argv[i], "--elf") && i + 1 < argc) elf = argv[++i];
        else if (!strcmp(argv[i], "--only") && i + 1 < argc) only = atol(argv[++i]);
        else if (!strcmp(argv[i], "--verbose")) verbose = 1;
        else if (!strcmp(argv[i], "--nofork")) no_fork = 1; /* one record (--only), in a debugger */
        else if (!strcmp(argv[i], "--native") && i + 1 < argc) native_library = argv[++i];
        else if (!strcmp(argv[i], "--recomp") && i + 1 < argc) recomp_library = argv[++i];
        else if (!strcmp(argv[i], "--lockstep")) lockstep = 1; /* with --recomp and a BB_RECOMP_TRACE=1 library */
        else records = argv[i];
    }
    if (!records) {
        fprintf(stderr, "usage: %s RECORDS [--elf ELF] [--only N] [--verbose] [--native LIB] [--recomp LIB]\n", argv[0]);
        return 2;
    }
    /* Read whole: the children must not share a file position with this process. */
    FILE *f = fopen(records, "rb");
    if (!f) { perror(records); return 1; }
    fseek(f, 0, SEEK_END);
    const long file_size = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *file = malloc((size_t)file_size + 1);
    if (!file || fread(file, 1, (size_t)file_size, f) != (size_t)file_size) { perror(records); return 1; }
    fclose(f);
    /* The interpreter's tables are made on its first run. */
    static BbCpu first;
    bbcpu_run(&first, 0);
    int mapped = 0, ok = 0, mismatched = 0, skipped = 0, racy = 0, number = 0;
    for (long at = 0; at < file_size; ++number) {
        Record r;
        if (at + (long)(sizeof(r.header) + sizeof(r.entry)) > file_size) break;
        memcpy(&r.header, file + at, sizeof(r.header));
        memcpy(&r.entry, file + at + sizeof(r.header), sizeof(r.entry));
        if (r.header.magic != BBREC_MAGIC || r.header.version != BBREC_VERSION) {
            fprintf(stderr, "%s: not a record at byte %ld\n", records, at);
            return 1;
        }
        r.events = file + at + sizeof(r.header) + sizeof(r.entry);
        at += (long)(sizeof(r.header) + sizeof(r.entry) + r.header.bytes);
        if (at > file_size) break;
        if (only >= 0 && number != only) continue;
        if (!mapped) {
            map_image(elf, r.header.image_base);
            mapped = 1;
            if (recomp_library) {
                /* The recompiled version of the recorded function, its calls answered here. */
                static RcApi api;
                api = (RcApi){.version = BB_RECOMP_API_VERSION, .image_base = r.header.image_base,
                              .step = recomp_step, .call = recomp_call, .tail = recomp_tail, .bail = recomp_bail,
                              .trace = generated_trace};
                void *library = dlopen(recomp_library, RTLD_NOW | RTLD_LOCAL);
                const RcInit init = library ? (RcInit)dlsym(library, BB_RECOMP_INIT) : NULL;
                if (!init) {
                    fprintf(stderr, "%s: %s\n", recomp_library, dlerror());
                    return 1;
                }
                size_t count = 0;
                const RcFunction *functions = init(&api, &count);
                for (size_t i = 0; i < count; ++i)
                    if (functions[i].offset == r.header.offset) recomp_function = functions[i].fn;
                if (!recomp_function) {
                    fprintf(stderr, "%s: %#" PRIx64 " was not recompiled\n", recomp_library, r.header.offset);
                    return 1;
                }
            }
            if (native_library) {
                /* The native version of the recorded function, with calls answered here. */
                static BbNativeApi api;
                api = (BbNativeApi){.version = BB_NATIVE_API_VERSION, .image_base = r.header.image_base,
                                    .call = native_call};
                void *library = dlopen(native_library, RTLD_NOW | RTLD_LOCAL);
                BbNativeInit init = library ? (BbNativeInit)dlsym(library, BB_NATIVE_INIT) : NULL;
                if (!init) {
                    fprintf(stderr, "%s: %s\n", native_library, dlerror());
                    return 1;
                }
                size_t count = 0;
                const BbNativeFunction *functions = init(&api, &count);
                for (size_t i = 0; i < count; ++i)
                    if (functions[i].offset == r.header.offset) native_function = &functions[i];
                if (!native_function) {
                    fprintf(stderr, "%s: no native version of %#" PRIx64 "\n", native_library, r.header.offset);
                    return 1;
                }
            }
        }
        printf("record %d (call %" PRIu64 " of %#" PRIx64 "):", number, r.header.index, r.header.offset);
        if (r.header.failed) {
            printf(" not usable (%s)\n", r.header.failed == BBREC_TOO_BIG ? "too big" : "unsupported instruction");
            ++skipped;
            continue;
        }
        if (r.header.racy) {
            printf(" %u bytes changed by other threads during the call\n", r.header.racy);
            ++racy;
        } else {
            printf("\n");
        }
        fflush(stdout);
        if (no_fork) return replay(&r);
        const pid_t child = fork();
        if (child == 0) {
            const int result = replay(&r);
            fflush(stdout);
            _exit(result);
        }
        int status = 0;
        waitpid(child, &status, 0);
        if (WIFEXITED(status) && WEXITSTATUS(status) == 0) {
            ++ok;
        } else if (WIFEXITED(status) && WEXITSTATUS(status) == 2) {
            ++skipped;
        } else {
            if (WIFSIGNALED(status)) printf("    crashed (signal %d)\n", WTERMSIG(status));
            printf("    MISMATCH\n");
            ++mismatched;
        }
    }
    printf("%d records: %d match, %d mismatch, %d not usable; %d had other threads' writes\n", number,
           ok, mismatched, skipped, racy);
    return mismatched ? 1 : 0;
}
