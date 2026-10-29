/* tools/recomp/bbrecomp.c ELF FUNCTIONS_TSV OUT.c OFFSET...: the recompiler (docs/RECOMPILATION.md). Writes
 * one C function per game function (image offsets; sizes from tools/recomp/scan.c's functions.tsv)
 * against src/recomp/rc.h, in files OUT_<n>.c, and the library's table and entry point in OUT.c.
 * Build: tools/recomp/recomp.sh.
 *
 * Generated code keeps the guest registers in local arrays (of the vector ones, those it uses)
 * and the flags lazily; every instruction gets a label. Instructions the generator does not
 * translate run in the interpreter (rc->step) with the state spilled around them; a branch to an
 * unknown place goes back to the translator (rc->bail) or, out of the function, is a tail call
 * (rc->tail). */
#include "../../src/cpu/trace.h"
#include <inttypes.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <unistd.h>

#define M(name) ZYDIS_MNEMONIC_##name

void *runtime_low_map(size_t size, int prot) {
    void *p = mmap(NULL, size, prot, MAP_PRIVATE | MAP_ANON, -1, 0);
    return p == MAP_FAILED ? NULL : p;
}
uint64_t bb_image_base;

/* ---- the image ---- */

typedef struct { unsigned char e_ident[16]; uint16_t e_type, e_machine; uint32_t e_version;
    uint64_t e_entry, e_phoff, e_shoff; uint32_t e_flags; uint16_t e_ehsize, e_phentsize, e_phnum,
    e_shentsize, e_shnum, e_shstrndx; } Elf64_Ehdr;
typedef struct { uint32_t p_type, p_flags; uint64_t p_offset, p_vaddr, p_paddr, p_filesz, p_memsz,
    p_align; } Elf64_Phdr;

static uint64_t base; /* where the image is mapped here */

static void map_image(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) { perror(path); exit(1); }
    fseek(f, 0, SEEK_END);
    const long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *data = malloc((size_t)size);
    if (!data || fread(data, 1, (size_t)size, f) != (size_t)size) exit(1);
    fclose(f);
    const Elf64_Ehdr *eh = (const Elf64_Ehdr *)data;
    uint64_t end = 0;
    for (int i = 0; i < eh->e_phnum; ++i) {
        const Elf64_Phdr *ph = (const Elf64_Phdr *)(data + eh->e_phoff + (uint64_t)i * eh->e_phentsize);
        if (ph->p_type == 1 && ph->p_vaddr + ph->p_memsz > end) end = ph->p_vaddr + ph->p_memsz;
    }
    uint8_t *image = mmap(NULL, (end + 16383) & ~UINT64_C(16383), PROT_READ | PROT_WRITE,
                          MAP_PRIVATE | MAP_ANON, -1, 0);
    if (image == MAP_FAILED) exit(1);
    base = (uint64_t)(uintptr_t)image;
    for (int i = 0; i < eh->e_phnum; ++i) {
        const Elf64_Phdr *ph = (const Elf64_Phdr *)(data + eh->e_phoff + (uint64_t)i * eh->e_phentsize);
        if (ph->p_type != 1) continue;
        memcpy(image + ph->p_vaddr, data + ph->p_offset, ph->p_filesz);
        if (ph->p_flags & 1) bbcpu_add_guest_code(base + ph->p_vaddr, ph->p_memsz);
    }
    free(data);
}

/* ---- functions ---- */

typedef struct { uint64_t offset, size; } Function;
static Function *functions;
static size_t function_count;

static uint64_t size_of(const char *tsv, uint64_t offset) {
    static uint64_t *starts, *sizes;
    static size_t count;
    if (!starts) {
        FILE *f = fopen(tsv, "r");
        if (!f) { perror(tsv); exit(1); }
        char line[256];
        size_t capacity = 0;
        while (fgets(line, sizeof(line), f)) {
            if (line[0] != '0') continue;
            if (count == capacity) {
                capacity = capacity ? capacity * 2 : 1 << 18;
                starts = realloc(starts, capacity * 8);
                sizes = realloc(sizes, capacity * 8);
            }
            char *end;
            starts[count] = strtoull(line, &end, 16);
            sizes[count++] = strtoull(end, NULL, 10);
        }
        fclose(f);
    }
    for (size_t lo = 0, hi = count; lo < hi;) { /* functions.tsv is sorted */
        const size_t mid = (lo + hi) / 2;
        if (starts[mid] == offset) return sizes[mid];
        if (starts[mid] < offset) lo = mid + 1; else hi = mid;
    }
    return 0;
}

/* ---- output ---- */

static FILE *out;
static void emit(const char *format, ...) __attribute__((format(printf, 1, 2)));
static void emit(const char *format, ...) {
    va_list args;
    va_start(args, format);
    vfprintf(out, format, args);
    va_end(args);
}

/* Closes `out` where writing ended: a translation that gave up may have written past it. */
static void close_output(void) {
    fflush(out);
    ftruncate(fileno(out), ftell(out));
    fclose(out);
}

/* The guest registers to and from the BbCpu around what reads or writes them there (calls, the
 * interpreter, returns): markers that generate replaces with the copies each place needs (see
 * sync_scan). SPILL before what reads them, RELOAD after what may change them, RELOAD_IF there on
 * a path that may not be taken (a slow path inside a translation). */
#define SPILL "@S"
#define RELOAD "@R"
#define RELOAD_IF "@r"
#define COND "rc_cond(%d, fk, fa, fb, fr, fz, fcf, c->flags)"
#define FLAGS_OUT "c->flags = rc_flags(fk, fa, fb, fr, fz, fcf, c->flags); fk = FK_CPU;"
#define FLAGS_NOW "rc_flags(fk, fa, fb, fr, fz, fcf, c->flags)"

typedef struct { char s[1024]; } Str;
static Str str(const char *format, ...) __attribute__((format(printf, 1, 2)));
static Str str(const char *format, ...) {
    Str v;
    va_list args;
    va_start(args, format);
    vsnprintf(v.s, sizeof(v.s), format, args);
    va_end(args);
    return v;
}
static uint64_t mask(int size) { return size >= 8 ? ~UINT64_C(0) : (UINT64_C(1) << (size * 8)) - 1; }

/* The address of a memory operand (image offsets for rip-relative); 0: not translatable. */
static int address(uint64_t off, const BbInsn *in, const BbOp *op, Str *s, int *stack) {
    if (op->segment) return 0;
    char text[200] = "";
    size_t n = 0;
    if (op->base == 0xfe) {
        n += (size_t)snprintf(text + n, sizeof(text) - n, "(B + 0x%" PRIx64 "ull)", off + in->length + (uint64_t)op->disp);
    } else {
        n += (size_t)snprintf(text + n, sizeof(text) - n, "(0x%" PRIx64 "ull", (uint64_t)op->disp);
        if (op->base != 0xff) n += (size_t)snprintf(text + n, sizeof(text) - n, " + r[%d]", op->base);
        if (op->index != 0xff) n += (size_t)snprintf(text + n, sizeof(text) - n, " + r[%d] * %d", op->index, op->scale);
        n += (size_t)snprintf(text + n, sizeof(text) - n, ")");
    }
    *stack = op->base == RSP && op->index == 0xff;
    *s = str("%s", text);
    return 1;
}

static int gpr(const BbOp *op) { return op->type == OP_REG && op->kind == RK_GPR; }

/* An operand's value, zero-extended (immediates: sign-extended to `size`, then masked). */
static int read_op(uint64_t off, const BbInsn *in, const BbOp *op, int size, Str *v) {
    if (gpr(op)) {
        if (op->size == 1 && op->high8) *v = str("((r[%d] >> 8) & 0xff)", op->reg);
        else if (op->size == 8) *v = str("r[%d]", op->reg);
        else *v = str("(r[%d] & 0x%" PRIx64 "ull)", op->reg, mask(op->size));
        return 1;
    }
    if (op->type == OP_IMM) {
        *v = str("0x%" PRIx64 "ull", (uint64_t)op->disp & mask(size));
        return 1;
    }
    if (op->type == OP_MEM) {
        Str a;
        int stack;
        if (!address(off, in, op, &a, &stack)) return 0;
        *v = str("%s(%s, %d)", stack ? "rc_lds" : "rc_ld", a.s, op->size);
        return 1;
    }
    return 0;
}

/* A statement writing `v` to a register or memory operand. */
static int write_op(uint64_t off, const BbInsn *in, const BbOp *op, const char *v, Str *s) {
    if (gpr(op)) {
        const int x = op->reg;
        switch (op->size) {
        case 8: *s = str("r[%d] = (%s);", x, v); break;
        case 4: *s = str("r[%d] = (uint32_t)(%s);", x, v); break;
        case 2: *s = str("r[%d] = (r[%d] & ~0xffffull) | ((%s) & 0xffff);", x, x, v); break;
        default:
            if (op->high8) *s = str("r[%d] = (r[%d] & ~0xff00ull) | (((%s) & 0xff) << 8);", x, x, v);
            else *s = str("r[%d] = (r[%d] & ~0xffull) | ((%s) & 0xff);", x, x, v);
        }
        return 1;
    }
    if (op->type == OP_MEM) {
        Str a;
        int stack;
        if (!address(off, in, op, &a, &stack)) return 0;
        *s = str("%s(%s, %d, (%s));", stack ? "rc_sts" : "rc_st", a.s, op->size, v);
        return 1;
    }
    return 0;
}

static int cc_of(int m) {
    static const int jcc[16] = {M(JO), M(JNO), M(JB), M(JNB), M(JZ), M(JNZ), M(JBE), M(JNBE),
                                M(JS), M(JNS), M(JP), M(JNP), M(JL), M(JNL), M(JLE), M(JNLE)};
    static const int setcc[16] = {M(SETO), M(SETNO), M(SETB), M(SETNB), M(SETZ), M(SETNZ), M(SETBE),
                                  M(SETNBE), M(SETS), M(SETNS), M(SETP), M(SETNP), M(SETL), M(SETNL),
                                  M(SETLE), M(SETNLE)};
    static const int cmovcc[16] = {M(CMOVO), M(CMOVNO), M(CMOVB), M(CMOVNB), M(CMOVZ), M(CMOVNZ),
                                   M(CMOVBE), M(CMOVNBE), M(CMOVS), M(CMOVNS), M(CMOVP), M(CMOVNP),
                                   M(CMOVL), M(CMOVNL), M(CMOVLE), M(CMOVNLE)};
    for (int i = 0; i < 16; ++i) {
        if (jcc[i] == m) return i;
        if (setcc[i] == m) return 16 + i;
        if (cmovcc[i] == m) return 32 + i;
    }
    return -1;
}

typedef struct { uint64_t start, end; } Range;

/* The functions being recompiled, by offset: calls of them go through their direct-call slot
 * (d_<offset>, rc.h) when the program filled it. */
static uint64_t *recompiled;
/* Import stubs by name (imports.tsv): calls of them are labelled. */
typedef struct { uint64_t stub; char name[64]; } Import;
static Import *imports;
static size_t import_count;
static const char *import_name(uint64_t t) {
    for (size_t lo = 0, hi = import_count; lo < hi;) {
        const size_t mid = (lo + hi) / 2;
        if (imports[mid].stub == t) return imports[mid].name;
        if (imports[mid].stub < t) lo = mid + 1; else hi = mid;
    }
    return NULL;
}
/* The project's names for the game's functions (annotations/functions.tsv, given by
 * BB_RECOMP_ANNOTATIONS): written at each function and at calls of it ("?" when a guess). */
typedef struct { uint64_t offset; char name[256]; } Annotation;
static Annotation *annotations;
static size_t annotation_count;
static const char *annotation(uint64_t offset) {
    for (size_t lo = 0, hi = annotation_count; lo < hi;) {
        const size_t mid = (lo + hi) / 2;
        if (annotations[mid].offset == offset) return annotations[mid].name;
        if (annotations[mid].offset < offset) lo = mid + 1; else hi = mid;
    }
    return NULL;
}
static void load_annotations(const char *path) {
    FILE *f = fopen(path, "r");
    if (!f) { perror(path); exit(1); }
    char line[1024];
    for (int number = 1; fgets(line, sizeof(line), f); ++number) {
        if (line[0] == '#' || line[0] == '\n') continue;
        unsigned long long offset;
        char name[250], confidence[16];
        if (sscanf(line, "%llx\t%249[^\t\n]\t%15[^\t\n]", &offset, name, confidence) != 3 || strstr(name, "*/") ||
            (strcmp(confidence, "sure") && strcmp(confidence, "guess")) ||
            (annotation_count && offset <= annotations[annotation_count - 1].offset)) {
            fprintf(stderr, "%s:%d: not offset, name, sure or guess, in offset order\n", path, number);
            exit(1);
        }
        annotations = realloc(annotations, (annotation_count + 1) * sizeof(*annotations));
        annotations[annotation_count].offset = offset;
        snprintf(annotations[annotation_count++].name, sizeof(annotations->name), "%s%s", name,
                 strcmp(confidence, "guess") ? "" : "?");
    }
    fclose(f);
}
static int by_offset(const void *a, const void *b);
static int is_recompiled(uint64_t t) {
    return recompiled && bsearch(&t, recompiled, function_count, sizeof(*recompiled), by_offset) != NULL;
}
/* A call of an import that never returns: nothing after it is this call's fall-through (a
 * function can end with one, data such as a jump table right after it). */
static int no_return(uint64_t off, const BbInsn *in) {
    if (in->mnemonic != M(CALL) || in->op[0].type != OP_IMM) return 0;
    const char *name = import_name(off + (uint64_t)in->op[0].disp);
    return name && (!strcmp(name, "__stack_chk_fail") || !strcmp(name, "abort") || !strcmp(name, "exit") ||
                    !strcmp(name, "_exit"));
}
/* Whether execution can go on to the instruction after this one. */
static int falls_through(uint64_t off, const BbInsn *in) {
    const int m = in->mnemonic;
    return !(m == M(JMP) || m == M(RET) || m == M(INVALID) || m == M(UD2) || m == M(HLT) || no_return(off, in));
}

/* A jump to image offset `t`: a goto inside the function. Out of it: a tail call when the stack is
 * back at the entry (the return address on top), else code of this function elsewhere (a part the
 * compiler moved out, a shared block): the translator runs the rest, the function's return address
 * at entry_rsp (rc->bail). */
static void jump(Range f, uint64_t t) {
    if (t >= f.start && t < f.end) emit("goto L_%" PRIx64 ";", t);
    else if (is_recompiled(t))
        emit("{ extern RcFn d_%" PRIx64 "; " SPILL " " FLAGS_OUT " c->rip = B + 0x%" PRIx64 "ull; if (r[RSP] == entry_rsp) { if (d_%" PRIx64 ") d_%" PRIx64 "(c); else rc->tail(c); } else rc->bail(c, entry_rsp); return; }", t, t, t, t);
    else emit("{ " SPILL " " FLAGS_OUT " c->rip = B + 0x%" PRIx64 "ull; if (r[RSP] == entry_rsp) rc->tail(c); else rc->bail(c, entry_rsp); return; }", t);
}

/* The instruction in the interpreter: state spilled, flags in cpu->flags; an unexpected next rip
 * dispatches. Also the slow path of translations (division faults). */
static void emit_interp(uint64_t off, const BbInsn *in, int slow_path) {
    emit("/* %s */ { " SPILL " " FLAGS_OUT " c->rip = B + 0x%" PRIx64 "ull; const uint64_t n = rc->step(c); %s"
         " if (n != B + 0x%" PRIx64 "ull) { dn = n; goto L_dispatch; } }",
         ZydisMnemonicGetString((ZydisMnemonic)in->mnemonic), off, slow_path ? RELOAD_IF : RELOAD, off + in->length);
}

static int translate_vector(uint64_t off, const BbInsn *in);
static int translate_bits(uint64_t off, const BbInsn *in);
static int translate_shift_mul(uint64_t off, const BbInsn *in);
static int translate_extra(uint64_t off, const BbInsn *in);
static int translate_atomic(uint64_t off, const BbInsn *in);

