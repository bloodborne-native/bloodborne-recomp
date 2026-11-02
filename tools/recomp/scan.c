/* tools/recomp/scan.c ELF OUTDIR [POINTERS]: the game's function table for the recompiler (docs/RECOMPILATION.md).
 * Functions come from the unwind tables (PT_GNU_EH_FRAME: every function the compiler emitted
 * unwind information for, with its exact start and size), then from what reaches code outside
 * them: direct calls, jumps, lea of a code address, and POINTERS (tools/recomp/pointers.py: the
 * code addresses relocations put into data, vtables). Leaf functions often have no unwind entry;
 * those are decoded by following their control flow, up to the next known function. Each one is
 * decoded with Zydis:
 *   OUTDIR/functions.tsv  address, size, instructions, undecodable bytes
 *   OUTDIR/calls.tsv      caller, callee (direct calls, and jumps to another function's start)
 *   OUTDIR/slots.tsv      caller, slot (calls and jumps through a RIP-relative pointer: imports)
 *   OUTDIR/strings.tsv    function, string address, string (RIP-relative references to C strings)
 * Addresses are image offsets (the image is loaded at a base the runtime picks). */
#include <Zydis/Zydis.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* The ELF64 structures used (macOS has no <elf.h>). */
typedef struct { unsigned char e_ident[16]; uint16_t e_type, e_machine; uint32_t e_version;
    uint64_t e_entry, e_phoff, e_shoff; uint32_t e_flags; uint16_t e_ehsize, e_phentsize, e_phnum,
    e_shentsize, e_shnum, e_shstrndx; } Elf64_Ehdr;
typedef struct { uint32_t p_type, p_flags; uint64_t p_offset, p_vaddr, p_paddr, p_filesz, p_memsz,
    p_align; } Elf64_Phdr;
enum { PT_LOAD = 1, PT_GNU_EH_FRAME = 0x6474e550, PF_X = 1 };

static unsigned char *data;
static long data_size;
static Elf64_Phdr *loads[8];
static int nloads;

/* The file bytes at image address `va` with at least `len` bytes after them, or NULL. */
static const unsigned char *at(uint64_t va, uint64_t len) {
    for (int i = 0; i < nloads; ++i) {
        const Elf64_Phdr *p = loads[i];
        if (va >= p->p_vaddr && va + len <= p->p_vaddr + p->p_filesz)
            return data + p->p_offset + (va - p->p_vaddr);
    }
    return NULL;
}

typedef struct { uint64_t start, size; } Function;
static int by_start(const void *a, const void *b) {
    const Function *x = a, *y = b;
    return x->start < y->start ? -1 : x->start > y->start;
}

/* A C string at `va`: at least 4 printable bytes (UTF-8 allowed) and a NUL; its length, or 0. */
static size_t c_string(uint64_t va) {
    const unsigned char *s = at(va, 1);
    if (!s) return 0;
    size_t n = 0;
    while (at(va + n, 1) && s[n]) {
        if (s[n] < 0x20 && s[n] != '\t' && s[n] != '\n' && s[n] != '\r') return 0;
        if (++n > 4096) return 0;
    }
    return at(va + n, 1) && n >= 4 ? n : 0;
}

static void write_string(FILE *f, const unsigned char *s, size_t n) {
    for (size_t i = 0; i < n; ++i) {
        if (s[i] == '\t') fputs("\\t", f);
        else if (s[i] == '\n') fputs("\\n", f);
        else if (s[i] == '\r') fputs("\\r", f);
        else if (s[i] == '\\') fputs("\\\\", f);
        else fputc(s[i], f);
    }
}

/* ---- functions the unwind tables do not list ---- */

static uint64_t code_lo, code_hi; /* the executable segment */

