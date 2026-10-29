/* bbcpu decode cache: guest code decoded once per basic block with Zydis into compact BbInsn
 * records. Blocks are immutable once published; lookups are lock-free, inserts take a mutex. */
#include "cpu_internal.h"
#include "trace.h"
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>

enum { BUCKETS = 1 << 20, MAX_BLOCK = 64, MAX_RANGES = 16 };
static BbBlock *buckets[BUCKETS];
static pthread_mutex_t insert_lock = PTHREAD_MUTEX_INITIALIZER;
static struct { uint64_t start, end; } ranges[MAX_RANGES];
static int range_count;
static ZydisDecoder decoder;
static uint64_t decoded_blocks, decoded_insns;

void bbcpu_add_guest_code(uintptr_t start, size_t size) {
    if (range_count == MAX_RANGES) {
        fputs("bbcpu: too many guest code ranges\n", stderr);
        abort();
    }
    ranges[range_count].start = start;
    ranges[range_count].end = start + size;
    ++range_count;
    if (range_count == 1) ZydisDecoderInit(&decoder, ZYDIS_MACHINE_MODE_LONG_64, ZYDIS_STACK_WIDTH_64);
}

int bbcpu_is_guest(uintptr_t address) { return bbcpu_is_guest_code(address); }

static uint32_t hash(uint64_t rip);

/* Blocks covering bucket h's part of the range are unlinked (and leaked: another thread may be
 * running them). */
static void unlink_overlapping(uint32_t h, uint64_t address, uint64_t size) {
    BbBlock **link = &buckets[h];
    while (*link) {
        BbBlock *b = *link;
        uint64_t end = b->start;
        for (uint32_t i = 0; i < b->count; ++i) end += b->insn[i].length;
        if (b->start < address + size && address < end) __atomic_store_n(link, b->next, __ATOMIC_RELEASE);
        else link = &b->next;
    }
}

/* A block holds at most MAX_BLOCK instructions of at most 15 bytes, so a block overlapping the
 * range starts at most MAX_BLOCK * 15 - 1 bytes before it: only those start addresses' buckets
 * are looked at, not the whole table. */
void bbcpu_invalidate(uintptr_t address, size_t size) {
    const uint64_t reach = MAX_BLOCK * 15 - 1;
    const uint64_t first = address > reach ? address - reach : 0;
    const uint64_t last = address + size; /* exclusive */
    pthread_mutex_lock(&insert_lock);
    if (last - first >= BUCKETS) {
        for (uint32_t h = 0; h < BUCKETS; ++h) unlink_overlapping(h, address, size);
    } else {
        for (uint64_t start = first; start < last; ++start) unlink_overlapping(hash(start), address, size);
    }
    pthread_mutex_unlock(&insert_lock);
}

int bbcpu_is_guest_code(uint64_t address) {
    for (int i = 0; i < range_count; ++i)
        if (address >= ranges[i].start && address < ranges[i].end) return 1;
    return 0;
}

static uint8_t gpr_index(ZydisRegister reg, uint8_t *high8) {
    *high8 = reg == ZYDIS_REGISTER_AH || reg == ZYDIS_REGISTER_CH || reg == ZYDIS_REGISTER_DH ||
             reg == ZYDIS_REGISTER_BH;
    const ZydisRegister full = ZydisRegisterGetLargestEnclosing(ZYDIS_MACHINE_MODE_LONG_64, reg);
    return (uint8_t)ZydisRegisterGetId(full);
}

static void convert_operand(const ZydisDecodedInstruction *zi, const ZydisDecodedOperand *zo,
                            BbOp *op) {
    memset(op, 0, sizeof(*op));
    op->size = (uint8_t)(zo->size / 8);
    switch (zo->type) {
    case ZYDIS_OPERAND_TYPE_REGISTER: {
        op->type = OP_REG;
        const ZydisRegisterClass cls = ZydisRegisterGetClass(zo->reg.value);
        switch (cls) {
        case ZYDIS_REGCLASS_GPR8: case ZYDIS_REGCLASS_GPR16: case ZYDIS_REGCLASS_GPR32:
        case ZYDIS_REGCLASS_GPR64:
            op->kind = RK_GPR;
            op->reg = gpr_index(zo->reg.value, &op->high8);
            break;
        case ZYDIS_REGCLASS_XMM: case ZYDIS_REGCLASS_YMM:
            op->kind = RK_VEC;
            op->reg = (uint8_t)ZydisRegisterGetId(zo->reg.value);
            break;
        case ZYDIS_REGCLASS_X87:
            op->kind = RK_X87;
            op->reg = (uint8_t)ZydisRegisterGetId(zo->reg.value);
            break;
        default:
            op->kind = RK_OTHER;
            op->reg = (uint8_t)(zo->reg.value & 0xff);
            break;
        }
        break;
    }
    case ZYDIS_OPERAND_TYPE_MEMORY:
        op->type = zo->mem.type == ZYDIS_MEMOP_TYPE_AGEN ? OP_AGEN : OP_MEM;
        op->base = 0xff;
        op->index = 0xff;
        if (zo->mem.base == ZYDIS_REGISTER_RIP) op->base = 0xfe;
        else if (zo->mem.base != ZYDIS_REGISTER_NONE) op->base = gpr_index(zo->mem.base, &op->high8);
        if (zo->mem.index != ZYDIS_REGISTER_NONE) {
            uint8_t unused;
            op->index = gpr_index(zo->mem.index, &unused);
        }
        op->high8 = 0;
        op->scale = zo->mem.scale ? zo->mem.scale : 1;
        op->disp = zo->mem.disp.has_displacement ? zo->mem.disp.value : 0;
        op->segment = zo->mem.segment == ZYDIS_REGISTER_FS ? 1 : zo->mem.segment == ZYDIS_REGISTER_GS ? 2 : 0;
        break;
    case ZYDIS_OPERAND_TYPE_IMMEDIATE:
        op->type = OP_IMM;
        op->disp = zo->imm.is_signed ? zo->imm.value.s : (int64_t)zo->imm.value.u;
        if (zo->imm.is_relative) op->disp += zi->length; /* relative to the next instruction */
        break;
    default:
        op->type = OP_NONE;
        break;
    }
}