/* One instruction natively; 0: it goes to the interpreter. */
static int translate(Range f, uint64_t off, const BbInsn *in, uint64_t *stats) {
    const uint64_t next = off + in->length;
    const int m = in->mnemonic;
    const BbOp *d = &in->op[0], *s = &in->op[1];
    Str a, b, w;
    if (in->rep || in->repne) return 0;
    if (in->lock || m == M(XADD) || m == M(CMPXCHG)) return translate_atomic(off, in);
    const int cc = cc_of(m);
    if (cc >= 0 && cc < 16) { /* jcc */
        if (d->type != OP_IMM) return 0;
        emit("if (" COND ") ", cc);
        jump(f, off + (uint64_t)d->disp);
        emit("\n");
        return 1;
    }
    if (cc >= 16 && cc < 32) { /* setcc */
        if (!write_op(off, in, d, str(COND, cc - 16).s, &w)) return 0;
        emit("%s\n", w.s);
        return 1;
    }
    if (cc >= 32) { /* cmovcc */
        if (!gpr(d) || !read_op(off, in, s, d->size, &b) || !write_op(off, in, d, b.s, &w)) return 0;
        emit("if (" COND ") { %s }", cc - 32, w.s);
        if (d->size == 4) emit(" else { r[%d] = (uint32_t)r[%d]; }", d->reg, d->reg);
        emit("\n");
        return 1;
    }
    if (bbcpu_is_tcb_load(in)) { /* mov reg, fs:[0] (the loader makes it a gs load): the TCB */
        emit("r[%d] = c->tcb;\n", d->reg);
        return 1;
    }
    switch (m) {
    case M(NOP): case M(ENDBR64): case M(PAUSE): case M(PREFETCHNTA): case M(PREFETCHT0):
    case M(PREFETCHT1): case M(PREFETCHT2): case M(PREFETCHW):
        emit("/* %s */\n", ZydisMnemonicGetString((ZydisMnemonic)m));
        return 1;
    case M(MOV):
        if (d->type == OP_REG && d->kind != RK_GPR) return 0;
        if (s->type == OP_REG && s->kind != RK_GPR) return 0;
        if (!read_op(off, in, s, d->size, &b) || !write_op(off, in, d, b.s, &w)) return 0;
        emit("%s\n", w.s);
        return 1;
    case M(MOVZX):
        if (!gpr(d) || !read_op(off, in, s, s->size, &b) || !write_op(off, in, d, b.s, &w)) return 0;
        emit("%s\n", w.s);
        return 1;
    case M(MOVSX): case M(MOVSXD): {
        if (!gpr(d) || !read_op(off, in, s, s->size, &b)) return 0;
        static const char *const ext[9] = {0, "int8_t", "int16_t", 0, "int32_t", 0, 0, 0, "int64_t"};
        if (!ext[s->size]) return 0;
        const Str v = str("(uint64_t)(int64_t)(%s)(%s)", ext[s->size], b.s);
        if (!write_op(off, in, d, v.s, &w)) return 0;
        emit("%s\n", w.s);
        return 1;
    }
    case M(LEA): {
        int stack;
        if (!gpr(d) || s->type != OP_AGEN || !address(off, in, s, &a, &stack)) return 0;
        if (!write_op(off, in, d, str("%s & 0x%" PRIx64 "ull", a.s, mask(d->size)).s, &w)) return 0;
        emit("%s\n", w.s);
        return 1;
    }
    case M(ADD): case M(SUB): case M(CMP): case M(AND): case M(OR): case M(XOR): case M(TEST): {
        const int size = d->size;
        if (!(gpr(d) || d->type == OP_MEM)) return 0;
        if (!read_op(off, in, s, size, &b)) return 0;
        const int kind = m == M(ADD) ? 1 : (m == M(SUB) || m == M(CMP)) ? 2 : 3;
        const char *op = m == M(ADD) ? "+" : (m == M(SUB) || m == M(CMP)) ? "-" : (m == M(OR)) ? "|" : (m == M(XOR)) ? "^" : "&";
        emit("{ ");
        Str da;
        int stack = 0;
        if (d->type == OP_MEM) { /* read-modify-write: the address once */
            if (!address(off, in, d, &da, &stack)) return 0;
            emit("const uint64_t ea = %s; const uint64_t a = %s(ea, %d); ", da.s, stack ? "rc_lds" : "rc_ld", size);
        } else {
            if (!read_op(off, in, d, size, &a)) return 0;
            emit("const uint64_t a = %s; ", a.s);
        }
        emit("const uint64_t b = %s; const uint64_t v = (a %s b) & 0x%" PRIx64 "ull; ", b.s, op, mask(size));
        emit("fk = %s; fa = a; fb = b; fr = v; fz = %d; ", kind == 1 ? "FK_ADD" : kind == 2 ? "FK_SUB" : "FK_LOGIC", size);
        if (m != M(CMP) && m != M(TEST)) {
            if (d->type == OP_MEM) emit("%s(ea, %d, v);", stack ? "rc_sts" : "rc_st", size);
            else {
                if (!write_op(off, in, d, "v", &w)) return 0;
                emit("%s", w.s);
            }
        }
        emit(" }\n");
        return 1;
    }
    case M(INC): case M(DEC): case M(NEG): case M(NOT): {
        const int size = d->size;
        emit("{ ");
        Str da;
        int stack = 0;
        if (d->type == OP_MEM) {
            if (!address(off, in, d, &da, &stack)) return 0;
            emit("const uint64_t ea = %s; const uint64_t a = %s(ea, %d); ", da.s, stack ? "rc_lds" : "rc_ld", size);
        } else if (gpr(d)) {
            if (!read_op(off, in, d, size, &a)) return 0;
            emit("const uint64_t a = %s; ", a.s);
        } else {
            return 0;
        }
        const char *calc = m == M(INC) ? "a + 1" : m == M(DEC) ? "a - 1" : m == M(NEG) ? "0 - a" : "~a";
        emit("const uint64_t v = (%s) & 0x%" PRIx64 "ull; ", calc, mask(size));
        if (m == M(INC) || m == M(DEC)) emit("fcf = " COND "; fk = %s; fa = a; fb = 1; fr = v; fz = %d; ", 2, m == M(INC) ? "FK_INC" : "FK_DEC", size);
        if (m == M(NEG)) emit("fk = FK_SUB; fa = 0; fb = a; fr = v; fz = %d; ", size);
        if (d->type == OP_MEM) emit("%s(ea, %d, v);", stack ? "rc_sts" : "rc_st", size);
        else {
            if (!write_op(off, in, d, "v", &w)) return 0;
            emit("%s", w.s);
        }
        emit(" }\n");
        return 1;
    }
    case M(PUSH):
        if (d->size == 2) return 0;
        if (d->type == OP_REG && d->kind != RK_GPR) return 0;
        if (!read_op(off, in, d, 8, &b)) return 0;
        if (d->type == OP_IMM) b = str("0x%" PRIx64 "ull", (uint64_t)d->disp);
        emit("{ const uint64_t v = %s; r[RSP] -= 8; rc_sts(r[RSP], 8, v); }\n", b.s);
        return 1;
    case M(POP):
        if (!gpr(d) || d->size != 8) return 0;
        emit("{ const uint64_t v = rc_lds(r[RSP], 8); r[RSP] += 8; r[%d] = v; }\n", d->reg);
        return 1;
    case M(LEAVE):
        emit("r[RSP] = r[RBP]; r[RBP] = rc_lds(r[RSP], 8); r[RSP] += 8;\n");
        return 1;
    case M(JMP):
        if (d->type == OP_IMM) {
            jump(f, off + (uint64_t)d->disp);
            emit("\n");
            return 1;
        }
        if (!read_op(off, in, d, 8, &b)) return 0;
        emit("{ dn = %s; goto L_dispatch; }\n", b.s);
        ++stats[1];
        return 1;
    case M(CALL): {
        if (d->type == OP_IMM) {
            /* A recompiled callee directly, when the program filled its slot (the game: the ones it
             * runs; the replay leaves them empty and answers each call from the record). */
            const uint64_t t = off + (uint64_t)d->disp;
            if (annotation(t)) emit("/* %s */ ", annotation(t));
            if (is_recompiled(t)) {
                emit("{ extern RcFn d_%" PRIx64 "; " SPILL " if (d_%" PRIx64 ") { c->r[RSP] -= 8; rc_st(c->r[RSP], 8, B + 0x%" PRIx64
                     "ull); c->rip = B + 0x%" PRIx64 "ull; d_%" PRIx64 "(c); } else rc->call(c, B + 0x%" PRIx64 "ull, B + 0x%" PRIx64
                     "ull); " RELOAD " fk = FK_CPU; }\n", t, t, next, t, t, t, next);
                return 1;
            }
            b = str("B + 0x%" PRIx64 "ull", t);
            if (import_name(t)) emit("/* %s */ ", import_name(t));
        } else if (!read_op(off, in, d, 8, &b)) {
            return 0;
        }
        emit("{ const uint64_t t = %s; " SPILL " rc->call(c, t, B + 0x%" PRIx64 "ull); " RELOAD " fk = FK_CPU; }\n", b.s, next);
        return 1;
    }
    case M(RET): {
        const uint64_t pop = 8 + (in->count && d->type == OP_IMM ? (uint64_t)d->disp : 0);
        emit("{ const uint64_t t = rc_lds(r[RSP], 8); r[RSP] += %" PRIu64 "; " SPILL " c->rip = t; return; }\n", pop);
        return 1;
    }
    default:
        return translate_shift_mul(off, in) || translate_extra(off, in) || translate_bits(off, in) || translate_vector(off, in);
    }
}


/* ---- vector instructions (as src/cpu/interp_vec.c does them) ---- */

/* The vector registers the function being generated keeps in locals (xmm[], see generate): the
 * ones its instructions name. A translation that reaches another is a generator bug. */
static uint32_t vused;
static int vlocal(int reg) {
    if (reg < 0 || reg > 15 || !(vused >> reg & 1)) {
        fprintf(stderr, "bbrecomp: xmm%d is not kept in a local\n", reg);
        exit(1);
    }
    return reg;
}
static int vreg(const BbOp *op) { return op->type == OP_REG && op->kind == RK_VEC && vlocal(op->reg) >= 0; }
static int vwidth(const BbInsn *in) { return in->vex && in->vl ? in->vl : 16; }

/* A vector operand's value (a BbVec expression). */
static int vread(uint64_t off, const BbInsn *in, const BbOp *op, Str *v) {
    if (vreg(op)) { *v = str("xmm[%d]", op->reg); return 1; }
    if (op->type == OP_MEM) {
        Str a;
        int stack;
        if (!address(off, in, op, &a, &stack)) return 0;
        *v = str("rc_vld(%s, %d)", a.s, op->size);
        return 1;
    }
    if (gpr(op)) {
        Str g;
        if (!read_op(off, in, op, 8, &g)) return 0;
        *v = str("rc_vgpr(%s)", g.s);
        return 1;
    }
    return 0;
}