/* The function containing `va` (start <= va < start + size), or NULL. */
static const Function *containing(const Function *f, uint32_t n, uint64_t va) {
    uint32_t lo = 0, hi = n;
    while (lo < hi) {
        const uint32_t mid = (lo + hi) / 2;
        if (f[mid].start <= va) lo = mid + 1; else hi = mid;
    }
    if (!lo) return NULL;
    const Function *c = &f[lo - 1];
    return va < c->start + (c->size ? c->size : 1) ? c : NULL;
}

/* The first function starting after `va`, or code_hi. */
static uint64_t next_start(const Function *f, uint32_t n, uint64_t va) {
    uint32_t lo = 0, hi = n;
    while (lo < hi) {
        const uint32_t mid = (lo + hi) / 2;
        if (f[mid].start <= va) lo = mid + 1; else hi = mid;
    }
    return lo < n ? f[lo].start : code_hi;
}

typedef struct { uint64_t *v; size_t n, cap; } List;
static void push(List *l, uint64_t x) {
    if (l->n == l->cap) { l->cap = l->cap ? l->cap * 2 : 1024; l->v = realloc(l->v, l->cap * sizeof(*l->v)); }
    l->v[l->n++] = x;
}
static int by_u64(const void *a, const void *b) {
    const uint64_t x = *(const uint64_t *)a, y = *(const uint64_t *)b;
    return x < y ? -1 : x > y;
}

/* Code addresses an instruction refers to: call/jmp targets, lea of code. */
static void references(const ZydisDecodedInstruction *ins, const ZydisDecodedOperand *ops, uint64_t next, List *out) {
    for (int o = 0; o < ins->operand_count_visible; ++o) {
        const ZydisDecodedOperand *op = &ops[o];
        uint64_t t;
        if ((ins->mnemonic == ZYDIS_MNEMONIC_CALL || ins->mnemonic == ZYDIS_MNEMONIC_JMP) &&
            op->type == ZYDIS_OPERAND_TYPE_IMMEDIATE && op->imm.is_relative)
            t = next + op->imm.value.s;
        else if (ins->mnemonic == ZYDIS_MNEMONIC_LEA && op->type == ZYDIS_OPERAND_TYPE_MEMORY &&
                 op->mem.base == ZYDIS_REGISTER_RIP && op->mem.index == ZYDIS_REGISTER_NONE)
            t = next + op->mem.disp.value;
        else
            continue;
        if (t >= code_lo && t < code_hi) push(out, t);
    }
}

/* The extent of a function without unwind information: its instructions reached from `start`
 * (branches followed, not past `limit`); 0 if its first instruction does not decode. */
static uint64_t follow(ZydisDecoder *dec, uint64_t start, uint64_t limit, List *refs) {
    List work = {0};
    push(&work, start);
    uint64_t end = start;
    static uint8_t *seen;
    static uint64_t seen_size;
    if (seen_size < code_hi) { free(seen); seen = calloc(code_hi, 1); seen_size = code_hi; }
    List touched = {0};
    while (work.n) {
        uint64_t pc = work.v[--work.n];
        for (;;) {
            if (pc < start || pc >= limit || seen[pc]) break;
            const unsigned char *code = at(pc, 1);
            if (!code) break;
            ZydisDecodedInstruction ins;
            ZydisDecodedOperand ops[ZYDIS_MAX_OPERAND_COUNT];
            const uint64_t avail = limit - pc < 15 ? limit - pc : 15;
            if (!ZYAN_SUCCESS(ZydisDecoderDecodeFull(dec, code, avail, &ins, ops))) {
                if (pc == start) { for (size_t i = 0; i < touched.n; ++i) seen[touched.v[i]] = 0; free(work.v); free(touched.v); return 0; }
                break;
            }
            seen[pc] = 1; push(&touched, pc);
            const uint64_t next = pc + ins.length;
            if (next > end) end = next;
            references(&ins, ops, next, refs);
            const int m = ins.mnemonic;
            if (m == ZYDIS_MNEMONIC_RET || m == ZYDIS_MNEMONIC_UD2 || m == ZYDIS_MNEMONIC_HLT || m == ZYDIS_MNEMONIC_INT3) break;
            if (ins.meta.category == ZYDIS_CATEGORY_COND_BR || m == ZYDIS_MNEMONIC_JMP) {
                if (ops[0].type == ZYDIS_OPERAND_TYPE_IMMEDIATE && ops[0].imm.is_relative) {
                    const uint64_t t = next + ops[0].imm.value.s;
                    if (t >= start && t < limit) push(&work, t);
                }
                if (m == ZYDIS_MNEMONIC_JMP) break;
            }
            pc = next;
        }
    }
    for (size_t i = 0; i < touched.n; ++i) seen[touched.v[i]] = 0;
    free(work.v); free(touched.v);
    return end - start;
}