static int ends_block(const ZydisDecodedInstruction *zi) {
    if (zi->meta.branch_type != ZYDIS_BRANCH_TYPE_NONE) return 1;
    switch (zi->mnemonic) {
    case ZYDIS_MNEMONIC_RET: case ZYDIS_MNEMONIC_INT3: case ZYDIS_MNEMONIC_UD2:
    case ZYDIS_MNEMONIC_HLT: case ZYDIS_MNEMONIC_SYSCALL: case ZYDIS_MNEMONIC_INT:
        return 1;
    default:
        return 0;
    }
}

static BbBlock *decode_block(uint64_t rip) {
    BbInsn insns[MAX_BLOCK];
    uint32_t count = 0;
    uint64_t at = rip;
    while (count < MAX_BLOCK) {
        /* A native or recompiled function, or a probe, starts its own block: the interpreter and
         * the JIT look them up at block starts only (bbcpu_step). */
        if (count && ((bbcpu_native_count && bbcpu_native_at(at)) ||
                      (bbcpu_recomp_count && bbcpu_recomp_at(at)) ||
                      (bbcpu_probe_count && bbcpu_probe_at(at))))
            break;
        ZydisDecodedInstruction zi;
        ZydisDecodedOperand zo[ZYDIS_MAX_OPERAND_COUNT];
        BbInsn *in = &insns[count];
        memset(in, 0, sizeof(*in));
        if (!ZYAN_SUCCESS(ZydisDecoderDecodeFull(&decoder, (const void *)at, 15, &zi, zo))) {
            in->mnemonic = ZYDIS_MNEMONIC_INVALID;
            in->length = 1;
            in->branch = 1;
            ++count;
            break;
        }
        in->mnemonic = (uint16_t)zi.mnemonic;
        in->length = zi.length;
        in->count = zi.operand_count_visible > 5 ? 5 : zi.operand_count_visible;
        in->opsize = (uint8_t)(zi.operand_width / 8);
        in->vex = zi.encoding == ZYDIS_INSTRUCTION_ENCODING_VEX ||
                  zi.encoding == ZYDIS_INSTRUCTION_ENCODING_EVEX;
        in->vl = (uint8_t)(zi.avx.vector_length / 8);
        in->lock = (zi.attributes & ZYDIS_ATTRIB_HAS_LOCK) != 0;
        in->rep = (zi.attributes & (ZYDIS_ATTRIB_HAS_REP | ZYDIS_ATTRIB_HAS_REPE)) != 0;
        in->repne = (zi.attributes & ZYDIS_ATTRIB_HAS_REPNE) != 0;
        in->branch = (uint8_t)ends_block(&zi);
        if (zi.cpu_flags) {
            const uint32_t arith = 0x8d5; /* CF PF AF ZF SF OF */
            in->freads = (uint16_t)(zi.cpu_flags->tested & arith);
            in->fwrites = (uint16_t)((zi.cpu_flags->modified | zi.cpu_flags->set_0 |
                                      zi.cpu_flags->set_1 | zi.cpu_flags->undefined) & arith);
        }
        for (int i = 0; i < in->count; ++i) convert_operand(&zi, &zo[i], &in->op[i]);
        at += zi.length;
        ++count;
        if (in->branch) break;
    }
    BbBlock *block = malloc(sizeof(BbBlock) + count * sizeof(BbInsn));
    if (!block) abort();
    block->start = rip;
    block->jit_code = NULL;
    block->jit_failed = 0;
    block->count = count;
    block->next = NULL;
    memcpy(block->insn, insns, count * sizeof(BbInsn));
    return block;
}

/* A block decoded for the caller alone, not cached (the caller frees it): hashing every
 * recompiled function at load (recomp.c) would otherwise keep the decoded blocks of the whole
 * game, which the translator never runs once the library replaced them. */
BbBlock *bbcpu_block_uncached(uint64_t rip) { return decode_block(rip); }

static uint32_t hash(uint64_t rip) { return (uint32_t)((rip * 0x9E3779B97F4A7C15ull) >> 44); }

const BbBlock *bbcpu_block(uint64_t rip) {
    const uint32_t h = hash(rip);
    for (BbBlock *b = __atomic_load_n(&buckets[h], __ATOMIC_ACQUIRE); b; b = b->next)
        if (b->start == rip) return b;
    BbBlock *block = decode_block(rip);
    pthread_mutex_lock(&insert_lock);
    for (BbBlock *b = buckets[h]; b; b = b->next)
        if (b->start == rip) {
            pthread_mutex_unlock(&insert_lock);
            free(block);
            return b;
        }
    block->next = buckets[h];
    __atomic_store_n(&buckets[h], block, __ATOMIC_RELEASE);
    ++decoded_blocks;
    decoded_insns += block->count;
    pthread_mutex_unlock(&insert_lock);
    return block;
}

void bbcpu_decode_stats(uint64_t *blocks, uint64_t *insns) {
    *blocks = decoded_blocks;
    *insns = decoded_insns;
}