/* A statement writing BbVec `v` (`bytes` of it to a register). */
static int vwrite(uint64_t off, const BbInsn *in, const BbOp *op, const char *v, int bytes, Str *s) {
    if (vreg(op)) { *s = str("rc_vset(&xmm[%d], %s, %d, %d);", op->reg, v, bytes, in->vex ? 1 : 0); return 1; }
    if (op->type == OP_MEM) {
        Str a;
        int stack;
        if (!address(off, in, op, &a, &stack)) return 0;
        *s = str("rc_vst(%s, %d, %s);", a.s, op->size, v);
        return 1;
    }
    return 0;
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
static VOps vops1(const BbInsn *in) {
    VOps o = {&in->op[0], NULL, NULL, NULL};
    int last = in->count - 1;
    if (in->op[last].type == OP_IMM) { o.imm = &in->op[last]; --last; }
    o.s2 = &in->op[last];
    o.s1 = &in->op[0];
    return o;
}

/* Two sources, a lane loop, the destination: `body` sets rr from a and b (i: the lane). */
static int vbinary(uint64_t off, const BbInsn *in, int lanes, int bytes, int zero, const char *body) {
    const VOps o = vops3(in);
    Str a, b, w;
    if (!vread(off, in, o.s1, &a) || !vread(off, in, o.s2, &b) || !vwrite(off, in, o.dst, "rr", bytes, &w)) return 0;
    emit("{ const BbVec a = %s, b = %s; BbVec rr = a; %s for (int i = 0; i < %d; ++i) { %s } %s }\n", a.s, b.s,
         zero ? "memset(&rr, 0, sizeof(rr));" : "", lanes, body, w.s);
    return 1;
}

static int translate_vector(uint64_t off, const BbInsn *in) {
    const int m = in->mnemonic, w = vwidth(in);
    Str a, b, s;
    switch (m) {
    case M(MOVAPS): case M(MOVUPS): case M(MOVAPD): case M(MOVUPD): case M(MOVDQA): case M(MOVDQU):
    case M(VMOVAPS): case M(VMOVUPS): case M(VMOVAPD): case M(VMOVUPD): case M(VMOVDQA): case M(VMOVDQU):
    case M(LDDQU): case M(VLDDQU):
        if (!vread(off, in, &in->op[1], &a) || !vwrite(off, in, &in->op[0], a.s, in->op[0].size, &s)) return 0;
        emit("%s\n", s.s);
        return 1;
    case M(MOVSS): case M(VMOVSS): case M(MOVSD): case M(VMOVSD): {
        if (m == M(MOVSD) && !(in->count && vreg(&in->op[0])) && !(in->count >= 2 && vreg(&in->op[1]))) return 0;
        const int bytes = (m == M(MOVSS) || m == M(VMOVSS)) ? 4 : 8;
        const BbOp *d = &in->op[0];
        if (d->type == OP_MEM) {
            Str ad;
            int stack;
            if (!vread(off, in, &in->op[in->count - 1], &a) || !address(off, in, d, &ad, &stack)) return 0;
            emit("rc_vst(%s, %d, %s);\n", ad.s, bytes, a.s);
            return 1;
        }
        if (!vreg(d)) return 0;
        if (in->count == 3) {
            if (!vread(off, in, &in->op[1], &a) || !vread(off, in, &in->op[2], &b)) return 0;
            emit("{ BbVec rr = %s; const BbVec b = %s; memcpy(rr.b, b.b, %d); rc_vset(&xmm[%d], rr, 16, %d); }\n", a.s, b.s,
                 bytes, d->reg, in->vex ? 1 : 0);
            return 1;
        }
        const BbOp *src = &in->op[1];
        if (src->type == OP_MEM) {
            Str ad;
            int stack;
            if (!address(off, in, src, &ad, &stack)) return 0;
            if (in->vex) emit("rc_vset(&xmm[%d], rc_vld(%s, %d), 16, 1);\n", d->reg, ad.s, bytes);
            else emit("{ const BbVec rr = rc_vld(%s, %d); memcpy(xmm[%d].b, rr.b, 16); }\n", ad.s, bytes, d->reg);
            return 1;
        }
        if (!vreg(src)) return 0;
        emit("{ BbVec rr = xmm[%d]; memcpy(rr.b, xmm[%d].b, %d); rc_vset(&xmm[%d], rr, 16, %d); }\n", d->reg,
             src->reg, bytes, d->reg, in->vex ? 1 : 0);
        return 1;
    }
    case M(ADDPS): case M(VADDPS): case M(SUBPS): case M(VSUBPS): case M(MULPS): case M(VMULPS):
    case M(DIVPS): case M(VDIVPS): case M(MINPS): case M(VMINPS): case M(MAXPS): case M(VMAXPS):
    case M(ADDSS): case M(VADDSS): case M(SUBSS): case M(VSUBSS): case M(MULSS): case M(VMULSS):
    case M(DIVSS): case M(VDIVSS): case M(MINSS): case M(VMINSS): case M(MAXSS): case M(VMAXSS):
    case M(ADDPD): case M(VADDPD): case M(SUBPD): case M(VSUBPD): case M(MULPD): case M(VMULPD):
    case M(DIVPD): case M(VDIVPD): case M(ADDSD): case M(VADDSD): case M(SUBSD): case M(VSUBSD):
    case M(MULSD): case M(VMULSD): case M(DIVSD): case M(VDIVSD): {
        const char *name = ZydisMnemonicGetString((ZydisMnemonic)m);
        const char *base = name[0] == 'v' ? name + 1 : name;
        const int dbl = base[strlen(base) - 1] == 'd', scalar = base[strlen(base) - 2] == 's';
        const char *f = dbl ? "fd" : "f", *t = dbl ? "double" : "float";
        const char *op = !strncmp(base, "add", 3) ? "x + y" : !strncmp(base, "sub", 3) ? "x - y"
                       : !strncmp(base, "mul", 3) ? "x * y" : !strncmp(base, "div", 3) ? "x / y"
                       : !strncmp(base, "min", 3) ? "x < y ? x : y" : "x > y ? x : y";
        const Str body = str("const %s x = a.%s[i], y = b.%s[i]; rr.%s[i] = %s;", t, f, f, f, op);
        return vbinary(off, in, scalar ? 1 : w / (dbl ? 8 : 4), scalar ? 16 : w, 0, body.s);
    }
    case M(ANDPS): case M(VANDPS): case M(ANDPD): case M(VANDPD): case M(PAND): case M(VPAND):
        return vbinary(off, in, 4, w, 0, "rr.q[i] = a.q[i] & b.q[i];");
    case M(ANDNPS): case M(VANDNPS): case M(ANDNPD): case M(VANDNPD): case M(PANDN): case M(VPANDN):
        return vbinary(off, in, 4, w, 0, "rr.q[i] = ~a.q[i] & b.q[i];");
    case M(ORPS): case M(VORPS): case M(ORPD): case M(VORPD): case M(POR): case M(VPOR):
        return vbinary(off, in, 4, w, 0, "rr.q[i] = a.q[i] | b.q[i];");
    case M(XORPS): case M(VXORPS): case M(XORPD): case M(VXORPD): case M(PXOR): case M(VPXOR):
        return vbinary(off, in, 4, w, 0, "rr.q[i] = a.q[i] ^ b.q[i];");
    case M(MOVSHDUP): case M(VMOVSHDUP): case M(MOVSLDUP): case M(VMOVSLDUP): {
        if (!vread(off, in, &in->op[1], &a) || !vwrite(off, in, &in->op[0], "rr", w, &s)) return 0;
        const int odd = m == M(MOVSHDUP) || m == M(VMOVSHDUP);
        emit("{ const BbVec a = %s; BbVec rr = a; for (int i = 0; i < %d; ++i) rr.d[i] = a.d[(i & ~1) + %d]; %s }\n",
             a.s, w / 4, odd, s.s);
        return 1;
    }
    case M(PADDD): case M(VPADDD): return vbinary(off, in, w / 4, w, 1, "rr.d[i] = a.d[i] + b.d[i];");
    case M(PSUBD): case M(VPSUBD): return vbinary(off, in, w / 4, w, 1, "rr.d[i] = a.d[i] - b.d[i];");
    case M(PADDQ): case M(VPADDQ): return vbinary(off, in, w / 8, w, 1, "rr.q[i] = a.q[i] + b.q[i];");
    case M(PSUBQ): case M(VPSUBQ): return vbinary(off, in, w / 8, w, 1, "rr.q[i] = a.q[i] - b.q[i];");
    case M(PCMPEQB): case M(VPCMPEQB): return vbinary(off, in, w, w, 1, "rr.b[i] = a.b[i] == b.b[i] ? 0xff : 0;");
    case M(PCMPEQD): case M(VPCMPEQD): return vbinary(off, in, w / 4, w, 1, "rr.d[i] = a.d[i] == b.d[i] ? ~0u : 0;");
    case M(PCMPEQQ): case M(VPCMPEQQ): return vbinary(off, in, w / 8, w, 1, "rr.q[i] = a.q[i] == b.q[i] ? ~0ull : 0;");
    case M(HADDPS): case M(VHADDPS): case M(HSUBPS): case M(VHSUBPS): {
        const char *op = (m == M(HADDPS) || m == M(VHADDPS)) ? "+" : "-";
        const Str body = str("const float *x = a.f + i * 4, *y = b.f + i * 4; rr.f[i * 4] = x[0] %s x[1]; "
                             "rr.f[i * 4 + 1] = x[2] %s x[3]; rr.f[i * 4 + 2] = y[0] %s y[1]; rr.f[i * 4 + 3] = y[2] %s y[3];",
                             op, op, op, op);
        return vbinary(off, in, w / 16, w, 1, body.s);
    }
    case M(CMPPS): case M(VCMPPS): case M(CMPPD): case M(VCMPPD): case M(CMPSS): case M(VCMPSS): case M(CMPSD): case M(VCMPSD): {
        const VOps o = vops3(in);
        if (!o.imm) return 0;
        const int predicate = (int)o.imm->disp & 31;
        const int dbl = m == M(CMPPD) || m == M(VCMPPD) || m == M(CMPSD) || m == M(VCMPSD);
        const int scalar = m == M(CMPSS) || m == M(VCMPSS) || m == M(CMPSD) || m == M(VCMPSD);
        const Str body = dbl ? str("rr.q[i] = rc_fcompare(a.fd[i], b.fd[i], %d) ? ~0ull : 0;", predicate)
                             : str("rr.d[i] = rc_fcompare(a.f[i], b.f[i], %d) ? ~0u : 0;", predicate);
        return vbinary(off, in, scalar ? 1 : w / (dbl ? 8 : 4), scalar ? 16 : w, 0, body.s);
    }
    case M(COMISS): case M(VCOMISS): case M(UCOMISS): case M(VUCOMISS): case M(COMISD): case M(VCOMISD):
    case M(UCOMISD): case M(VUCOMISD): {
        const int dbl = m == M(COMISD) || m == M(VCOMISD) || m == M(UCOMISD) || m == M(VUCOMISD);
        if (!vread(off, in, &in->op[0], &a) || !vread(off, in, &in->op[1], &b)) return 0;
        emit("{ const BbVec a = %s, b = %s; c->flags = rc_comis(c->flags, a.%s[0], b.%s[0]); fk = FK_CPU; }\n", a.s, b.s,
             dbl ? "fd" : "f", dbl ? "fd" : "f");
        return 1;
    }
    case M(SHUFPS): case M(VSHUFPS): {
        const VOps o = vops3(in);
        if (!o.imm) return 0;
        const int imm = (int)o.imm->disp;
        const Str body = str("rr.d[i * 4] = a.d[i * 4 + %d]; rr.d[i * 4 + 1] = a.d[i * 4 + %d]; rr.d[i * 4 + 2] = b.d[i * 4 + %d]; "
                             "rr.d[i * 4 + 3] = b.d[i * 4 + %d];", imm & 3, (imm >> 2) & 3, (imm >> 4) & 3, (imm >> 6) & 3);
        return vbinary(off, in, w / 16, w, 1, body.s);
    }
    case M(PSHUFD): case M(VPSHUFD): case M(VPERMILPS): {
        if (m == M(VPERMILPS) && in->op[2].type != OP_IMM) /* the variable form */
            return vbinary(off, in, w / 16, w, 1, "for (int k = 0; k < 4; ++k) rr.d[i * 4 + k] = a.d[i * 4 + (b.d[i * 4 + k] & 3)];");
        const VOps o = vops1(in);
        if (!o.imm || !vread(off, in, o.s2, &a) || !vwrite(off, in, o.dst, "rr", w, &s)) return 0;
        const int imm = (int)o.imm->disp;
        emit("{ const BbVec a = %s; BbVec rr = a; for (int h = 0; h < %d; ++h) { rr.d[h * 4] = a.d[h * 4 + %d]; "
             "rr.d[h * 4 + 1] = a.d[h * 4 + %d]; rr.d[h * 4 + 2] = a.d[h * 4 + %d]; rr.d[h * 4 + 3] = a.d[h * 4 + %d]; } %s }\n",
             a.s, w / 16, imm & 3, (imm >> 2) & 3, (imm >> 4) & 3, (imm >> 6) & 3, s.s);
        return 1;
    }
    case M(MOVMSKPS): case M(VMOVMSKPS): case M(MOVMSKPD): case M(VMOVMSKPD): case M(PMOVMSKB): case M(VPMOVMSKB): {
        if (!vread(off, in, &in->op[1], &a)) return 0;
        Str wr;
        if (!write_op(off, in, &in->op[0], "mask", &wr)) return 0;
        const char *bit = (m == M(MOVMSKPS) || m == M(VMOVMSKPS)) ? "(uint64_t)(a.d[i] >> 31)"
                        : (m == M(MOVMSKPD) || m == M(VMOVMSKPD)) ? "(a.q[i] >> 63)" : "(uint64_t)(a.b[i] >> 7)";
        const int lanes = (m == M(MOVMSKPS) || m == M(VMOVMSKPS)) ? w / 4 : (m == M(MOVMSKPD) || m == M(VMOVMSKPD)) ? w / 8 : w;
        emit("{ const BbVec a = %s; uint64_t mask = 0; for (int i = 0; i < %d; ++i) mask |= %s << i; %s }\n", a.s, lanes, bit, wr.s);
        return 1;
    }
    case M(MOVHLPS): case M(VMOVHLPS): case M(MOVLHPS): case M(VMOVLHPS): {
        const VOps o = vops3(in);
        if (!vread(off, in, o.s1, &a) || !vread(off, in, o.s2, &b) || !vwrite(off, in, o.dst, "rr", 16, &s)) return 0;
        emit("{ BbVec rr = %s; const BbVec b = %s; %s %s }\n", a.s, b.s,
             (m == M(MOVHLPS) || m == M(VMOVHLPS)) ? "rr.q[0] = b.q[1];" : "rr.q[1] = b.q[0];", s.s);
        return 1;
    }
    case M(MOVD): case M(VMOVD): case M(MOVQ): case M(VMOVQ): {
        const BbOp *d = &in->op[0], *src = &in->op[1];
        const int bytes = (m == M(MOVD) || m == M(VMOVD)) ? 4 : 8;
        if (!vread(off, in, src, &a)) return 0;
        if (vreg(d)) {
            emit("{ BbVec rr; memset(&rr, 0, sizeof(rr)); const BbVec s = %s; memcpy(rr.b, s.b, %d); memcpy(xmm[%d].b, rr.b, 16);%s }\n",
                 a.s, bytes, d->reg, in->vex ? str(" memset(xmm[%d].b + 16, 0, 16);", d->reg).s : "");
            return 1;
        }
        if (d->type == OP_MEM) {
            Str ad;
            int stack;
            if (!address(off, in, d, &ad, &stack)) return 0;
            emit("rc_vst(%s, %d, %s);\n", ad.s, bytes, a.s);
            return 1;
        }
        Str wr;
        if (!write_op(off, in, d, bytes == 4 ? "s.d[0]" : "s.q[0]", &wr)) return 0;
        emit("{ const BbVec s = %s; %s }\n", a.s, wr.s);
        return 1;
    }
    case M(DPPS): case M(VDPPS): { /* the hardware's pairwise order: (p0 + p1) + (p2 + p3) */
        const VOps o = vops3(in);
        if (!o.imm) return 0;
        const int imm = (int)o.imm->disp & 0xff;
        const Str body = str("float p[4]; for (int k = 0; k < 4; ++k) p[k] = (%d & (0x10 << k)) ? a.f[i * 4 + k] * b.f[i * 4 + k] : 0.0f; "
                             "const float sum = (p[0] + p[1]) + (p[2] + p[3]); "
                             "for (int k = 0; k < 4; ++k) rr.f[i * 4 + k] = (%d & (1 << k)) ? sum : 0.0f;", imm, imm);
        return vbinary(off, in, w / 16, w, 1, body.s);
    }
    case M(INSERTPS): case M(VINSERTPS): {
        const VOps o = vops3(in);
        if (!o.imm || !vread(off, in, o.s1, &a) || !vread(off, in, o.s2, &b) || !vwrite(off, in, o.dst, "rr", 16, &s)) return 0;
        const int imm = (int)o.imm->disp & 0xff;
        emit("{ BbVec rr = %s; const BbVec b = %s; rr.d[%d] = b.d[%d];", a.s, b.s, (imm >> 4) & 3,
             o.s2->type == OP_MEM ? 0 : (imm >> 6) & 3);
        for (int i = 0; i < 4; ++i) if (imm & (1 << i)) emit(" rr.d[%d] = 0;", i);
        emit(" %s }\n", s.s);
        return 1;
    }
    case M(BLENDVPS): case M(VBLENDVPS): case M(BLENDVPD): case M(VBLENDVPD): case M(PBLENDVB): case M(VPBLENDVB): {
        Str mk;
        if (in->vex) {
            if (in->count < 4 || !vread(off, in, &in->op[1], &a) || !vread(off, in, &in->op[2], &b) ||
                !vread(off, in, &in->op[3], &mk)) return 0;
        } else {
            if (!vread(off, in, &in->op[0], &a) || !vread(off, in, &in->op[1], &b)) return 0;
            mk = str("xmm[%d]", vlocal(0));
        }
        if (!vwrite(off, in, &in->op[0], "rr", w, &s)) return 0;
        const int es = (m == M(BLENDVPS) || m == M(VBLENDVPS)) ? 4 : (m == M(BLENDVPD) || m == M(VBLENDVPD)) ? 8 : 1;
        emit("{ BbVec rr = %s; const BbVec b = %s, mk = %s; for (int i = 0; i < %d; ++i) if (mk.b[i * %d + %d] & 0x80) "
             "memcpy(rr.b + i * %d, b.b + i * %d, %d); %s }\n", a.s, b.s, mk.s, w / es, es, es - 1, es, es, es, s.s);
        return 1;
    }
    case M(BLENDPS): case M(VBLENDPS): case M(BLENDPD): case M(VBLENDPD): case M(PBLENDW): case M(VPBLENDW): case M(VPBLENDD): {
        const VOps o = vops3(in);
        if (!o.imm || !vread(off, in, o.s1, &a) || !vread(off, in, o.s2, &b) || !vwrite(off, in, o.dst, "rr", w, &s)) return 0;
        const int imm = (int)o.imm->disp & 0xff;
        const int es = (m == M(BLENDPS) || m == M(VBLENDPS) || m == M(VPBLENDD)) ? 4 : (m == M(BLENDPD) || m == M(VBLENDPD)) ? 8 : 2;
        emit("{ BbVec rr = %s; const BbVec b = %s;", a.s, b.s);
        for (int i = 0; i < w / es; ++i)
            if (imm & (1 << (es == 2 ? (i & 7) : i))) emit(" memcpy(rr.b + %d, b.b + %d, %d);", i * es, i * es, es);
        emit(" %s }\n", s.s);
        return 1;
    }
    case M(SQRTPS): case M(VSQRTPS): case M(RSQRTPS): case M(VRSQRTPS): case M(RCPPS): case M(VRCPPS): case M(SQRTPD): case M(VSQRTPD): {
        const VOps o = vops1(in);
        if (o.imm || !vread(off, in, o.s2, &a) || !vwrite(off, in, o.dst, "rr", w, &s)) return 0;
        const char *body = (m == M(SQRTPD) || m == M(VSQRTPD)) ? "for (int i = 0; i < %d; ++i) rr.fd[i] = __builtin_sqrt(x.fd[i]);"
                         : (m == M(SQRTPS) || m == M(VSQRTPS)) ? "for (int i = 0; i < %d; ++i) rr.f[i] = __builtin_sqrtf(x.f[i]);"
                         : (m == M(RCPPS) || m == M(VRCPPS)) ? "for (int i = 0; i < %d; ++i) rr.f[i] = 1.0f / x.f[i];"
                         : "for (int i = 0; i < %d; ++i) rr.f[i] = 1.0f / __builtin_sqrtf(x.f[i]);";
        emit("{ const BbVec x = %s; BbVec rr; memset(&rr, 0, sizeof(rr)); ", a.s);
        emit(body, (m == M(SQRTPD) || m == M(VSQRTPD)) ? w / 8 : w / 4);
        emit(" %s }\n", s.s);
        return 1;
    }
    case M(SQRTSS): case M(VSQRTSS): case M(RSQRTSS): case M(VRSQRTSS): case M(RCPSS): case M(VRCPSS): case M(SQRTSD): case M(VSQRTSD): {
        const VOps o = vops3(in);
        if (o.imm || !vread(off, in, o.s1, &a) || !vread(off, in, o.s2, &b) || !vwrite(off, in, o.dst, "rr", 16, &s)) return 0;
        const char *calc = (m == M(SQRTSD) || m == M(VSQRTSD)) ? "rr.fd[0] = __builtin_sqrt(x.fd[0]);"
                         : (m == M(SQRTSS) || m == M(VSQRTSS)) ? "rr.f[0] = __builtin_sqrtf(x.f[0]);"
                         : (m == M(RCPSS) || m == M(VRCPSS)) ? "rr.f[0] = 1.0f / x.f[0];" : "rr.f[0] = 1.0f / __builtin_sqrtf(x.f[0]);";
        emit("{ BbVec rr = %s; const BbVec x = %s; %s %s }\n", a.s, b.s, calc, s.s);
        return 1;
    }
    case M(CVTSI2SS): case M(VCVTSI2SS): case M(CVTSI2SD): case M(VCVTSI2SD): {
        const VOps o = vops3(in);
        Str g;
        if (o.imm || (o.s2->size != 4 && o.s2->size != 8) || !vread(off, in, o.s1, &a) || !read_op(off, in, o.s2, o.s2->size, &g) ||
            !vwrite(off, in, o.dst, "rr", 16, &s)) return 0;
        emit("{ BbVec rr = %s; const int64_t x = (int64_t)(%s)(%s); %s %s }\n", a.s, o.s2->size == 8 ? "int64_t" : "int32_t", g.s,
             (m == M(CVTSI2SS) || m == M(VCVTSI2SS)) ? "rr.f[0] = (float)x;" : "rr.fd[0] = (double)x;", s.s);
        return 1;
    }
    case M(CVTTSS2SI): case M(VCVTTSS2SI): case M(CVTSS2SI): case M(VCVTSS2SI): case M(CVTTSD2SI): case M(VCVTTSD2SI):
    case M(CVTSD2SI): case M(VCVTSD2SI): {
        const BbOp *d = &in->op[0];
        Str wr;
        if (!gpr(d) || (d->size != 4 && d->size != 8) || !vread(off, in, &in->op[1], &a)) return 0;
        const int truncate = m == M(CVTTSS2SI) || m == M(VCVTTSS2SI) || m == M(CVTTSD2SI) || m == M(VCVTTSD2SI);
        const int dbl = m == M(CVTTSD2SI) || m == M(VCVTTSD2SI) || m == M(CVTSD2SI) || m == M(VCVTSD2SI);
        if (!write_op(off, in, d, d->size == 8 ? str("(uint64_t)rc_f2i64(x, %d, c->mxcsr)", truncate).s
                                               : str("(uint32_t)rc_f2i32(x, %d, c->mxcsr)", truncate).s, &wr)) return 0;
        emit("{ const BbVec sv = %s; const double x = sv.%s[0]; %s }\n", a.s, dbl ? "fd" : "f", wr.s);
        return 1;
    }
    case M(CVTDQ2PS): case M(VCVTDQ2PS): case M(CVTPS2DQ): case M(VCVTPS2DQ): case M(CVTTPS2DQ): case M(VCVTTPS2DQ): {
        if (!vread(off, in, &in->op[1], &a) || !vwrite(off, in, &in->op[0], "rr", w, &s)) return 0;
        const char *calc = (m == M(CVTDQ2PS) || m == M(VCVTDQ2PS)) ? "rr.f[i] = (float)x.sd[i];"
                         : (m == M(CVTTPS2DQ) || m == M(VCVTTPS2DQ)) ? "rr.sd[i] = rc_f2i32(x.f[i], 1, c->mxcsr);"
                         : "rr.sd[i] = rc_f2i32(x.f[i], 0, c->mxcsr);";
        emit("{ const BbVec x = %s; BbVec rr; memset(&rr, 0, sizeof(rr)); for (int i = 0; i < %d; ++i) %s %s }\n", a.s, w / 4, calc, s.s);
        return 1;
    }
    case M(VBROADCASTSS): case M(VBROADCASTSD): case M(VBROADCASTF128): case M(VPBROADCASTD): case M(VPBROADCASTQ):
    case M(VPBROADCASTB): case M(VPBROADCASTW): {
        if (!vread(off, in, &in->op[1], &a) || !vwrite(off, in, &in->op[0], "rr", w, &s)) return 0;
        const int es = (m == M(VBROADCASTSS) || m == M(VPBROADCASTD)) ? 4 : (m == M(VBROADCASTSD) || m == M(VPBROADCASTQ)) ? 8
                     : m == M(VPBROADCASTB) ? 1 : m == M(VPBROADCASTW) ? 2 : 16;
        emit("{ const BbVec x = %s; BbVec rr; memset(&rr, 0, sizeof(rr)); for (int i = 0; i < %d; i += %d) memcpy(rr.b + i, x.b, %d); %s }\n",
             a.s, w, es, es, s.s);
        return 1;
    }
    case M(UNPCKLPS): case M(VUNPCKLPS): case M(UNPCKHPS): case M(VUNPCKHPS):
        return vbinary(off, in, w / 16, w, 1, (m == M(UNPCKLPS) || m == M(VUNPCKLPS))
            ? "rr.d[i * 4] = a.d[i * 4]; rr.d[i * 4 + 1] = b.d[i * 4]; rr.d[i * 4 + 2] = a.d[i * 4 + 1]; rr.d[i * 4 + 3] = b.d[i * 4 + 1];"
            : "rr.d[i * 4] = a.d[i * 4 + 2]; rr.d[i * 4 + 1] = b.d[i * 4 + 2]; rr.d[i * 4 + 2] = a.d[i * 4 + 3]; rr.d[i * 4 + 3] = b.d[i * 4 + 3];");
    case M(UNPCKLPD): case M(VUNPCKLPD): case M(UNPCKHPD): case M(VUNPCKHPD):
        return vbinary(off, in, w / 16, w, 1, (m == M(UNPCKLPD) || m == M(VUNPCKLPD))
            ? "rr.q[i * 2] = a.q[i * 2]; rr.q[i * 2 + 1] = b.q[i * 2];" : "rr.q[i * 2] = a.q[i * 2 + 1]; rr.q[i * 2 + 1] = b.q[i * 2 + 1];");
    case M(PSLLW): case M(VPSLLW): case M(PSLLD): case M(VPSLLD): case M(PSLLQ): case M(VPSLLQ): case M(PSRLW): case M(VPSRLW):
    case M(PSRLD): case M(VPSRLD): case M(PSRLQ): case M(VPSRLQ): case M(PSRAW): case M(VPSRAW): case M(PSRAD): case M(VPSRAD):
    case M(PSLLDQ): case M(VPSLLDQ): case M(PSRLDQ): case M(VPSRLDQ): {
        const BbOp *src = in->vex ? &in->op[1] : &in->op[0], *cnt = in->vex ? &in->op[2] : &in->op[1];
        Str n;
        if (!vread(off, in, src, &a) || !vwrite(off, in, &in->op[0], "rr", w, &s)) return 0;
        if (cnt->type == OP_IMM) n = str("%" PRIu64 "ull", (uint64_t)cnt->disp & 0xff);
        else if (!vread(off, in, cnt, &b)) return 0;
        else n = str("(%s).q[0]", b.s);
        const char *body;
        switch (m) {
        case M(PSLLW): case M(VPSLLW): body = "for (int i = 0; i < %d / 2; ++i) rr.w[i] = n > 15 ? 0 : (uint16_t)(x.w[i] << n);"; break;
        case M(PSLLD): case M(VPSLLD): body = "for (int i = 0; i < %d / 4; ++i) rr.d[i] = n > 31 ? 0 : x.d[i] << n;"; break;
        case M(PSLLQ): case M(VPSLLQ): body = "for (int i = 0; i < %d / 8; ++i) rr.q[i] = n > 63 ? 0 : x.q[i] << n;"; break;
        case M(PSRLW): case M(VPSRLW): body = "for (int i = 0; i < %d / 2; ++i) rr.w[i] = n > 15 ? 0 : (uint16_t)(x.w[i] >> n);"; break;
        case M(PSRLD): case M(VPSRLD): body = "for (int i = 0; i < %d / 4; ++i) rr.d[i] = n > 31 ? 0 : x.d[i] >> n;"; break;
        case M(PSRLQ): case M(VPSRLQ): body = "for (int i = 0; i < %d / 8; ++i) rr.q[i] = n > 63 ? 0 : x.q[i] >> n;"; break;
        case M(PSRAW): case M(VPSRAW): body = "for (int i = 0; i < %d / 2; ++i) rr.sw[i] = (int16_t)(x.sw[i] >> (n > 15 ? 15 : n));"; break;
        case M(PSRAD): case M(VPSRAD): body = "for (int i = 0; i < %d / 4; ++i) rr.sd[i] = x.sd[i] >> (n > 31 ? 31 : n);"; break;
        case M(PSLLDQ): case M(VPSLLDQ):
            body = "for (int h = 0; h < %d / 16; ++h) for (int i = 0; i < 16; ++i) rr.b[h * 16 + i] = (uint64_t)i >= n ? x.b[h * 16 + i - n] : 0;"; break;
        default:
            body = "for (int h = 0; h < %d / 16; ++h) for (int i = 0; i < 16; ++i) rr.b[h * 16 + i] = i + n < 16 ? x.b[h * 16 + i + n] : 0;"; break;
        }
        emit("{ const BbVec x = %s; const uint64_t n = %s; BbVec rr; memset(&rr, 0, sizeof(rr)); ", a.s, n.s);
        emit(body, w);
        emit(" %s }\n", s.s);
        return 1;
    }
    case M(EXTRACTPS): case M(VEXTRACTPS): case M(PEXTRD): case M(VPEXTRD): case M(PEXTRQ): case M(VPEXTRQ):
    case M(PEXTRW): case M(VPEXTRW): case M(PEXTRB): case M(VPEXTRB): {
        if (in->count < 3 || in->op[2].type != OP_IMM || !vread(off, in, &in->op[1], &a)) return 0;
        const int imm = (int)in->op[2].disp;
        const Str v = (m == M(PEXTRQ) || m == M(VPEXTRQ)) ? str("x.q[%d]", imm & 1) : (m == M(PEXTRW) || m == M(VPEXTRW)) ? str("x.w[%d]", imm & 7)
                    : (m == M(PEXTRB) || m == M(VPEXTRB)) ? str("x.b[%d]", imm & 15) : str("x.d[%d]", imm & 3);
        BbOp dst = in->op[0];
        if (dst.type == OP_REG && dst.size < 4) dst.size = 4;
        Str wr;
        if (!write_op(off, in, &dst, v.s, &wr)) return 0;
        emit("{ const BbVec x = %s; %s }\n", a.s, wr.s);
        return 1;
    }
    case M(PINSRD): case M(VPINSRD): case M(PINSRQ): case M(VPINSRQ): case M(PINSRW): case M(VPINSRW): case M(PINSRB): case M(VPINSRB): {
        const VOps o = vops3(in);
        Str g;
        if (!o.imm || !vread(off, in, o.s1, &a) || !read_op(off, in, o.s2, o.s2->size, &g) || !vwrite(off, in, o.dst, "rr", 16, &s)) return 0;
        const int imm = (int)o.imm->disp;
        const Str set = (m == M(PINSRQ) || m == M(VPINSRQ)) ? str("rr.q[%d] = v;", imm & 1)
                      : (m == M(PINSRW) || m == M(VPINSRW)) ? str("rr.w[%d] = (uint16_t)v;", imm & 7)
                      : (m == M(PINSRB) || m == M(VPINSRB)) ? str("rr.b[%d] = (uint8_t)v;", imm & 15) : str("rr.d[%d] = (uint32_t)v;", imm & 3);
        emit("{ BbVec rr = %s; const uint64_t v = %s; %s %s }\n", a.s, g.s, set.s, s.s);
        return 1;
    }
    case M(PALIGNR): case M(VPALIGNR): {
        const VOps o = vops3(in);
        if (!o.imm) return 0;
        const Str body = str("uint8_t cat[32]; memcpy(cat, b.b + i * 16, 16); memcpy(cat + 16, a.b + i * 16, 16); "
                             "for (int k = 0; k < 16; ++k) rr.b[i * 16 + k] = k + %d < 32 ? cat[k + %d] : 0;",
                             (int)o.imm->disp & 0xff, (int)o.imm->disp & 0xff);
        return vbinary(off, in, w / 16, w, 1, body.s);
    }
    case M(PSHUFB): case M(VPSHUFB):
        return vbinary(off, in, w, w, 1, "rr.b[i] = (b.b[i] & 0x80) ? 0 : a.b[(i & ~15) + (b.b[i] & 15)];");
    case M(PUNPCKLBW): case M(VPUNPCKLBW): case M(PUNPCKHBW): case M(VPUNPCKHBW): case M(PUNPCKLWD): case M(VPUNPCKLWD):
    case M(PUNPCKHWD): case M(VPUNPCKHWD): case M(PUNPCKLDQ): case M(VPUNPCKLDQ): case M(PUNPCKHDQ): case M(VPUNPCKHDQ):
    case M(PUNPCKLQDQ): case M(VPUNPCKLQDQ): case M(PUNPCKHQDQ): case M(VPUNPCKHQDQ): {
        const char *name = ZydisMnemonicGetString((ZydisMnemonic)m);
        const char *kind = strstr(name, "unpck") + 5; /* lbw, hwd, ldq, hqdq... */
        const int high = kind[0] == 'h';
        const int es = !strcmp(kind + 1, "bw") ? 1 : !strcmp(kind + 1, "wd") ? 2 : !strcmp(kind + 1, "dq") ? 4 : 8;
        const Str body = str("for (int k = 0; k < %d; ++k) { memcpy(rr.b + i * 16 + k * %d, a.b + i * 16 + %d + k * %d, %d); "
                             "memcpy(rr.b + i * 16 + k * %d + %d, b.b + i * 16 + %d + k * %d, %d); }",
                             8 / es, 2 * es, high ? 8 : 0, es, es, 2 * es, es, high ? 8 : 0, es, es);
        return vbinary(off, in, w / 16, w, 1, body.s);
    }
    case M(PACKSSDW): case M(VPACKSSDW): case M(PACKUSDW): case M(VPACKUSDW):
    case M(PACKSSWB): case M(VPACKSSWB): case M(PACKUSWB): case M(VPACKUSWB): {
        const int dw = m == M(PACKSSDW) || m == M(VPACKSSDW) || m == M(PACKUSDW) || m == M(VPACKUSDW);
        const int us = m == M(PACKUSDW) || m == M(VPACKUSDW) || m == M(PACKUSWB) || m == M(VPACKUSWB);
        const Str body = dw ? str("for (int k = 0; k < 4; ++k) { rr.%s[i * 8 + k] = %s(a.sd[i * 4 + k]); rr.%s[i * 8 + 4 + k] = %s(b.sd[i * 4 + k]); }",
                                  us ? "w" : "sw", us ? "rc_usat16" : "rc_sat16", us ? "w" : "sw", us ? "rc_usat16" : "rc_sat16")
                            : str("for (int k = 0; k < 8; ++k) { rr.%s[i * 16 + k] = %s(a.sw[i * 8 + k]); rr.%s[i * 16 + 8 + k] = %s(b.sw[i * 8 + k]); }",
                                  us ? "b" : "sb", us ? "rc_usat8" : "rc_sat8", us ? "b" : "sb", us ? "rc_usat8" : "rc_sat8");
        return vbinary(off, in, w / 16, w, 1, body.s);
    }
    case M(MOVLPS): case M(VMOVLPS): case M(MOVLPD): case M(VMOVLPD): case M(MOVHPS): case M(VMOVHPS): case M(MOVHPD): case M(VMOVHPD): {
        const int high = m == M(MOVHPS) || m == M(VMOVHPS) || m == M(MOVHPD) || m == M(VMOVHPD);
        const BbOp *d = &in->op[0], *last = &in->op[in->count - 1];
        Str ad;
        int stack;
        if (d->type == OP_MEM) {
            if (!vread(off, in, &in->op[1], &a) || !address(off, in, d, &ad, &stack)) return 0;
            emit("{ const BbVec x = %s; rc_st(%s, 8, x.q[%d]); }\n", a.s, ad.s, high);
            return 1;
        }
        if (!vreg(d) || last->type != OP_MEM || !vread(off, in, in->count == 3 ? &in->op[1] : d, &a) ||
            !address(off, in, last, &ad, &stack) || !vwrite(off, in, d, "rr", 16, &s)) return 0;
        emit("{ BbVec rr = %s; rr.q[%d] = rc_ld(%s, 8); %s }\n", a.s, high, ad.s, s.s);
        return 1;
    }
    case M(VINSERTF128): case M(VINSERTI128):
        if (in->count < 4 || in->op[3].type != OP_IMM || !vread(off, in, &in->op[1], &a) || !vread(off, in, &in->op[2], &b) ||
            !vwrite(off, in, &in->op[0], "rr", 32, &s)) return 0;
        emit("{ BbVec rr = %s; const BbVec x = %s; memcpy(rr.b + %d, x.b, 16); %s }\n", a.s, b.s, (in->op[3].disp & 1) ? 16 : 0, s.s);
        return 1;
    case M(VEXTRACTF128): case M(VEXTRACTI128):
        if (in->count < 3 || in->op[2].type != OP_IMM || !vread(off, in, &in->op[1], &a) || !vwrite(off, in, &in->op[0], "rr", 16, &s)) return 0;
        emit("{ const BbVec x = %s; BbVec rr; memset(&rr, 0, sizeof(rr)); memcpy(rr.b, x.b + %d, 16); %s }\n", a.s,
             (in->op[2].disp & 1) ? 16 : 0, s.s);
        return 1;
    case M(VPERM2F128): case M(VPERM2I128): {
        if (in->count < 4 || in->op[3].type != OP_IMM || !vread(off, in, &in->op[1], &a) || !vread(off, in, &in->op[2], &b) ||
            !vwrite(off, in, &in->op[0], "rr", 32, &s)) return 0;
        const int imm = (int)in->op[3].disp;
        emit("{ const BbVec a = %s, b = %s; BbVec rr;", a.s, b.s);
        for (int h = 0; h < 2; ++h) {
            const int sel = (imm >> (h * 4)) & 0xf;
            if (sel & 8) emit(" memset(rr.b + %d, 0, 16);", h * 16);
            else emit(" memcpy(rr.b + %d, %s.b + %d, 16);", h * 16, (sel & 2) ? "b" : "a", (sel & 1) ? 16 : 0);
        }
        emit(" %s }\n", s.s);
        return 1;
    }
    case M(PTEST): case M(VPTEST): case M(VTESTPS): {
        if (!vread(off, in, &in->op[0], &a) || !vread(off, in, &in->op[1], &b)) return 0;
        const uint64_t lane = m == M(VTESTPS) ? UINT64_C(0x8000000080000000) : ~UINT64_C(0);
        emit("{ const BbVec a = %s, b = %s; uint64_t and_ = 0, andn = 0; for (int i = 0; i < %d; ++i) { and_ |= a.q[i] & b.q[i] & 0x%" PRIx64 "ull; "
             "andn |= ~a.q[i] & b.q[i] & 0x%" PRIx64 "ull; } c->flags = (" FLAGS_NOW " & ~(uint64_t)F_ARITH) | (and_ ? 0 : F_ZF) | (andn ? 0 : F_CF); "
             "fk = FK_CPU; }\n", a.s, b.s, w / 8, lane, lane);
        return 1;
    }
    case M(CVTSS2SD): case M(VCVTSS2SD): case M(CVTSD2SS): case M(VCVTSD2SS): {
        const VOps o = vops3(in);
        if (o.imm || !vread(off, in, o.s1, &a) || !vread(off, in, o.s2, &b) || !vwrite(off, in, o.dst, "rr", 16, &s)) return 0;
        emit("{ BbVec rr = %s; const BbVec x = %s; %s %s }\n", a.s, b.s,
             (m == M(CVTSS2SD) || m == M(VCVTSS2SD)) ? "rr.fd[0] = x.f[0];" : "rr.f[0] = (float)x.fd[0];", s.s);
        return 1;
    }
    case M(ROUNDSS): case M(VROUNDSS): case M(ROUNDSD): case M(VROUNDSD): {
        const VOps o = vops3(in);
        if (!o.imm || !vread(off, in, o.s1, &a) || !vread(off, in, o.s2, &b) || !vwrite(off, in, o.dst, "rr", 16, &s)) return 0;
        const int imm = (int)o.imm->disp;
        emit("{ BbVec rr = %s; const BbVec x = %s; %s %s }\n", a.s, b.s, (m == M(ROUNDSD) || m == M(VROUNDSD))
             ? str("rr.fd[0] = rc_round_mode(x.fd[0], %d, c->mxcsr);", imm).s : str("rr.f[0] = (float)rc_round_mode(x.f[0], %d, c->mxcsr);", imm).s, s.s);
        return 1;
    }
    case M(ROUNDPS): case M(VROUNDPS): case M(ROUNDPD): case M(VROUNDPD): {
        const VOps o = vops1(in);
        if (!o.imm || !vread(off, in, o.s2, &a) || !vwrite(off, in, o.dst, "rr", w, &s)) return 0;
        const int imm = (int)o.imm->disp, dbl = m == M(ROUNDPD) || m == M(VROUNDPD);
        emit("{ const BbVec x = %s; BbVec rr; memset(&rr, 0, sizeof(rr)); for (int i = 0; i < %d; ++i) %s %s }\n", a.s, dbl ? w / 8 : w / 4,
             dbl ? str("rr.fd[i] = rc_round_mode(x.fd[i], %d, c->mxcsr);", imm).s : str("rr.f[i] = (float)rc_round_mode(x.f[i], %d, c->mxcsr);", imm).s, s.s);
        return 1;
    }
    case M(PMOVZXBW): case M(VPMOVZXBW): case M(PMOVZXBD): case M(VPMOVZXBD): case M(PMOVZXWD): case M(VPMOVZXWD):
    case M(PMOVZXDQ): case M(VPMOVZXDQ): case M(PMOVSXBW): case M(VPMOVSXBW): case M(PMOVSXWD): case M(VPMOVSXWD):
    case M(PMOVSXDQ): case M(VPMOVSXDQ): case M(PMOVSXBD): case M(VPMOVSXBD): {
        if (!vread(off, in, &in->op[1], &a) || !vwrite(off, in, &in->op[0], "rr", w, &s)) return 0;
        const char *body;
        int lanes;
        switch (m) {
        case M(PMOVZXBW): case M(VPMOVZXBW): body = "rr.w[i] = x.b[i];"; lanes = w / 2; break;
        case M(PMOVZXBD): case M(VPMOVZXBD): body = "rr.d[i] = x.b[i];"; lanes = w / 4; break;
        case M(PMOVZXWD): case M(VPMOVZXWD): body = "rr.d[i] = x.w[i];"; lanes = w / 4; break;
        case M(PMOVZXDQ): case M(VPMOVZXDQ): body = "rr.q[i] = x.d[i];"; lanes = w / 8; break;
        case M(PMOVSXBW): case M(VPMOVSXBW): body = "rr.sw[i] = x.sb[i];"; lanes = w / 2; break;
        case M(PMOVSXWD): case M(VPMOVSXWD): body = "rr.sd[i] = x.sw[i];"; lanes = w / 4; break;
        case M(PMOVSXDQ): case M(VPMOVSXDQ): body = "rr.sq[i] = x.sd[i];"; lanes = w / 8; break;
        default: body = "rr.sd[i] = x.sb[i];"; lanes = w / 4; break;
        }
        emit("{ const BbVec x = %s; BbVec rr; memset(&rr, 0, sizeof(rr)); for (int i = 0; i < %d; ++i) %s %s }\n", a.s, lanes, body, s.s);
        return 1;
    }
    case M(SHUFPD): case M(VSHUFPD): {
        const VOps o = vops3(in);
        if (!o.imm) return 0;
        const int imm = (int)o.imm->disp;
        const Str body = str("rr.q[i * 2] = a.q[i * 2 + ((%d >> (i * 2)) & 1)]; rr.q[i * 2 + 1] = b.q[i * 2 + ((%d >> (i * 2 + 1)) & 1)];", imm, imm);
        return vbinary(off, in, w / 16, w, 1, body.s);
    }
    case M(VPERMILPD): case M(PSHUFLW): case M(VPSHUFLW): case M(PSHUFHW): case M(VPSHUFHW): {
        if (m == M(VPERMILPD) && in->op[2].type != OP_IMM) /* the variable form */
            return vbinary(off, in, w / 16, w, 1, "for (int k = 0; k < 2; ++k) rr.q[i * 2 + k] = a.q[i * 2 + ((b.q[i * 2 + k] >> 1) & 1)];");
        const VOps o = vops1(in);
        if (!o.imm || !vread(off, in, o.s2, &a) || !vwrite(off, in, o.dst, "rr", w, &s)) return 0;
        const int imm = (int)o.imm->disp;
        const Str body = m == M(VPERMILPD) ? str("for (int k = 0; k < 2; ++k) rr.q[h * 2 + k] = a.q[h * 2 + ((%d >> (h * 2 + k)) & 1)];", imm)
                       : (m == M(PSHUFLW) || m == M(VPSHUFLW)) ? str("for (int k = 0; k < 4; ++k) rr.w[h * 8 + k] = a.w[h * 8 + ((%d >> (k * 2)) & 3)];", imm)
                       : str("for (int k = 0; k < 4; ++k) rr.w[h * 8 + 4 + k] = a.w[h * 8 + 4 + ((%d >> (k * 2)) & 3)];", imm);
        emit("{ const BbVec a = %s; BbVec rr = a; for (int h = 0; h < %d; ++h) { %s } %s }\n", a.s, w / 16, body.s, s.s);
        return 1;
    }
    default: {
        /* Integer lanes (the interpreter's ibinary): rr.field[i] = (type)(expr). */
        static const struct { int m, vm, es; const char *field, *type, *expr; } lanes[] = {
            {M(PADDB), M(VPADDB), 1, "b", "uint8_t", "a.b[i] + b.b[i]"},
            {M(PADDW), M(VPADDW), 2, "w", "uint16_t", "a.w[i] + b.w[i]"},
            {M(PSUBB), M(VPSUBB), 1, "b", "uint8_t", "a.b[i] - b.b[i]"},
            {M(PSUBW), M(VPSUBW), 2, "w", "uint16_t", "a.w[i] - b.w[i]"},
            {M(PADDSW), M(VPADDSW), 2, "sw", "int16_t", "rc_sat16(a.sw[i] + b.sw[i])"},
            {M(PADDUSW), M(VPADDUSW), 2, "w", "uint16_t", "rc_usat16(a.w[i] + b.w[i])"},
            {M(PSUBSW), M(VPSUBSW), 2, "sw", "int16_t", "rc_sat16(a.sw[i] - b.sw[i])"},
            {M(PSUBUSW), M(VPSUBUSW), 2, "w", "uint16_t", "rc_usat16(a.w[i] - b.w[i])"},
            {M(PADDSB), M(VPADDSB), 1, "sb", "int8_t", "rc_sat8(a.sb[i] + b.sb[i])"},
            {M(PADDUSB), M(VPADDUSB), 1, "b", "uint8_t", "rc_usat8(a.b[i] + b.b[i])"},
            {M(PSUBSB), M(VPSUBSB), 1, "sb", "int8_t", "rc_sat8(a.sb[i] - b.sb[i])"},
            {M(PSUBUSB), M(VPSUBUSB), 1, "b", "uint8_t", "rc_usat8(a.b[i] - b.b[i])"},
            {M(PMULLW), M(VPMULLW), 2, "w", "uint16_t", "a.sw[i] * b.sw[i]"},
            {M(PMULHW), M(VPMULHW), 2, "w", "uint16_t", "(a.sw[i] * b.sw[i]) >> 16"},
            {M(PMULHUW), M(VPMULHUW), 2, "w", "uint16_t", "((uint32_t)a.w[i] * b.w[i]) >> 16"},
            {M(PMULLD), M(VPMULLD), 4, "d", "uint32_t", "(uint32_t)((int64_t)a.sd[i] * b.sd[i])"},
            {M(PMULUDQ), M(VPMULUDQ), 8, "q", "uint64_t", "(uint64_t)a.d[i * 2] * b.d[i * 2]"},
            {M(PMULDQ), M(VPMULDQ), 8, "q", "uint64_t", "(uint64_t)((int64_t)a.sd[i * 2] * b.sd[i * 2])"},
            {M(PAVGB), M(VPAVGB), 1, "b", "uint8_t", "(a.b[i] + b.b[i] + 1) >> 1"},
            {M(PAVGW), M(VPAVGW), 2, "w", "uint16_t", "(a.w[i] + b.w[i] + 1) >> 1"},
            {M(PMINSD), M(VPMINSD), 4, "sd", "int32_t", "a.sd[i] < b.sd[i] ? a.sd[i] : b.sd[i]"},
            {M(PMAXSD), M(VPMAXSD), 4, "sd", "int32_t", "a.sd[i] > b.sd[i] ? a.sd[i] : b.sd[i]"},
            {M(PMINUD), M(VPMINUD), 4, "d", "uint32_t", "a.d[i] < b.d[i] ? a.d[i] : b.d[i]"},
            {M(PMAXUD), M(VPMAXUD), 4, "d", "uint32_t", "a.d[i] > b.d[i] ? a.d[i] : b.d[i]"},
            {M(PMINSW), M(VPMINSW), 2, "sw", "int16_t", "a.sw[i] < b.sw[i] ? a.sw[i] : b.sw[i]"},
            {M(PMAXSW), M(VPMAXSW), 2, "sw", "int16_t", "a.sw[i] > b.sw[i] ? a.sw[i] : b.sw[i]"},
            {M(PMINUW), M(VPMINUW), 2, "w", "uint16_t", "a.w[i] < b.w[i] ? a.w[i] : b.w[i]"},
            {M(PMAXUW), M(VPMAXUW), 2, "w", "uint16_t", "a.w[i] > b.w[i] ? a.w[i] : b.w[i]"},
            {M(PMINUB), M(VPMINUB), 1, "b", "uint8_t", "a.b[i] < b.b[i] ? a.b[i] : b.b[i]"},
            {M(PMAXUB), M(VPMAXUB), 1, "b", "uint8_t", "a.b[i] > b.b[i] ? a.b[i] : b.b[i]"},
            {M(PMINSB), M(VPMINSB), 1, "sb", "int8_t", "a.sb[i] < b.sb[i] ? a.sb[i] : b.sb[i]"},
            {M(PMAXSB), M(VPMAXSB), 1, "sb", "int8_t", "a.sb[i] > b.sb[i] ? a.sb[i] : b.sb[i]"},
            {M(PCMPEQW), M(VPCMPEQW), 2, "w", "uint16_t", "a.w[i] == b.w[i] ? 0xffff : 0"},
            {M(PCMPGTB), M(VPCMPGTB), 1, "b", "uint8_t", "a.sb[i] > b.sb[i] ? 0xff : 0"},
            {M(PCMPGTW), M(VPCMPGTW), 2, "w", "uint16_t", "a.sw[i] > b.sw[i] ? 0xffff : 0"},
            {M(PCMPGTD), M(VPCMPGTD), 4, "d", "uint32_t", "a.sd[i] > b.sd[i] ? ~0u : 0"},
            {M(PCMPGTQ), M(VPCMPGTQ), 8, "q", "uint64_t", "a.sq[i] > b.sq[i] ? ~0ull : 0"},
            {M(PABSD), M(VPABSD), 4, "d", "uint32_t", "b.sd[i] < 0 ? -(uint32_t)b.sd[i] : (uint32_t)b.sd[i]"},
            {M(PABSW), M(VPABSW), 2, "w", "uint16_t", "b.sw[i] < 0 ? -b.sw[i] : b.sw[i]"},
            {M(PABSB), M(VPABSB), 1, "b", "uint8_t", "b.sb[i] < 0 ? -b.sb[i] : b.sb[i]"},
        };
        for (size_t k = 0; k < sizeof(lanes) / sizeof(lanes[0]); ++k)
            if (lanes[k].m == m || lanes[k].vm == m)
                return vbinary(off, in, w / lanes[k].es, w, 1, str("rr.%s[i] = (%s)(%s);", lanes[k].field, lanes[k].type, lanes[k].expr).s);
        return 0;
    }
    }
}

/* Locked read-modify-writes (and xadd, cmpxchg): a compare-and-exchange loop, as the interpreter. */
static int translate_atomic(uint64_t off, const BbInsn *in) {
    const int m = in->mnemonic;
    const BbOp *d = &in->op[0], *src = &in->op[1];
    if (d->type != OP_MEM) return 0;
    const int size = d->size;
    if (size != 1 && size != 2 && size != 4 && size != 8) return 0;
    Str ad, b, w;
    int stack;
    if (!address(off, in, d, &ad, &stack)) return 0;
    const uint64_t mk = mask(size);
    switch (m) {
    case M(ADD): case M(SUB): case M(AND): case M(OR): case M(XOR): {
        const char *op = m == M(ADD) ? "+" : m == M(SUB) ? "-" : m == M(AND) ? "&" : m == M(OR) ? "|" : "^";
        if (!read_op(off, in, src, size, &b)) return 0;
        emit("{ const uint64_t ea = %s; const uint64_t b = %s; uint64_t a, seen; do a = rc_ld(ea, %d); "
             "while (!rc_cas(ea, %d, a, (a %s b) & 0x%" PRIx64 "ull, &seen)); const uint64_t v = (a %s b) & 0x%" PRIx64 "ull; "
             "fk = %s; fa = a; fb = b; fr = v; fz = %d; }\n", ad.s, b.s, size, size, op, mk, op, mk,
             m == M(ADD) ? "FK_ADD" : m == M(SUB) ? "FK_SUB" : "FK_LOGIC", size);
        return 1;
    }
    case M(INC): case M(DEC): {
        const char *op = m == M(INC) ? "+" : "-";
        emit("{ const uint64_t ea = %s; fcf = " COND "; uint64_t a, seen; do a = rc_ld(ea, %d); "
             "while (!rc_cas(ea, %d, a, (a %s 1) & 0x%" PRIx64 "ull, &seen)); const uint64_t v = (a %s 1) & 0x%" PRIx64 "ull; "
             "fk = %s; fa = a; fb = 1; fr = v; fz = %d; }\n", ad.s, 2, size, size, op, mk, op, mk,
             m == M(INC) ? "FK_INC" : "FK_DEC", size);
        return 1;
    }
    case M(XADD): {
        if (!gpr(src) || !read_op(off, in, src, size, &b) || !write_op(off, in, src, "a", &w)) return 0;
        emit("{ const uint64_t ea = %s; const uint64_t b = %s; uint64_t a, seen; do a = rc_ld(ea, %d); "
             "while (!rc_cas(ea, %d, a, (a + b) & 0x%" PRIx64 "ull, &seen)); fk = FK_ADD; fa = a; fb = b; "
             "fr = (a + b) & 0x%" PRIx64 "ull; fz = %d; %s }\n", ad.s, b.s, size, size, mk, mk, size, w.s);
        return 1;
    }
    case M(CMPXCHG): {
        const BbOp rax = {.type = OP_REG, .kind = RK_GPR, .reg = RAX, .size = (uint8_t)size};
        Str e;
        if (!gpr(src) || !read_op(off, in, src, size, &b) || !read_op(off, in, &rax, size, &e) ||
            !write_op(off, in, &rax, "seen", &w))
            return 0;
        emit("{ const uint64_t ea = %s; const uint64_t e = (%s) & 0x%" PRIx64 "ull; uint64_t seen; "
             "const int ok = rc_cas(ea, %d, e, %s, &seen); fk = FK_SUB; fa = e; fb = seen; fr = (e - seen) & 0x%" PRIx64 "ull; "
             "fz = %d; if (!ok) { %s } }\n", ad.s, e.s, mk, size, b.s, mk, size, w.s);
        return 1;
    }
    default:
        return 0;
    }
}

/* Sign extensions of rax; adc/sbb; bt/bts/btr/btc; shifts by cl. */
static int translate_extra(uint64_t off, const BbInsn *in) {
    const int m = in->mnemonic;
    const BbOp *d = &in->op[0], *src = &in->op[1];
    static const char *const sext[9] = {0, "int8_t", "int16_t", 0, "int32_t", 0, 0, 0, "int64_t"};
    Str a, b, w, ad;
    int stack = 0;
    switch (m) {
    case M(CBW): emit("r[RAX] = (r[RAX] & ~0xffffull) | ((uint64_t)(int16_t)(int8_t)r[RAX] & 0xffff);\n"); return 1;
    case M(CWDE): emit("r[RAX] = (uint32_t)(int32_t)(int16_t)r[RAX];\n"); return 1;
    case M(CDQE): emit("r[RAX] = (uint64_t)(int64_t)(int32_t)r[RAX];\n"); return 1;
    case M(CWD): emit("r[RDX] = (r[RDX] & ~0xffffull) | (((int16_t)r[RAX] < 0) ? 0xffff : 0);\n"); return 1;
    case M(CDQ): emit("r[RDX] = ((int32_t)r[RAX] < 0) ? 0xffffffffu : 0;\n"); return 1;
    case M(CQO): emit("r[RDX] = ((int64_t)r[RAX] < 0) ? ~0ull : 0;\n"); return 1;
    case M(ADC): case M(SBB): {
        const int size = d->size;
        if (!(gpr(d) || d->type == OP_MEM) || !read_op(off, in, src, size, &b)) return 0;
        emit("{ const uint64_t cy = " COND "; ", 2);
        if (d->type == OP_MEM) {
            if (!address(off, in, d, &ad, &stack)) return 0;
            emit("const uint64_t ea = %s; const uint64_t a = %s(ea, %d); ", ad.s, stack ? "rc_lds" : "rc_ld", size);
        } else {
            if (!read_op(off, in, d, size, &a)) return 0;
            emit("const uint64_t a = %s; ", a.s);
        }
        const char *op = m == M(ADC) ? "+" : "-";
        emit("const uint64_t b = %s; const uint64_t v = (a %s b %s cy) & 0x%" PRIx64 "ull; "
             "c->flags = rc_carry_flags(rc_flags(fk, fa, fb, fr, fz, fcf, c->flags), %d, a, b, cy, v, %d); fk = FK_CPU; ",
             b.s, op, op, mask(size), m == M(SBB), size);
        if (d->type == OP_MEM) emit("%s(ea, %d, v);", stack ? "rc_sts" : "rc_st", size);
        else {
            if (!write_op(off, in, d, "v", &w)) return 0;
            emit("%s", w.s);
        }
        emit(" }\n");
        return 1;
    }
    case M(BT): case M(BTS): case M(BTR): case M(BTC): {
        const int size = d->size, bits = size * 8;
        if (size < 2 || !read_op(off, in, src, size, &b)) return 0;
        const char *calc = m == M(BTS) ? "a | bm" : m == M(BTR) ? "a & ~bm" : m == M(BTC) ? "a ^ bm" : "a";
        emit("{ ");
        if (d->type == OP_MEM) { /* register bit offsets (reaching outside the operand): the interpreter */
            if (src->type != OP_IMM || !address(off, in, d, &ad, &stack)) return 0;
            emit("const uint64_t ea = %s; const unsigned bit = (unsigned)(%s) & %d; ", ad.s, b.s, bits - 1);
            emit("const uint64_t a = %s(ea, %d); ", stack ? "rc_lds" : "rc_ld", size);
        } else if (gpr(d)) {
            if (!read_op(off, in, d, size, &a)) return 0;
            emit("const uint64_t a = %s; const unsigned bit = (unsigned)(%s) & %d; ", a.s, b.s, bits - 1);
        } else {
            return 0;
        }
        emit("const uint64_t bm = 1ull << bit; c->flags = (rc_flags(fk, fa, fb, fr, fz, fcf, c->flags) & ~(uint64_t)F_CF) | "
             "((a & bm) ? F_CF : 0); fk = FK_CPU; ");
        if (m != M(BT)) {
            emit("const uint64_t v = %s; ", calc);
            if (d->type == OP_MEM) emit("%s(ea, %d, v);", stack ? "rc_sts" : "rc_st", size);
            else {
                if (!write_op(off, in, d, "v", &w)) return 0;
                emit("%s", w.s);
            }
        }
        emit(" }\n");
        return 1;
    }
    case M(SHL): case M(SHR): case M(SAR): {
        if (in->count < 2 || src->type != OP_REG || src->reg != RCX || src->size != 1) return 0;
        const int size = d->size, bits = size * 8;
        if (!sext[size]) return 0;
        emit("{ const unsigned n = (unsigned)r[RCX] & %d; if (n) { ", size == 8 ? 63 : 31);
        if (d->type == OP_MEM) {
            if (!address(off, in, d, &ad, &stack)) return 0;
            emit("const uint64_t ea = %s; const uint64_t a = %s(ea, %d); ", ad.s, stack ? "rc_lds" : "rc_ld", size);
        } else if (gpr(d)) {
            if (!read_op(off, in, d, size, &a)) return 0;
            emit("const uint64_t a = %s; ", a.s);
        } else {
            return 0;
        }
        if (m == M(SHL)) emit("const uint64_t v = n >= %d ? 0 : (a << n) & 0x%" PRIx64 "ull; ", bits, mask(size));
        else if (m == M(SHR)) emit("const uint64_t v = n >= %d ? 0 : a >> n; ", bits);
        else emit("const uint64_t v = (uint64_t)((int64_t)(%s)a >> (n >= %d ? %d : n)) & 0x%" PRIx64 "ull; ", sext[size], bits,
                  bits - 1, mask(size));
        emit("c->flags = rc_shift_flags(rc_flags(fk, fa, fb, fr, fz, fcf, c->flags), %d, %s, n, v, %d); fk = FK_CPU; ",
             m == M(SHL) ? 0 : m == M(SHR) ? 1 : 2, m == M(SAR) ? str("(uint64_t)(int64_t)(%s)a", sext[size]).s : "a", size);
        if (d->type == OP_MEM) emit("%s(ea, %d, v);", stack ? "rc_sts" : "rc_st", size);
        else {
            if (!write_op(off, in, d, "v", &w)) return 0;
            emit("%s", w.s);
        }
        emit(" } }\n");
        return 1;
    }
    default:
        return 0;
    }
}

/* shl/shr/sar by a constant; imul with two or three operands. */
static int translate_shift_mul(uint64_t off, const BbInsn *in) {
    const int m = in->mnemonic;
    const BbOp *d = &in->op[0];
    const int size = d->size;
    if (size != 4 && size != 8 && size != 2 && size != 1) return 0;
    static const char *const sext[9] = {0, "int8_t", "int16_t", 0, "int32_t", 0, 0, 0, "int64_t"};
    Str a, w;
    if (m == M(SHL) || m == M(SHR) || m == M(SAR)) {
        unsigned count = 1;
        if (in->count > 1) {
            if (in->op[1].type != OP_IMM) return 0; /* by cl */
            count = (unsigned)in->op[1].disp;
        }
        count &= size == 8 ? 63 : 31;
        if (!count) { emit("/* shift by 0 */\n"); return 1; }
        const int bits = size * 8;
        Str da;
        int stack = 0;
        emit("{ ");
        if (d->type == OP_MEM) {
            if (!address(off, in, d, &da, &stack)) return 0;
            emit("const uint64_t ea = %s; const uint64_t a = %s(ea, %d); ", da.s, stack ? "rc_lds" : "rc_ld", size);
        } else if (gpr(d)) {
            if (!read_op(off, in, d, size, &a)) return 0;
            emit("const uint64_t a = %s; ", a.s);
        } else {
            return 0;
        }
        if (m == M(SHL)) emit("const uint64_t v = %s; ", count >= (unsigned)bits ? "0" : str("(a << %u) & 0x%" PRIx64 "ull", count, mask(size)).s);
        else if (m == M(SHR)) emit("const uint64_t v = %s; ", count >= (unsigned)bits ? "0" : str("a >> %u", count).s);
        else emit("const uint64_t v = (uint64_t)((int64_t)(%s)a >> %u) & 0x%" PRIx64 "ull; ", sext[size],
                  count >= (unsigned)bits ? (unsigned)bits - 1 : count, mask(size));
        emit("c->flags = rc_shift_flags(rc_flags(fk, fa, fb, fr, fz, fcf, c->flags), %d, %s, %u, v, %d); fk = FK_CPU; ",
             m == M(SHL) ? 0 : m == M(SHR) ? 1 : 2, m == M(SAR) ? str("(uint64_t)(int64_t)(%s)a", sext[size]).s : "a", count, size);
        if (d->type == OP_MEM) emit("%s(ea, %d, v);", stack ? "rc_sts" : "rc_st", size);
        else {
            if (!write_op(off, in, d, "v", &w)) return 0;
            emit("%s", w.s);
        }
        emit(" }\n");
        return 1;
    }
    if (m == M(IMUL) && in->count >= 2 && gpr(d) && size >= 2) {
        Str x, y;
        if (!read_op(off, in, in->count == 3 ? &in->op[1] : d, size, &x)) return 0;
        if (in->count == 3) {
            if (in->op[2].type != OP_IMM) return 0;
            y = str("(int64_t)%" PRId64 "ll", in->op[2].disp);
        } else {
            Str t;
            if (!read_op(off, in, &in->op[1], size, &t)) return 0;
            y = str("(int64_t)(%s)(%s)", sext[size], t.s);
        }
        if (!write_op(off, in, d, "v", &w)) return 0;
        emit("{ const __int128 full = (__int128)(int64_t)(%s)(%s) * %s; const uint64_t v = (uint64_t)full & 0x%" PRIx64 "ull; "
             "const int ov = (__int128)(int64_t)(%s)v != full; "
             "c->flags = rc_szp((rc_flags(fk, fa, fb, fr, fz, fcf, c->flags) & ~(uint64_t)(F_CF | F_OF)) | (ov ? F_CF | F_OF : 0), v, %d); "
             "fk = FK_CPU; %s }\n", sext[size], x.s, y.s, mask(size), sext[size], size, w.s);
        return 1;
    }
    return 0;
}

/* A read-modify-write destination: `a` its value (zero-extended), `put` the statement storing v. */
static int rmw_dest(uint64_t off, const BbInsn *in, const BbOp *d, Str *load, Str *put) {
    Str ad, w;
    int stack = 0;
    if (d->type == OP_MEM) {
        if (!address(off, in, d, &ad, &stack)) return 0;
        *load = str("const uint64_t ea = %s; const uint64_t a = %s(ea, %d);", ad.s, stack ? "rc_lds" : "rc_ld", d->size);
        *put = str("%s(ea, %d, v);", stack ? "rc_sts" : "rc_st", d->size);
        return 1;
    }
    if (!gpr(d) || !read_op(off, in, d, d->size, &ad) || !write_op(off, in, d, "v", &w)) return 0;
    *load = str("const uint64_t a = %s;", ad.s);
    *put = w;
    return 1;
}

/* Bit scans and counts, BMI1, mul/imul/div/idiv of rdx:rax, rotates, bswap/movbe, xchg, rdtsc (as
 * src/cpu/interp.c does them: flags it leaves alone stay as they were). */
static int translate_bits(uint64_t off, const BbInsn *in) {
    const int m = in->mnemonic;
    const BbOp *d = &in->op[0], *src = &in->op[1];
    const int size = d->size, bits = size * 8;
    static const char *const sext[9] = {0, "int8_t", "int16_t", 0, "int32_t", 0, 0, 0, "int64_t"};
    Str a, b, w, load, put;
    switch (m) {
    case M(BSF): case M(BSR): case M(TZCNT): case M(LZCNT): case M(POPCNT): {
        if (!gpr(d) || (size != 2 && size != 4 && size != 8) || !read_op(off, in, src, size, &a) || !write_op(off, in, d, "v", &w))
            return 0;
        emit("{ const uint64_t a = %s; const uint64_t f = " FLAGS_NOW "; ", a.s);
        if (m == M(BSF) || m == M(BSR))
            emit("if (!a) c->flags = f | F_ZF; else { const uint64_t v = %s; c->flags = f & ~(uint64_t)F_ZF; %s } ",
                 m == M(BSF) ? "(uint64_t)__builtin_ctzll(a)" : "(uint64_t)(63 - __builtin_clzll(a))", w.s);
        else if (m == M(POPCNT))
            emit("const uint64_t v = (uint64_t)__builtin_popcountll(a); c->flags = (f & ~(uint64_t)F_ARITH) | (a ? 0 : F_ZF); %s ", w.s);
        else
            emit("const uint64_t v = !a ? %d : %s; c->flags = (f & ~(uint64_t)(F_CF | F_ZF)) | (!a ? F_CF : 0) | (!v ? F_ZF : 0); %s ",
                 bits, m == M(TZCNT) ? "(uint64_t)__builtin_ctzll(a)" : str("(uint64_t)(__builtin_clzll(a) - %d)", 64 - bits).s, w.s);
        emit("fk = FK_CPU; }\n");
        return 1;
    }
    case M(ANDN): case M(BEXTR): case M(BLSR): case M(BLSI): case M(BLSMSK): {
        if (!gpr(d) || (size != 4 && size != 8) || !read_op(off, in, src, size, &a) || !write_op(off, in, d, "v", &w)) return 0;
        if ((m == M(ANDN) || m == M(BEXTR)) && (in->count < 3 || !read_op(off, in, &in->op[2], size, &b))) return 0;
        const uint64_t mk = mask(size);
        emit("{ uint64_t f = " FLAGS_NOW " & ~(uint64_t)F_ARITH; const uint64_t s = %s; ", a.s);
        if (m == M(ANDN)) emit("const uint64_t v = ~s & (%s) & 0x%" PRIx64 "ull; ", b.s, mk);
        else if (m == M(BEXTR))
            emit("const uint64_t ctl = %s; const unsigned start = ctl & 0xff, len = (ctl >> 8) & 0xff; uint64_t v = start >= %d ? 0 : s >> start; "
                 "if (len < 64) v &= (1ull << len) - 1; v &= 0x%" PRIx64 "ull; ", b.s, bits, mk);
        else if (m == M(BLSR)) emit("const uint64_t v = s & (s - 1) & 0x%" PRIx64 "ull; if (!s) f |= F_CF; ", mk);
        else if (m == M(BLSI)) emit("const uint64_t v = s & (0 - s) & 0x%" PRIx64 "ull; if (s) f |= F_CF; ", mk);
        else emit("const uint64_t v = (s ^ (s - 1)) & 0x%" PRIx64 "ull; if (!s) f |= F_CF; ", mk);
        emit("if (!v) f |= F_ZF; if (v & 0x%" PRIx64 "ull) f |= F_SF; c->flags = f; fk = FK_CPU; %s }\n", mk ^ (mk >> 1), w.s);
        return 1;
    }
    case M(MUL): case M(IMUL): { /* one operand: rdx:rax = rax * it (two and three: translate_shift_mul) */
        if (in->count != 1 || !sext[size] || !read_op(off, in, d, size, &a)) return 0;
        const uint64_t mk = mask(size);
        emit("{ const uint64_t s = %s; const uint64_t a = r[RAX] & 0x%" PRIx64 "ull; uint64_t lo, hi; int ov; ", a.s, mk);
        if (m == M(MUL) && size == 8)
            emit("const unsigned __int128 full = (unsigned __int128)a * s; lo = (uint64_t)full; hi = (uint64_t)(full >> 64); ov = hi != 0; ");
        else if (m == M(MUL))
            emit("const uint64_t full = a * s; lo = full & 0x%" PRIx64 "ull; hi = (full >> %d) & 0x%" PRIx64 "ull; ov = hi != 0; ", mk, bits, mk);
        else if (size == 8)
            emit("const __int128 full = (__int128)(int64_t)a * (int64_t)s; lo = (uint64_t)full; hi = (uint64_t)((unsigned __int128)full >> 64); "
                 "ov = (__int128)(int64_t)lo != full; ");
        else
            emit("const int64_t full = (int64_t)(%s)a * (int64_t)(%s)s; lo = (uint64_t)full & 0x%" PRIx64 "ull; "
                 "hi = ((uint64_t)full >> %d) & 0x%" PRIx64 "ull; ov = (int64_t)(%s)lo != full; ", sext[size], sext[size], mk, bits, mk, sext[size]);
        if (size == 1) emit("r[RAX] = (r[RAX] & ~0xffffull) | lo | (hi << 8); ");
        else if (size == 2) emit("r[RAX] = (r[RAX] & ~0xffffull) | lo; r[RDX] = (r[RDX] & ~0xffffull) | hi; ");
        else emit("r[RAX] = lo; r[RDX] = hi; ");
        emit("c->flags = (" FLAGS_NOW " & ~(uint64_t)(F_CF | F_OF)) | (ov ? F_CF | F_OF : 0); fk = FK_CPU; }\n");
        return 1;
    }
    case M(DIV): case M(IDIV): { /* 32 and 64 bits; a zero divisor or a quotient too wide: the interpreter */
        if ((size != 4 && size != 8) || !read_op(off, in, d, size, &a)) return 0;
        if (m == M(DIV) && size == 4)
            emit("{ const uint64_t s = %s; const uint64_t n = ((r[RDX] & 0xffffffffull) << 32) | (r[RAX] & 0xffffffffull); "
                 "const int ok = s && (n >> 32) < s; uint64_t q = 0, rem = 0; if (ok) { q = n / s; rem = n %% s; } ", a.s);
        else if (m == M(DIV))
            emit("{ const uint64_t s = %s, hi = r[RDX], lo = r[RAX]; const int ok = s && hi < s; uint64_t q = 0, rem = 0; "
                 "if (ok && !hi) { q = lo / s; rem = lo %% s; } else if (ok) { const unsigned __int128 n = ((unsigned __int128)hi << 64) | lo; "
                 "q = (uint64_t)(n / s); rem = (uint64_t)(n %% s); } ", a.s);
        else if (size == 4)
            emit("{ const int64_t s = (int32_t)(%s); const int64_t n = (int64_t)(((r[RDX] & 0xffffffffull) << 32) | (r[RAX] & 0xffffffffull)); "
                 "int ok = s && !(s == -1 && n == INT64_MIN); int64_t q = 0, rem = 0; if (ok) { q = n / s; rem = n %% s; ok = q == (int32_t)q; } ", a.s);
        else
            emit("{ const int64_t s = (int64_t)(%s); const uint64_t hi = r[RDX], lo = r[RAX]; int ok = s != 0; int64_t q = 0, rem = 0; "
                 "if (ok && hi == (uint64_t)((int64_t)lo >> 63)) { if (s == -1 && lo == 0x8000000000000000ull) ok = 0; "
                 "else { q = (int64_t)lo / s; rem = (int64_t)lo %% s; } } else if (ok && s != -1) { "
                 "const __int128 n = (__int128)(((unsigned __int128)hi << 64) | lo), qq = n / s; ok = qq == (int64_t)qq; "
                 "q = (int64_t)qq; rem = (int64_t)(n %% s); } else ok = 0; ", a.s);
        emit("if (ok) { r[RAX] = %s; r[RDX] = %s; } else ", size == 4 ? "(uint32_t)q" : "(uint64_t)q", size == 4 ? "(uint32_t)rem" : "(uint64_t)rem");
        emit_interp(off, in, 1);
        emit(" }\n");
        return 1;
    }
    case M(ROL): case M(ROR): {
        if (!sext[size] || !rmw_dest(off, in, d, &load, &put)) return 0;
        const unsigned cmask = size == 8 ? 63 : 31;
        Str n;
        if (in->count < 2) n = str("1u");
        else if (src->type == OP_IMM) {
            if (!((unsigned)src->disp & cmask)) { emit("/* rotate by 0 */\n"); return 1; }
            n = str("%uu", (unsigned)src->disp & cmask);
        } else if (gpr(src) && src->reg == RCX && src->size == 1) n = str("((unsigned)r[RCX] & %u)", cmask);
        else return 0;
        const uint64_t mk = mask(size), sign = mk ^ (mk >> 1);
        emit("{ const unsigned n = %s; if (n) { %s const unsigned k = n %% %d; ", n.s, load.s, bits);
        if (m == M(ROL))
            emit("const uint64_t v = k ? ((a << k) | (a >> (%d - k))) & 0x%" PRIx64 "ull : a; const int cf = (int)(v & 1); "
                 "c->flags = (" FLAGS_NOW " & ~(uint64_t)(F_CF | F_OF)) | (cf ? F_CF : 0) | ((((v & 0x%" PRIx64 "ull) != 0) != cf) ? F_OF : 0); ",
                 bits, mk, sign);
        else
            emit("const uint64_t v = k ? ((a >> k) | (a << (%d - k))) & 0x%" PRIx64 "ull : a; const int msb = (v & 0x%" PRIx64 "ull) != 0, "
                 "msb2 = (v & 0x%" PRIx64 "ull) != 0; c->flags = (" FLAGS_NOW " & ~(uint64_t)(F_CF | F_OF)) | (msb ? F_CF : 0) | (msb != msb2 ? F_OF : 0); ",
                 bits, mk, sign, sign >> 1);
        emit("fk = FK_CPU; %s } }\n", put.s);
        return 1;
    }
    case M(BSWAP):
        if (!gpr(d) || (size != 4 && size != 8)) return 0;
        emit("r[%d] = %s;\n", d->reg, size == 8 ? str("__builtin_bswap64(r[%d])", d->reg).s : str("__builtin_bswap32((uint32_t)r[%d])", d->reg).s);
        return 1;
    case M(MOVBE): {
        if (size != 2 && size != 4 && size != 8) return 0;
        static const char *const swap[9] = {0, 0, "__builtin_bswap16((uint16_t)", 0, "__builtin_bswap32((uint32_t)", 0, 0, 0, "__builtin_bswap64((uint64_t)"};
        if (!read_op(off, in, src, size, &a) || !write_op(off, in, d, str("%s(%s))", swap[size], a.s).s, &w)) return 0;
        emit("%s\n", w.s);
        return 1;
    }
    case M(XCHG): {
        if (in->count != 2) return 0;
        if (gpr(d) && gpr(src)) {
            Str wa, wb;
            if (!read_op(off, in, d, size, &a) || !read_op(off, in, src, size, &b) || !write_op(off, in, d, "vb", &wa) ||
                !write_op(off, in, src, "va", &wb)) return 0;
            emit("{ const uint64_t va = %s, vb = %s; %s %s }\n", a.s, b.s, wa.s, wb.s);
            return 1;
        }
        const BbOp *mem = d->type == OP_MEM ? d : src, *reg = d->type == OP_MEM ? src : d;
        Str ad;
        int stack;
        static const char *const type[9] = {0, "uint8_t", "uint16_t", 0, "uint32_t", 0, 0, 0, "uint64_t"};
        if (mem->type != OP_MEM || !gpr(reg) || !type[mem->size] || !address(off, in, mem, &ad, &stack) ||
            !read_op(off, in, reg, reg->size, &b) || !write_op(off, in, reg, "old", &w)) return 0;
        emit("{ const uint64_t old = __atomic_exchange_n((%s *)(%s), (%s)(%s), __ATOMIC_SEQ_CST); %s }\n", type[mem->size], ad.s,
             type[mem->size], b.s, w.s);
        return 1;
    }
    case M(RDTSC):
        emit("{ const uint64_t t = rc_tsc(); r[RAX] = (uint32_t)t; r[RDX] = t >> 32; }\n");
        return 1;
    default:
        return 0;
    }
}

/* The instructions of [start, start + size) the code reaches from the entry: fall-through, direct
 * jumps and branches inside the function (data between them, such as jump tables, is never decoded;
 * a place only an indirect jump reaches goes back to the translator). Sorted by offset. */
typedef struct { uint64_t off; BbInsn in; } Insn;
static int insn_order(const void *a, const void *b) {
    const uint64_t x = ((const Insn *)a)->off, y = ((const Insn *)b)->off;
    return x < y ? -1 : x > y;
}
/* Targets of the jump tables decode found (labels too), for the function being generated. */
static uint64_t *table_targets;
/* The function has a jmp through a register that is not a jump table decode recognized (a
 * computed jump, such as into an unrolled loop): every decoded instruction gets a label, so its
 * targets inside the function dispatch there instead of going back to the translator. */
static int computed_jump;
static size_t table_target_count, table_target_capacity;

/* The decoded instruction before `off` on a path to it: the one ending there that falls through,
 * else the single decoded branch that jumps there (`taken`: by its jump). NULL: none or several. */
static const Insn *path_before(const Insn *list, size_t n, uint64_t off, int *taken) {
    const Insn *found = NULL;
    for (size_t i = 0; i < n; ++i)
        if (list[i].off + list[i].in.length == off && falls_through(list[i].off, &list[i].in) &&
            list[i].in.mnemonic != M(JMP)) {
            *taken = 0;
            return &list[i];
        }
    for (size_t i = 0; i < n; ++i) {
        const BbInsn *x = &list[i].in;
        const int cc = cc_of(x->mnemonic);
        if (((cc >= 0 && cc < 16) || x->mnemonic == M(JMP)) && x->op[0].type == OP_IMM &&
            list[i].off + (uint64_t)x->op[0].disp == off) {
            if (found) return NULL;
            found = &list[i];
        }
    }
    *taken = 1;
    return found;
}

/* A jump table's size from the code before its load when the bound check is not next to it: back
 * along the path (through the branch into the table's block), the index followed as `reg & mask`
 * through copies and masks, to `cmp X, N` with ja falling through (or jbe taken) where X is the
 * index or `mov X, reg; and X, mask` of it. 0: not found. */
static uint64_t table_bound(const Insn *list, size_t n, int index) {
    int reg = index, taken = 0;
    uint64_t mask = ~UINT64_C(0);
    uint64_t at = list[n - 3].off;
    for (int steps = 0; steps < 24; ++steps) {
        const Insn *p = path_before(list, n, at, &taken);
        if (!p) return 0;
        const BbInsn *x = &p->in;
        const int m = x->mnemonic;
        if (m == M(JNBE) || m == M(JBE)) {
            if ((m == M(JNBE)) == taken) return 0; /* the path where the index is above the bound */
            const Insn *c = path_before(list, n, p->off, &taken);
            if (!c || taken || c->in.mnemonic != M(CMP) || c->in.op[0].type != OP_REG || c->in.op[1].type != OP_IMM)
                return 0;
            const uint64_t entries = (uint64_t)c->in.op[1].disp + 1;
            const int r = c->in.op[0].reg;
            if (r == reg && mask == ~UINT64_C(0)) return entries;
            /* mov X, reg; and X, mask; cmp X, N: X is the index too */
            const Insn *a = path_before(list, n, c->off, &taken);
            const Insn *v = a && !taken ? path_before(list, n, a->off, &taken) : NULL;
            if (!v || taken || a->in.mnemonic != M(AND) || !gpr(&a->in.op[0]) || a->in.op[0].reg != r ||
                a->in.op[1].type != OP_IMM || (uint64_t)a->in.op[1].disp != mask || v->in.mnemonic != M(MOV) ||
                !gpr(&v->in.op[0]) || v->in.op[0].reg != r || !gpr(&v->in.op[1]) || v->in.op[1].reg != reg)
                return 0;
            return entries <= mask + 1 ? entries : mask + 1;
        }
        if (taken || m == M(CALL) || m == M(RET)) return 0;
        /* What the step back passes: writes of the index's register are copies or masks. */
        if (x->count && gpr(&x->op[0]) && x->op[0].reg == reg) {
            if (m == M(AND) && x->op[1].type == OP_IMM && x->op[1].disp > 0 && x->op[1].disp < 4096 &&
                !((x->op[1].disp + 1) & x->op[1].disp))
                mask &= (uint64_t)x->op[1].disp;
            else if (m == M(MOV) && gpr(&x->op[1]) && x->op[0].size >= 4) reg = x->op[1].reg;
            else return 0;
        }
        at = p->off;
    }
    return 0;
}

/* A compiler's jump table ending at list[n - 1] (jmp reg): lea T, [rip + table]; movsxd C,
 * dword [T + 4 * I]; add C, T; jmp C, its size from cmp I, N; ja (N + 1 entries; or jbe to the
 * table, the default case falling through). Its targets
 * are pushed to `work` and kept in table_targets; 0 if it is not one. */
static int jump_table(Function fn, const Insn *list, size_t n, uint64_t *work, size_t *pending, const uint8_t *seen) {
    if (n < 4) return 0;
    const BbInsn *jmp = &list[n - 1].in, *add = &list[n - 2].in, *load = &list[n - 3].in;
    if (jmp->op[0].type != OP_REG || jmp->op[0].kind != RK_GPR || jmp->op[0].size != 8) return 0;
    const int c = jmp->op[0].reg;
    if (add->mnemonic != M(ADD) || add->op[0].type != OP_REG || add->op[0].reg != c || add->op[1].type != OP_REG ||
        add->op[0].size != 8) return 0;
    const int t = add->op[1].reg;
    if (load->mnemonic != M(MOVSXD) || load->op[0].type != OP_REG || load->op[0].reg != c || load->op[1].type != OP_MEM ||
        load->op[1].base != t || load->op[1].scale != 4 || load->op[1].index == 0xff || load->op[1].disp != 0) return 0;
    const int index = load->op[1].index;
    uint64_t table = 0, entries = 0;
    for (size_t k = n - 3; k-- > 0 && k + 32 > n;) { /* the nearest; the default case can sit between */
        const Insn *x = &list[k];
        if (!table && x->in.mnemonic == M(LEA) && x->in.op[0].type == OP_REG && x->in.op[0].reg == t &&
            x->in.op[1].base == 0xfe && x->in.op[1].index == 0xff)
            table = x->off + x->in.length + (uint64_t)x->in.op[1].disp;
        if (!entries && x->in.mnemonic == M(CMP) && x->in.op[0].type == OP_REG && x->in.op[0].reg == index &&
            x->in.op[1].type == OP_IMM && k + 1 < n &&
            (list[k + 1].in.mnemonic == M(JNBE) || list[k + 1].in.mnemonic == M(JBE)))
            entries = (uint64_t)x->in.op[1].disp + 1;
    }
    /* Also: cmp R, N, then flag-free moves (the index copied from R: mov I, R), then ja
     * (clang interleaves the stores of the case's common code). */
    for (size_t k = n - 3; !entries && k-- > 0 && k + 32 > n;) {
        if (list[k].in.mnemonic != M(JNBE) && list[k].in.mnemonic != M(JBE)) continue;
        int from = -1; /* the register the bound applies to */
        for (size_t j = k; j-- > 0 && j + 8 > k;) {
            const BbInsn *x = &list[j].in;
            if (x->mnemonic == M(CMP)) {
                if (x->op[0].type == OP_REG && x->op[1].type == OP_IMM) from = x->op[0].reg, entries = (uint64_t)x->op[1].disp + 1;
                break;
            }
            if (x->mnemonic != M(MOV) && x->mnemonic != M(LEA) && x->mnemonic != M(MOVZX) && x->mnemonic != M(MOVSXD)) break;
        }
        if (!entries) break;
        /* The index is R, or a copy of R made after the cmp (before the table load). */
        int ok = from == index;
        for (size_t j = k + 1; !ok && j < n - 3; ++j) {
            const BbInsn *x = &list[j].in;
            if (x->mnemonic == M(MOV) && x->op[0].type == OP_REG && x->op[0].reg == index && x->op[1].type == OP_REG &&
                x->op[1].reg == from) ok = 1;
        }
        for (size_t j = k; !ok && j-- > 0 && j + 8 > k;) { /* or just before it: mov I, R; cmp R, N */
            const BbInsn *x = &list[j].in;
            if (x->mnemonic == M(CMP)) continue;
            if (x->mnemonic == M(MOV) && x->op[0].type == OP_REG && x->op[0].reg == index && x->op[1].type == OP_REG &&
                x->op[1].reg == from) ok = 1;
        }
        if (!ok) entries = 0;
        break;
    }
    if (table && !entries) entries = table_bound(list, n, index);
    if (!table || !entries || entries > 4096) return 0;
    for (uint64_t e = 0; e < entries; ++e) {
        int32_t rel;
        memcpy(&rel, (const void *)(uintptr_t)(base + table + 4 * e), 4);
        const uint64_t target = table + (uint64_t)(int64_t)rel;
        if (target < fn.offset || target >= fn.offset + fn.size) return 0; /* not what it looked like */
        if (table_target_count == table_target_capacity) {
            table_target_capacity = table_target_capacity ? table_target_capacity * 2 : 256;
            table_targets = realloc(table_targets, table_target_capacity * sizeof(*table_targets));
        }
        table_targets[table_target_count++] = target;
        if (!seen[target - fn.offset]) work[(*pending)++] = target;
    }
    return 1;
}

static Insn *decode(Function fn, size_t *count) {
    Insn *list = NULL;
    size_t n = 0, capacity = 0;
    uint8_t *seen = calloc(fn.size + 16, 1);
    size_t work_capacity = fn.size + 16 + 4096;
    uint64_t *work = malloc(work_capacity * sizeof(*work));
    size_t pending = 0;
    table_target_count = 0;
    computed_jump = 0;
    work[pending++] = fn.offset;
    while (pending) {
        uint64_t off = work[--pending];
        while (off >= fn.offset && off < fn.offset + fn.size && !seen[off - fn.offset]) {
            const BbBlock *block = bbcpu_block(base + off);
            if (!block->count) break;
            int ended = 0;
            for (uint32_t i = 0; i < block->count && !ended; ++i) {
                if (off >= fn.offset + fn.size || seen[off - fn.offset]) { ended = 1; break; }
                const BbInsn *in = &block->insn[i];
                seen[off - fn.offset] = 1;
                if (n == capacity) {
                    capacity = capacity ? capacity * 2 : 256;
                    list = realloc(list, capacity * sizeof(*list));
                }
                list[n++] = (Insn){off, *in};
                const int m = in->mnemonic, cc = cc_of(m);
                if (((cc >= 0 && cc < 16) || m == M(JMP) || m == M(JRCXZ) || m == M(JECXZ) || m == M(LOOP) ||
                     m == M(LOOPE) || m == M(LOOPNE)) && in->op[0].type == OP_IMM) {
                    const uint64_t t = off + (uint64_t)in->op[0].disp;
                    if (t >= fn.offset && t < fn.offset + fn.size && !seen[t - fn.offset]) work[pending++] = t;
                }
                if (m == M(JMP) && in->op[0].type == OP_REG &&
                    !(pending + 4096 < work_capacity && jump_table(fn, list, n, work, &pending, seen)))
                    computed_jump = 1;
                /* No fall-through after these. */
                if (!falls_through(off, in)) ended = 2;
                off += in->length;
            }
            if (ended == 2) break;
        }
    }
    free(work);
    free(seen);
    qsort(list, n, sizeof(*list), insn_order);
    if (getenv("BB_RECOMP_OVERLAPS"))
        for (size_t i = 0; i + 1 < n; ++i)
            if (list[i].off + list[i].in.length > list[i + 1].off) {
                fprintf(stderr, "overlap %#llx at %#llx\n", (unsigned long long)fn.offset, (unsigned long long)list[i + 1].off);
                break;
            }
    *count = n;
    return list;
}

/* Snippet mode (fuzzing, tests/fuzz_recomp.c): one instruction, then a return at its end. */
static int snippet_mode;
/* BB_RECOMP_TRACE=1: a trace point before each instruction (replay --lockstep). */
static int trace_points;
static uint64_t snippet_line;

static int by_offset(const void *a, const void *b) {
    const uint64_t x = *(const uint64_t *)a, y = *(const uint64_t *)b;
    return x < y ? -1 : x > y;
}

/* ---- register sync ---- */

/* generate writes a function's instructions with the SPILL/RELOAD markers, then fills them in from
 * the text, with two analyses over the gotos between the instructions (registers as masks: r[n]
 * bits 0-15, xmm[n] bits 16-31):
 * - dirty (forward): the registers whose local may differ from the BbCpu's, written since the
 *   last RELOAD. A SPILL stores those.
 * - live (backward): the registers read before they are written again; a SPILL reads its dirty
 *   ones. A RELOAD loads those, and the entry; the others keep their value in the BbCpu only,
 *   until an instruction overwrites them whole (mov, movzx, movsx, lea, pop into a 32- or 64-bit
 *   register, its only mention). */
enum { EV_USE, EV_WRITE, EV_KILL, EV_SPILL, EV_RELOAD, EV_RELOAD_IF, EV_GOTO, EV_DISPATCH };
typedef struct { int kind; uint32_t mask; uint64_t target; } SyncEvent;
static SyncEvent *events;
static size_t event_count, event_capacity;
static const uint32_t *marker_masks; /* for the markers in order, while writing the text */
static size_t marker_next;

static int ident_char(char ch) { return (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') || (ch >= '0' && ch <= '9') || ch == '_'; }

static void event(int kind, uint32_t mask, uint64_t target) {
    if (event_count == event_capacity) {
        event_capacity = event_capacity ? 2 * event_capacity : 4096;
        events = realloc(events, event_capacity * sizeof(*events));
    }
    events[event_count++] = (SyncEvent){kind, mask, target};
}

static void sync_copy(FILE *o, uint32_t mask, int load) {
    for (int k = 0; k < 16; ++k)
        if (mask >> k & 1) fprintf(o, load ? " r[%d] = c->r[%d];" : " c->r[%d] = r[%d];", k, k);
    for (int k = 0; k < 16; ++k)
        if (mask >> (16 + k) & 1) fprintf(o, load ? " xmm[%d] = c->v[%d];" : " c->v[%d] = xmm[%d];", k, k);
}

/* A register index as generated code writes it (r[3], r[RSP]) at s, or -1. */
static int sync_index(const char *s, size_t n, size_t *end) {
    static const char *const names[16] = {"RAX", "RCX", "RDX", "RBX", "RSP", "RBP", "RSI", "RDI",
                                          "R8", "R9", "R10", "R11", "R12", "R13", "R14", "R15"};
    size_t j = 0;
    while (j < n && ident_char(s[j])) ++j;
    if (!j || j >= n || s[j] != ']') return -1;
    *end = j + 1;
    if (s[0] >= '0' && s[0] <= '9') {
        const long v = strtol(s, NULL, 10);
        return v >= 0 && v < 16 ? (int)v : -1;
    }
    for (int k = 0; k < 16; ++k)
        if (strlen(names[k]) == j && !memcmp(s, names[k], j)) return k;
    return -1;
}

/* Goes through the generated text of one instruction. Without `o`, notes its events in order: a
 * register's mention is a use, and a write too where an assignment follows it (after any .member
 * or [index]), its address is taken or a member array of it is (xmm[n].b, as memcpy's
 * destination); a bare r or xmm is all of them. `kill` (or -1): the register the instruction
 * overwrites whole, if this is its only mention. With `o`, writes the text there with the
 * markers filled in from marker_masks. */
static void sync_scan(const char *s, size_t n, int kill, FILE *o) {
    size_t i = 0, run = 0;
    const size_t first = event_count;
    int kill_mentions = 0;
    while (i < n) {
        if (s[i] == '/' && i + 1 < n && s[i + 1] == '*') { /* comments name instructions and imports */
            i += 2;
            while (i + 1 < n && !(s[i] == '*' && s[i + 1] == '/')) ++i;
            i += 2;
            continue;
        }
        if (s[i] == '@' && i + 1 < n) {
            const int kind = s[i + 1] == 'S' ? EV_SPILL : s[i + 1] == 'R' ? EV_RELOAD : s[i + 1] == 'r' ? EV_RELOAD_IF : -1;
            if (kind < 0) {
                fprintf(stderr, "bbrecomp: unknown sync marker @%c\n", s[i + 1]);
                exit(1);
            }
            if (o) {
                fwrite(s + run, 1, i - run, o);
                sync_copy(o, marker_masks[marker_next++], kind != EV_SPILL);
            } else {
                event(kind, 0, 0);
            }
            i += 2;
            run = i;
            continue;
        }
        if (!ident_char(s[i]) || (i && ident_char(s[i - 1]))) { ++i; continue; }
        size_t j = i;
        while (j < n && ident_char(s[j])) ++j;
        const size_t len = j - i;
        const int vec = len == 3 && !memcmp(s + i, "xmm", 3);
        if (o) {
        } else if (len == 4 && !memcmp(s + i, "goto", 4)) {
            size_t k = j;
            while (k < n && s[k] == ' ') ++k;
            if (k + 10 <= n && !memcmp(s + k, "L_dispatch", 10)) event(EV_DISPATCH, 0, 0);
            else if (k + 2 < n && !memcmp(s + k, "L_", 2)) event(EV_GOTO, 0, strtoull(s + k + 2, NULL, 16));
            else {
                fprintf(stderr, "bbrecomp: a goto the register sync does not know: %.40s\n", s + i);
                exit(1);
            }
        } else if (len == 7 && !memcmp(s + i, "RC_VOUT", 7)) { /* trace points */
            event(EV_USE, 0xffff0000u, 0);
        } else if ((vec || (len == 1 && s[i] == 'r')) && !(i && (s[i - 1] == '.' || s[i - 1] == '>'))) {
            if (j < n && s[j] == '[') {
                size_t end;
                const int k = sync_index(s + j + 1, n - j - 1, &end);
                if (k < 0) {
                    fprintf(stderr, "bbrecomp: a register the register sync does not know: %.40s\n", s + i);
                    exit(1);
                }
                const uint32_t bit = 1u << (k + (vec ? 16 : 0));
                int write = i && s[i - 1] == '&', assign = 0;
                if (i >= 2 && ((s[i - 1] == '+' && s[i - 2] == '+') || (s[i - 1] == '-' && s[i - 2] == '-'))) write = 1;
                size_t p = j + 1 + end;
                int array = 0;
                for (;;) { /* .member, [index] */
                    if (p < n && s[p] == '.') {
                        ++p;
                        while (p < n && ident_char(s[p])) ++p;
                        array = 1;
                    } else if (p < n && s[p] == '[') {
                        int depth = 0;
                        do depth += s[p] == '[' ? 1 : s[p] == ']' ? -1 : 0;
                        while (++p < n && depth);
                        array = 0;
                    } else {
                        break;
                    }
                }
                if (array) write = 1; /* xmm[n].b as a pointer: memcpy's destination */
                while (p < n && s[p] == ' ') ++p;
                if (p + 1 < n) {
                    const char a = s[p], b = s[p + 1];
                    if (a == '=' && b != '=') { write = 1; assign = p == j + 1 + end + 1; }
                    if (b == '=' && strchr("+-*/%&|^", a)) write = 1;
                    if ((a == '+' && b == '+') || (a == '-' && b == '-')) write = 1;
                    if (p + 2 < n && ((a == '<' && b == '<') || (a == '>' && b == '>')) && s[p + 2] == '=') write = 1;
                }
                if (!vec && k == kill) {
                    ++kill_mentions;
                    if (!assign || !write) kill = -1;
                }
                event(!vec && k == kill ? EV_KILL : EV_USE, bit, 0);
                if (write) event(EV_WRITE, bit, 0);
            } else {
                const uint32_t all = vec ? 0xffff0000u : 0xffffu;
                event(EV_USE, all, 0);
                event(EV_WRITE, all, 0);
            }
        }
        i = j;
    }
    if (o) {
        fwrite(s + run, 1, n - run, o);
        return;
    }
    /* A kill only as the instruction's one mention of it, in straight code. */
    for (size_t e = first; e < event_count; ++e)
        if (events[e].kind == EV_KILL && (kill_mentions != 1 || kill < 0 || memmem(s, n, "if", 2) || memchr(s, '?', n) ||
                                          memchr(s, '@', n) || memmem(s, n, "goto", 4)))
            events[e].kind = EV_USE;
}

/* The register an instruction overwrites whole (its 32 or 64 bits), or -1. */
static int sync_kill(const BbInsn *in) {
    const int m = in->mnemonic;
    const BbOp *d = &in->op[0];
    if (!in->count || d->type != OP_REG || d->kind != RK_GPR || d->reg > 15 || (d->size != 4 && d->size != 8)) return -1;
    if (m == M(MOV) || m == M(MOVZX) || m == M(MOVSX) || m == M(MOVSXD) || m == M(LEA)) return d->reg;
    return m == M(POP) && d->size == 8 ? d->reg : -1;
}

/* Where instruction i goes on to: 0 the next one in address order (or past the function's end), 1
 * another decoded instruction (a label), 2 an address not decoded (the dispatcher), -1 nowhere. */
static int falls_to(const Insn *list, size_t count, size_t i, Range f) {
    if (!falls_through(list[i].off, &list[i].in)) return -1;
    const uint64_t next = list[i].off + list[i].in.length;
    if (i + 1 < count ? list[i + 1].off == next : next == f.end) return 0;
    return bsearch(&next, list, count, sizeof(*list), by_offset) ? 1 : 2;
}

static void generate(Function fn, uint64_t *stats) {
    size_t count;
    Insn *list = decode(fn, &count);
    const Range f = {fn.offset, fn.offset + fn.size};
    /* Labels only where a direct jump goes (and the entry), or everywhere in a function with a
     * computed jump: every other address an indirect jump or an interpreted branch reaches goes
     * back to the translator (L_dispatch). */
    uint64_t *targets = malloc((2 * count + 1 + table_target_count) * sizeof(*targets));
    size_t target_count = 0;
    targets[target_count++] = f.start;
    /* Up to 1000 instructions: a label everywhere in a larger function made clang's -O2 run for
     * over ten minutes on one file. */
    if (computed_jump && count <= 1000) {
        for (size_t i = 0; i < count; ++i) targets[target_count++] = list[i].off;
    }
    for (size_t i = 0; i < table_target_count; ++i) targets[target_count++] = table_targets[i]; /* jump tables */
    /* Instructions decoded from different starting points can overlap (data decoded as code after
     * a jump table, a branch into the middle of an instruction): the instruction after one in
     * address order is then not where it continues, so it gets an explicit jump (falls_to). */
    for (size_t i = 0; i < count; ++i)
        if (falls_to(list, count, i, f) == 1) targets[target_count++] = list[i].off + list[i].in.length;
    for (size_t i = 0; i < count; ++i) {
        const BbInsn *in = &list[i].in;
        const int cc = cc_of(in->mnemonic);
        if (((cc >= 0 && cc < 16) || in->mnemonic == M(JMP)) && in->op[0].type == OP_IMM) {
            const uint64_t t = list[i].off + (uint64_t)in->op[0].disp;
            if (t >= f.start && t < f.end) targets[target_count++] = t;
        }
    }
    qsort(targets, target_count, sizeof(*targets), by_offset);
    emit("\n/* 0x%" PRIx64 ", %" PRIu64 " bytes, %zu instructions%s%s */\n", fn.offset, fn.size, count,
         annotation(fn.offset) ? ": " : "", annotation(fn.offset) ? annotation(fn.offset) : "");
    /* Vector registers in locals: the ones the instructions name (non-VEX blendv reads xmm0). */
    vused = 0;
    for (size_t i = 0; i < count; ++i) {
        const BbInsn *in = &list[i].in;
        for (int k = 0; k < 5; ++k)
            if (in->op[k].type == OP_REG && in->op[k].kind == RK_VEC && in->op[k].reg < 16) vused |= 1u << in->op[k].reg;
        const int m = in->mnemonic;
        if (!in->vex && (m == M(BLENDVPS) || m == M(BLENDVPD) || m == M(PBLENDVB))) vused |= 1;
    }
    if (trace_points) { /* the trace reads the locals: all of them are kept */
        emit("#define RC_VOUT");
        for (int k = 0; k < 16; ++k) if (vused >> k & 1) emit(" c->v[%d] = xmm[%d];", k, k);
        emit("\n");
    }
    if (snippet_mode) emit("static void s_%" PRIx64 "(BbCpu *c) {\n", snippet_line);
    else emit("__attribute__((visibility(\"hidden\"))) void f_%" PRIx64 "(BbCpu *c) {\n", fn.offset);
    /* The instructions, each from starts[i]; then the copies filled in (register sync). */
    const long body = ftell(out);
    long *starts = malloc((count + 1) * sizeof(*starts));
    for (size_t i = 0; i < count; ++i) {
        const Insn *x = &list[i];
        starts[i] = ftell(out) - body;
        if (bsearch(&x->off, targets, target_count, sizeof(*targets), by_offset)) emit("L_%" PRIx64 ": ", x->off);
        if (trace_points) emit("RC_VOUT rc->trace(0x%" PRIx64 ", r, c); ", x->off);
        ++stats[2];
        const long mark = ftell(out);
        if (translate(f, x->off, &x->in, stats)) {
            ++stats[0];
        } else {
            fseek(out, mark, SEEK_SET); /* what a translation wrote before it gave up */
            emit_interp(x->off, &x->in, 0);
            emit("\n");
        }
        const uint64_t next = x->off + x->in.length;
        const int to = falls_to(list, count, i, f);
        if (to == 1) emit("goto L_%" PRIx64 "; /* not the next one in address order */\n", next);
        else if (to == 2) emit("{ dn = B + 0x%" PRIx64 "ull; goto L_dispatch; }\n", next);
    }
    starts[count] = ftell(out) - body;
    char *text = malloc((size_t)starts[count] + 1);
    fseek(out, body, SEEK_SET);
    if (fread(text, 1, (size_t)starts[count], out) != (size_t)starts[count]) { perror("bbrecomp: reading back"); exit(1); }
    fseek(out, body, SEEK_SET);
    /* The events of each instruction, from evs[i]. */
    size_t *evs = malloc((count + 1) * sizeof(*evs));
    event_count = 0;
    for (size_t i = 0; i < count; ++i) {
        evs[i] = event_count;
        sync_scan(text + starts[i], (size_t)(starts[i + 1] - starts[i]), sync_kill(&list[i].in), NULL);
    }
    evs[count] = event_count;
    if (trace_points) event(EV_USE, 0xffffu, 0); /* the entry: the trace reads all of r */
    for (size_t e = 0; e < evs[count]; ++e) {
        if (events[e].kind != EV_GOTO) continue;
        const Insn *t = bsearch(&events[e].target, list, count, sizeof(*list), by_offset);
        if (!t) { fprintf(stderr, "bbrecomp: goto L_%" PRIx64 " without its instruction\n", events[e].target); exit(1); }
        events[e].target = (uint64_t)(t - list); /* the instruction it goes to */
    }
    unsigned char *is_case = malloc(count); /* L_dispatch's cases */
    for (size_t i = 0; i < count; ++i) is_case[i] = bsearch(&list[i].off, targets, target_count, sizeof(*targets), by_offset) != NULL;
    /* Dirty sets at each instruction's start (the entry clean), to a fixed point; L_dispatch takes
     * its set to every case and to the exit. The set at each SPILL goes in mask[]. */
    uint32_t *mask = calloc(evs[count] + 1, sizeof(*mask));
    uint32_t *dirty_in = calloc(count + 1, sizeof(*dirty_in)), dispatch = 0;
    for (int changed = 1; changed;) {
        changed = 0;
        for (size_t i = 0; i < count; ++i) {
            uint32_t dirty = dirty_in[i];
            for (size_t e = evs[i]; e < evs[i + 1]; ++e) {
                const SyncEvent *v = &events[e];
                if (v->kind == EV_WRITE) dirty |= v->mask;
                else if (v->kind == EV_SPILL) mask[e] = dirty;
                else if (v->kind == EV_RELOAD) dirty = 0;
                else if (v->kind == EV_DISPATCH) dispatch |= dirty;
                else if (v->kind == EV_GOTO && (dirty_in[v->target] | dirty) != dirty_in[v->target]) {
                    dirty_in[v->target] |= dirty;
                    changed = 1;
                }
            }
            if ((dirty_in[i + 1] | dirty) != dirty_in[i + 1]) { dirty_in[i + 1] |= dirty; changed = 1; }
        }
        for (size_t i = 0; i < count; ++i)
            if (is_case[i] && (dirty_in[i] | dispatch) != dirty_in[i]) { dirty_in[i] |= dispatch; changed = 1; }
    }
    const uint32_t dirty_out = dispatch | dirty_in[count]; /* L_dispatch's default, past the end */
    const uint32_t end_out = snippet_mode ? dirty_in[count] : dirty_out;
    /* Live sets at each instruction's start, to a fixed point; the exits read their dirty
     * registers and RSP. The set after each RELOAD goes in mask[]. */
    uint32_t *live_in = calloc(count + 1, sizeof(*live_in)), live_dispatch = 0;
    live_in[count] = end_out | 1u << RSP;
    for (int changed = 1; changed;) {
        changed = 0;
        uint32_t cases = dirty_out | 1u << RSP;
        for (size_t i = 0; i < count; ++i) cases |= is_case[i] ? live_in[i] : 0;
        if (cases != live_dispatch) { live_dispatch = cases; changed = 1; }
        for (size_t i = count; i-- > 0;) {
            uint32_t live = live_in[i + 1];
            for (size_t e = evs[i + 1]; e-- > evs[i];) {
                const SyncEvent *v = &events[e];
                if (v->kind == EV_USE) live |= v->mask;
                else if (v->kind == EV_KILL) live &= ~v->mask;
                else if (v->kind == EV_SPILL) live |= mask[e];
                else if (v->kind == EV_RELOAD) { mask[e] = live; live = 0; }
                else if (v->kind == EV_RELOAD_IF) mask[e] = live;
                else if (v->kind == EV_GOTO) live |= live_in[v->target];
                else if (v->kind == EV_DISPATCH) live |= live_dispatch;
            }
            if ((live_in[i] | live) != live_in[i]) { live_in[i] |= live; changed = 1; }
        }
    }
    uint32_t entry = live_in[0] | 1u << RSP; /* entry_rsp */
    for (size_t e = evs[count]; e < event_count; ++e) entry |= events[e].mask;
    emit("    const uint64_t B = rc->image_base;\n");
    emit("    uint64_t r[16];\n    BbVec xmm[16];\n   ");
    sync_copy(out, entry, 1);
    emit("\n    int fk = FK_CPU, fz = 8, fcf = 0;\n    uint64_t fa = 0, fb = 0, fr = 0, dn = 0;\n");
    emit("    const uint64_t entry_rsp = r[RSP];\n");
    emit("    (void)fa; (void)fb; (void)fr; (void)fz; (void)fcf; (void)entry_rsp; (void)dn;\n");
    /* The text with the copies: the masks of the markers in order. */
    size_t markers = 0;
    for (size_t e = 0; e < evs[count]; ++e)
        if (events[e].kind == EV_SPILL || events[e].kind == EV_RELOAD || events[e].kind == EV_RELOAD_IF) mask[markers++] = mask[e];
    marker_masks = mask;
    marker_next = 0;
    for (size_t i = 0; i < count; ++i)
        sync_scan(text + starts[i], (size_t)(starts[i + 1] - starts[i]), -1, out);
    if (snippet_mode) {
        emit("   ");
        sync_copy(out, end_out, 0);
        emit(" " FLAGS_OUT " c->rip = B + 0x%" PRIx64 "ull; return;\n", f.end);
    }
    emit("    dn = B + 0x%" PRIx64 "ull; /* past the end */\n", f.end);
    emit("L_dispatch:\n    switch (dn - B) {\n");
    for (size_t i = 0; i < target_count; ++i)
        if (!i || targets[i] != targets[i - 1]) emit("    case 0x%" PRIx64 ": goto L_%" PRIx64 ";\n", targets[i], targets[i]);
    emit("    default: break;\n    }\n   ");
    sync_copy(out, dirty_out, 0);
    emit(" " FLAGS_OUT "\n    c->rip = dn;\n");
    emit("    if ((dn - B >= 0x%" PRIx64 "ull && dn - B < 0x%" PRIx64 "ull) || r[RSP] != entry_rsp) rc->bail(c, entry_rsp); /* the translator */\n", f.start, f.end);
    emit("    else rc->tail(c); /* out of the function, the stack as at its entry: a tail call */\n}\n");
    if (trace_points) emit("#undef RC_VOUT\n");
    free(live_in);
    free(dirty_in);
    free(mask);
    free(is_case);
    free(evs);
    free(text);
    free(starts);
    free(targets);
    free(list);
}

/* --snippets ENCODINGS OUT.c: each instruction (hex bytes per line) at CODE_AT, alone; the ones
 * translated natively get a function s_<line number>, in a table (offset: the line number). */
#define SNIPPET_AT UINT64_C(0x30000000000)
static int snippets(const char *encodings, const char *path) {
    uint8_t *code = mmap((void *)SNIPPET_AT, 0x4000, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON | MAP_FIXED, -1, 0);
    if (code == MAP_FAILED) { perror("mmap"); return 1; }
    base = SNIPPET_AT;
    bbcpu_add_guest_code(SNIPPET_AT, 0x4000);
    static BbCpu warm;
    bbcpu_run(&warm, 0);
    snippet_mode = 1;
    FILE *in = fopen(encodings, "r");
    out = fopen(path, "w+"); /* generate reads each function back */
    if (!in || !out) { perror(encodings); return 1; }
    emit("/* Written by tools/recomp/bbrecomp.c --snippets (tests/fuzz_recomp.c). */\n");
    emit("#include \"recomp/rc.h\"\n\nstatic const RcApi *rc;\n");
    char line[128];
    uint64_t stats[3] = {0, 0, 0}, number = 0;
    uint64_t *lines = NULL;
    size_t count = 0, capacity = 0;
    while (fgets(line, sizeof(line), in)) {
        ++number;
        uint8_t bytes[16];
        int len = 0;
        for (const char *p = line; p[0] && p[1] && p[0] != '\n' && len < 15; p += 2) {
            unsigned v;
            if (sscanf(p, "%2x", &v) != 1) break;
            bytes[len++] = (uint8_t)v;
        }
        if (!len) continue;
        memset(code, 0x90, 64);
        memcpy(code, bytes, (size_t)len);
        bbcpu_invalidate(SNIPPET_AT, 64);
        const BbBlock *block = bbcpu_block(SNIPPET_AT);
        if (!block->count || block->insn[0].length != len || block->insn[0].branch) continue;
        /* Natively translated only: a fallback is the interpreter itself. */
        const long mark = ftell(out);
        const uint64_t before = stats[0];
        snippet_line = number;
        generate((Function){0, (uint64_t)len}, stats);
        if (stats[0] == before) { fseek(out, mark, SEEK_SET); continue; }
        if (count == capacity) { capacity = capacity ? capacity * 2 : 4096; lines = realloc(lines, capacity * 8); }
        lines[count++] = number;
    }
    emit("\nstatic const RcFunction table[] = {\n");
    for (size_t i = 0; i < count; ++i) emit("    {%" PRIu64 ", 0, 0, s_%" PRIx64 "},\n", lines[i], lines[i]);
    emit("};\n\nconst RcFunction *bb_recomp_init(const RcApi *api, size_t *count) {\n");
    emit("    rc = api;\n    *count = sizeof(table) / sizeof(table[0]);\n    return table;\n}\n");
    close_output();
    printf("%zu instructions translated natively (of %" PRIu64 " lines)\n", count, number);
    return 0;
}

int main(int argc, char **argv) {
    if (argc == 4 && !strcmp(argv[1], "--snippets")) return snippets(argv[2], argv[3]);
    if (argc < 5) {
        fprintf(stderr, "usage: %s ELF FUNCTIONS_TSV OUT.c OFFSET...\n", argv[0]);
        return 2;
    }
    map_image(argv[1]);
    static BbCpu warm;
    bbcpu_run(&warm, 0); /* the interpreter's tables */
    trace_points = getenv("BB_RECOMP_TRACE") && getenv("BB_RECOMP_TRACE")[0] == '1';
    if (getenv("BB_RECOMP_ANNOTATIONS") && *getenv("BB_RECOMP_ANNOTATIONS")) load_annotations(getenv("BB_RECOMP_ANNOTATIONS"));
    /* OFFSET... or @LIST (one offset a line). */
    size_t capacity = (size_t)argc;
    functions = calloc(capacity, sizeof(*functions));
    for (int i = 4; i < argc; ++i) {
        FILE *list = argv[i][0] == '@' ? fopen(argv[i] + 1, "r") : NULL;
        if (argv[i][0] == '@' && !list) { perror(argv[i] + 1); return 1; }
        char line[64];
        while (list ? fgets(line, sizeof(line), list) != NULL : (snprintf(line, sizeof(line), "%s", argv[i]), 1)) {
            const uint64_t offset = strtoull(line, NULL, 0);
            if (offset || !list) {
                const uint64_t size = size_of(argv[2], offset);
                if (!size) { fprintf(stderr, "%#" PRIx64 ": not in %s\n", offset, argv[2]); return 1; }
                if (function_count == capacity) { capacity *= 2; functions = realloc(functions, capacity * sizeof(*functions)); }
                functions[function_count++] = (Function){offset, size};
            }
            if (!list) break;
        }
        if (list) fclose(list);
    }
    /* Functions that call setjmp stay with the translator: the setjmp's return address would be
     * the runtime's (rc->call), and a longjmp cannot come back into a C function that went on.
     * From calls.tsv and imports.tsv next to FUNCTIONS_TSV (tools/recomp/scan.c, imports.py). */
    {
        char dir[1024];
        snprintf(dir, sizeof(dir), "%s", argv[2]);
        char *slash = strrchr(dir, '/');
        if (slash) slash[1] = 0; else dir[0] = 0;
        char path[1100], line[512];
        snprintf(path, sizeof(path), "%simports.tsv", dir);
        uint64_t jumps[8];
        int jump_count = 0;
        FILE *f = fopen(path, "r");
        while (f && fgets(line, sizeof(line), f)) {
            char name[256];
            unsigned long long stub;
            if (sscanf(line, "%llx\t%255s", &stub, name) == 2) {
                imports = realloc(imports, (import_count + 1) * sizeof(*imports));
                imports[import_count].stub = stub;
                snprintf(imports[import_count].name, sizeof(imports[import_count].name), "%s", name);
                ++import_count; /* imports.tsv is sorted */
            }
            if (sscanf(line, "%llx\t%255s", &stub, name) == 2 &&
                (!strcmp(name, "setjmp") || !strcmp(name, "_setjmp") || !strcmp(name, "sigsetjmp")) && jump_count < 8)
                jumps[jump_count++] = stub;
        }
        if (f) fclose(f);
        else fprintf(stderr, "%s: none (functions that call setjmp are not known)\n", path);
        snprintf(path, sizeof(path), "%scalls.tsv", dir);
        f = jump_count ? fopen(path, "r") : NULL;
        size_t skipped = 0;
        while (f && fgets(line, sizeof(line), f)) {
            unsigned long long caller, callee;
            if (sscanf(line, "%llx\t%llx", &caller, &callee) != 2) continue;
            for (int j = 0; j < jump_count; ++j) {
                if (callee != jumps[j]) continue;
                for (size_t i = 0; i < function_count; ++i)
                    if (functions[i].offset == caller) { functions[i] = functions[--function_count]; ++skipped; break; }
            }
        }
        if (f) fclose(f);
        if (skipped) printf("%zu functions call setjmp: left to the translator\n", skipped);
        /* Import stubs (a jump through a slot the loader fills) stay too: recompiled, the jump is a
         * tail call through the runtime, which puts its own return address on the stack, and
         * setjmp saved that one. They gain nothing in C. */
        size_t stubs = 0;
        for (size_t i = 0; i < function_count;) {
            if (import_name(functions[i].offset)) { functions[i] = functions[--function_count]; ++stubs; }
            else ++i;
        }
        if (stubs) printf("%zu import stubs: left to the translator\n", stubs);
    }
    /* By offset, once each. */
    qsort(functions, function_count, sizeof(*functions), by_offset); /* offset first in Function */
    size_t unique = 0;
    for (size_t i = 0; i < function_count; ++i)
        if (!unique || functions[i].offset != functions[unique - 1].offset) functions[unique++] = functions[i];
    function_count = unique;
    recompiled = malloc(function_count * sizeof(*recompiled));
    for (size_t i = 0; i < function_count; ++i) recompiled[i] = functions[i].offset;
    /* A file per WINDOW of the image (OUT_<window>.c, compiled in parallel; the table in OUT.c):
     * adding functions changes only the files of their windows, and recomp.sh compiles only files
     * that changed. */
    enum { WINDOW = 0x20000 };
    char path[1024];
    const size_t stem = strlen(argv[3]) - (strlen(argv[3]) > 2 && !strcmp(argv[3] + strlen(argv[3]) - 2, ".c") ? 2 : 0);
    uint64_t stats[3] = {0, 0, 0}; /* translated, indirect jumps, instructions */
    int files = 0;
    uint64_t window = UINT64_MAX;
    out = NULL;
    for (size_t i = 0; i < function_count; ++i) {
        if (functions[i].offset / WINDOW != window) {
            if (out) close_output();
            window = functions[i].offset / WINDOW;
            snprintf(path, sizeof(path), "%.*s_%05" PRIx64 ".c", (int)stem, argv[3], window);
            out = fopen(path, "w+"); /* generate reads each function back */
            if (!out) { perror(path); return 1; }
            ++files;
            emit("/* Written by tools/recomp/bbrecomp.c from the game's eboot.bin: not to be distributed. */\n");
            emit("#include \"recomp/rc.h\"\n\nextern const RcApi *rc;\n");
        }
        generate(functions[i], stats);
    }
    if (out) close_output();
    out = fopen(argv[3], "w");
    if (!out) { perror(argv[3]); return 1; }
    emit("/* Written by tools/recomp/bbrecomp.c from the game's eboot.bin: not to be distributed. */\n");
    emit("#include \"recomp/rc.h\"\n\n__attribute__((visibility(\"hidden\"))) const RcApi *rc;\n");
    for (size_t i = 0; i < function_count; ++i)
        emit("void f_%" PRIx64 "(BbCpu *c);\n__attribute__((visibility(\"hidden\"))) RcFn d_%" PRIx64 ";\n",
             functions[i].offset, functions[i].offset);
    emit("\nstatic const RcFunction table[] = {\n");
    for (size_t i = 0; i < function_count; ++i)
        emit("    {0x%" PRIx64 ", %" PRIu64 ", 0x%" PRIx64 "ull, f_%" PRIx64 ", &d_%" PRIx64 "},\n", functions[i].offset,
             functions[i].size, bbcpu_recomp_hash(base + functions[i].offset, functions[i].size), functions[i].offset,
             functions[i].offset);
    emit("};\n\n__attribute__((visibility(\"default\"))) const RcFunction *bb_recomp_init(const RcApi *api, size_t *count) {\n");
    emit("    rc = api;\n    *count = sizeof(table) / sizeof(table[0]);\n    return table;\n}\n");
    fclose(out);
    printf("%zu functions, %" PRIu64 " instructions: %" PRIu64 " in C (%.0f%%), the rest in the interpreter; %d files\n",
           function_count, stats[2], stats[0], stats[2] ? 100.0 * (double)stats[0] / (double)stats[2] : 0.0, files);
    return 0;
}