/* The executable segment also holds the read-only data, which pointers reach as well. Code ends with
 * the import stubs, 16 bytes each after a 16-byte header, just past the last unwound function. */
static uint64_t end_of_code(const Function *f, uint32_t n) {
    uint64_t end = 0;
    for (uint32_t i = 0; i < n; ++i)
        if (f[i].start + f[i].size > end) end = f[i].start + f[i].size;
    for (uint64_t pc = end; pc < end + 0x10000 && pc + 16 <= code_hi; ++pc) {
        const unsigned char *b = at(pc, 16);
        if (!b) break;
        if (b[0] == 0xff && b[1] == 0x35 && b[6] == 0xff && b[7] == 0x25) {
            pc += 16;
            for (b = at(pc, 2); b && b[0] == 0xff && b[1] == 0x25; b = at(pc, 2)) pc += 16;
            return pc;
        }
    }
    return end;
}

/* Adds the functions reached from the known ones and from the pointers, until none is new. */
static uint32_t discover(Function **functions, uint32_t n, const char *pointers) {
    for (int i = 0; i < nloads; ++i)
        if (loads[i]->p_flags & PF_X) { code_lo = loads[i]->p_vaddr; code_hi = loads[i]->p_vaddr + loads[i]->p_filesz; }
    code_hi = end_of_code(*functions, n);
    ZydisDecoder dec;
    ZydisDecoderInit(&dec, ZYDIS_MACHINE_MODE_LONG_64, ZYDIS_STACK_WIDTH_64);
    List candidates = {0};
    if (pointers) {
        FILE *f = fopen(pointers, "r");
        if (!f) { perror(pointers); exit(1); }
        char line[64];
        while (fgets(line, sizeof(line), f)) {
            const uint64_t t = strtoull(line, NULL, 0);
            if (t >= code_lo && t < code_hi) push(&candidates, t);
        }
        fclose(f);
    }
    /* References from the unwound functions (linear: their extent is exact). */
    for (uint32_t i = 0; i < n; ++i) {
        const Function fn = (*functions)[i];
        const unsigned char *code = at(fn.start, fn.size);
        if (!code) continue;
        for (uint64_t off = 0; off < fn.size;) {
            ZydisDecodedInstruction ins;
            ZydisDecodedOperand ops[ZYDIS_MAX_OPERAND_COUNT];
            if (!ZYAN_SUCCESS(ZydisDecoderDecodeFull(&dec, code + off, fn.size - off, &ins, ops))) { ++off; continue; }
            references(&ins, ops, fn.start + off + ins.length, &candidates);
            off += ins.length;
        }
    }
    for (int round = 1; candidates.n; ++round) {
        qsort(candidates.v, candidates.n, sizeof(uint64_t), by_u64);
        List fresh = {0};
        for (size_t i = 0; i < candidates.n; ++i) {
            const uint64_t t = candidates.v[i];
            if ((i && t == candidates.v[i - 1]) || containing(*functions, n, t)) continue;
            push(&fresh, t);
        }
        candidates.n = 0;
        if (!fresh.n) break;
        /* Each new start is bounded by the next known or new start. */
        *functions = realloc(*functions, (n + fresh.n) * sizeof(Function));
        uint32_t added = 0;
        for (size_t i = 0; i < fresh.n; ++i) {
            const uint64_t t = fresh.v[i];
            uint64_t limit = next_start(*functions, n, t);
            if (i + 1 < fresh.n && fresh.v[i + 1] < limit) limit = fresh.v[i + 1];
            const uint64_t size = follow(&dec, t, limit, &candidates);
            if (size) (*functions)[n + added++] = (Function){t, size};
        }
        n += added;
        qsort(*functions, n, sizeof(Function), by_start);
        fprintf(stderr, "round %d: %u new functions\n", round, added);
        free(fresh.v);
    }
    free(candidates.v);
    return n;
}

int main(int argc, char **argv) {
    if (argc != 3 && argc != 4) {
        fprintf(stderr, "usage: %s ELF OUTDIR [POINTERS]\n", argv[0]);
        return 2;
    }
    FILE *in = fopen(argv[1], "rb");
    if (!in) { perror(argv[1]); return 1; }
    fseek(in, 0, SEEK_END); data_size = ftell(in); fseek(in, 0, SEEK_SET);
    data = malloc(data_size);
    if (fread(data, 1, data_size, in) != (size_t)data_size) return 1;
    fclose(in);
    const Elf64_Ehdr *eh = (const Elf64_Ehdr *)data;
    const Elf64_Phdr *eh_frame = NULL;
    for (int p = 0; p < eh->e_phnum; ++p) {
        Elf64_Phdr *ph = (Elf64_Phdr *)(data + eh->e_phoff + p * eh->e_phentsize);
        if (ph->p_type == PT_LOAD && nloads < 8 && ph->p_offset + ph->p_filesz <= (uint64_t)data_size)
            loads[nloads++] = ph;
        if (ph->p_type == PT_GNU_EH_FRAME) eh_frame = ph;
    }
    if (!eh_frame) { fprintf(stderr, "no PT_GNU_EH_FRAME\n"); return 1; }

    /* .eh_frame_hdr: version, eh_frame_ptr_enc, fde_count_enc, table_enc, then the search table
     * of (initial location, FDE address) pairs, both datarel sdata4 (0x3b) from the header. */
    const unsigned char *hdr = data + eh_frame->p_offset;
    if (hdr[0] != 1 || hdr[2] != 0x03 || hdr[3] != 0x3b) {
        fprintf(stderr, "unexpected .eh_frame_hdr encodings %#x %#x %#x\n", hdr[1], hdr[2], hdr[3]);
        return 1;
    }
    uint32_t count;
    memcpy(&count, hdr + 8, 4);
    Function *functions = calloc(count, sizeof(Function));
    uint32_t nfunctions = 0;
    for (uint32_t i = 0; i < count; ++i) {
        int32_t loc, fde_rel;
        memcpy(&loc, hdr + 12 + 8 * i, 4);
        memcpy(&fde_rel, hdr + 16 + 8 * i, 4);
        /* FDE: length, CIE pointer, pc_begin (pcrel sdata4: augmentation "zR" with 0x1b), pc_range. */
        const uint64_t fde = eh_frame->p_vaddr + fde_rel;
        const unsigned char *f = at(fde, 16);
        if (!f) continue;
        uint32_t range;
        memcpy(&range, f + 12, 4);
        functions[nfunctions++] = (Function){eh_frame->p_vaddr + loc, range};
    }
    qsort(functions, nfunctions, sizeof(Function), by_start);
    const uint32_t unwound = nfunctions;
    nfunctions = discover(&functions, nfunctions, argc > 3 ? argv[3] : NULL);

    char path[1024];
    FILE *out_functions, *out_calls, *out_slots, *out_strings;
#define OPEN(var, name)                                                                   \
    snprintf(path, sizeof(path), "%s/" name, argv[2]);                                    \
    if (!(var = fopen(path, "w"))) { perror(path); return 1; }
    OPEN(out_functions, "functions.tsv");
    OPEN(out_calls, "calls.tsv");
    OPEN(out_slots, "slots.tsv");
    OPEN(out_strings, "strings.tsv");
    fputs("address\tsize\tinstructions\tundecodable\n", out_functions);
    fputs("caller\tcallee\n", out_calls);
    fputs("caller\tslot\n", out_slots);
    fputs("function\taddress\tstring\n", out_strings);

    ZydisDecoder dec;
    ZydisDecoderInit(&dec, ZYDIS_MACHINE_MODE_LONG_64, ZYDIS_STACK_WIDTH_64);
    uint64_t total_instructions = 0, total_calls = 0, total_strings = 0;
    for (uint32_t i = 0; i < nfunctions; ++i) {
        const Function fn = functions[i];
        const unsigned char *code = at(fn.start, fn.size);
        if (!code) continue;
        uint64_t off = 0, instructions = 0, undecodable = 0;
        ZydisDecodedInstruction ins;
        ZydisDecodedOperand ops[ZYDIS_MAX_OPERAND_COUNT];
        while (off < fn.size) {
            if (!ZYAN_SUCCESS(ZydisDecoderDecodeFull(&dec, code + off, fn.size - off, &ins, ops))) {
                ++undecodable; ++off;
                continue;
            }
            ++instructions;
            const uint64_t next = fn.start + off + ins.length;
            const int is_call = ins.mnemonic == ZYDIS_MNEMONIC_CALL;
            const int is_jmp = ins.mnemonic == ZYDIS_MNEMONIC_JMP;
            if ((is_call || is_jmp) && ins.operand_count_visible) {
                const ZydisDecodedOperand *op = &ops[0];
                if (op->type == ZYDIS_OPERAND_TYPE_IMMEDIATE && op->imm.is_relative) {
                    const uint64_t target = next + op->imm.value.s;
                    if (is_call || target < fn.start || target >= fn.start + fn.size) {
                        fprintf(out_calls, "%#llx\t%#llx\n", (unsigned long long)fn.start,
                                (unsigned long long)target);
                        ++total_calls;
                    }
                } else if (op->type == ZYDIS_OPERAND_TYPE_MEMORY && op->mem.base == ZYDIS_REGISTER_RIP &&
                           op->mem.index == ZYDIS_REGISTER_NONE) {
                    fprintf(out_slots, "%#llx\t%#llx\n", (unsigned long long)fn.start,
                            (unsigned long long)(next + op->mem.disp.value));
                }
            } else {
                for (int o = 0; o < ins.operand_count_visible; ++o) {
                    const ZydisDecodedOperand *op = &ops[o];
                    if (op->type != ZYDIS_OPERAND_TYPE_MEMORY || op->mem.base != ZYDIS_REGISTER_RIP ||
                        op->mem.index != ZYDIS_REGISTER_NONE)
                        continue;
                    const uint64_t target = next + op->mem.disp.value;
                    const size_t n = ins.mnemonic == ZYDIS_MNEMONIC_LEA ? c_string(target) : 0;
                    if (n) {
                        fprintf(out_strings, "%#llx\t%#llx\t", (unsigned long long)fn.start,
                                (unsigned long long)target);
                        write_string(out_strings, at(target, n), n);
                        fputc('\n', out_strings);
                        ++total_strings;
                    }
                }
            }
            off += ins.length;
        }
        total_instructions += instructions;
        fprintf(out_functions, "%#llx\t%llu\t%llu\t%llu\n", (unsigned long long)fn.start,
                (unsigned long long)fn.size, (unsigned long long)instructions,
                (unsigned long long)undecodable);
    }
    fclose(out_functions); fclose(out_calls); fclose(out_slots); fclose(out_strings);
    printf("%u functions (%u from the unwind tables), %llu instructions, %llu direct calls, %llu string references\n",
           nfunctions, unwound, (unsigned long long)total_instructions, (unsigned long long)total_calls,
           (unsigned long long)total_strings);
    return 0;
}
