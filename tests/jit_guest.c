/* tests/jit_guest.c: x86-64 guest functions for tests/test_jit.c. Built as an x86-64 executable
 * at a fixed base (tools/cpu/test_jit.sh); run natively (Rosetta) it prints the expected results,
 * test_jit.c maps it into an arm64 process and runs the same functions through bbcpu. No libc
 * calls from the tests (the arm64 side has no x86 libc): memset/memcpy are defined here. */
#include <immintrin.h>
#include <stdint.h>
#include <stdio.h>

#define T __attribute__((noinline, used))

T void *memset(void *d, int c, unsigned long n) {
    unsigned char *p = d;
    while (n--) *p++ = (unsigned char)c;
    return d;
}
T void *memcpy(void *d, const void *s, unsigned long n) {
    unsigned char *p = d;
    const unsigned char *q = s;
    while (n--) *p++ = *q++;
    return d;
}

T uint64_t t_arith(uint64_t a, uint64_t b, uint64_t c) {
    uint64_t r = a * 0x9E3779B97F4A7C15ull + (b >> (c & 63)) - (a << (b & 31));
    r ^= (r >> 17) | (a << 7);
    r += (uint64_t)((int64_t)b / ((int64_t)(c | 1)));
    r += b % (a | 1);
    r += (uint64_t)__builtin_popcountll(a) + (uint64_t)__builtin_ctzll(b | 1) + (uint64_t)__builtin_clzll(c | 1);
    r += (uint32_t)a * (uint32_t)b;
    r += (uint64_t)(int16_t)(a >> 3) * (uint64_t)(int8_t)b;
    r += a > b ? a - b : b - a;
    r += (int64_t)a < (int64_t)c ? 7 : 13;
    unsigned __int128 wide = (unsigned __int128)a * b;
    r += (uint64_t)(wide >> 64);
    r += __builtin_bswap64(c) + __builtin_bswap32((uint32_t)a);
    r += (a >> 61) | (a << 3);
    return r;
}

/* Comparisons and branches of every condition, signed and unsigned, all sizes. */
T uint64_t t_compare(uint64_t a, uint64_t b, uint64_t c) {
    uint64_t r = 0;
    const int8_t a8 = (int8_t)a, b8 = (int8_t)b;
    const int16_t a16 = (int16_t)a, b16 = (int16_t)b;
    const int32_t a32 = (int32_t)a, b32 = (int32_t)b;
    const int64_t a64 = (int64_t)a, b64 = (int64_t)b;
    r = r * 3 + (a8 < b8) + 2 * ((uint8_t)a8 < (uint8_t)b8) + 4 * (a8 == b8) + 8 * (a8 >= b8);
    r = r * 3 + (a16 < b16) + 2 * ((uint16_t)a16 <= (uint16_t)b16) + 4 * (a16 != b16) + 8 * (a16 > b16);
    r = r * 3 + (a32 < b32) + 2 * ((uint32_t)a32 > (uint32_t)b32) + 4 * (a32 <= b32) + 8 * ((uint32_t)a32 >= (uint32_t)b32);
    r = r * 3 + (a64 < b64) + 2 * (a < b) + 4 * (a64 <= b64) + 8 * (a <= b) + 16 * (a64 > b64);
    for (uint64_t i = 0; i < (c & 15) + 3; ++i) {
        if ((int64_t)(a - i * b) < 0) r += i; else r ^= i << 3;
        if ((uint32_t)(b + i) > (uint32_t)c) r -= 5;
        if (((a >> i) & 1) == 0) r = r * 7 + 1;
        int32_t s = (int32_t)(a >> 7) + (int32_t)i;
        r += s > 1000 ? 1 : s < -1000 ? 2 : 3;
    }
    uint64_t sum = a, carry = 0;
    __asm__ volatile("add %[b], %[s]\n\tadc %[c], %[s]\n\tsbb %[b], %[s]\n\tsetc %b[k]"
                     : [s] "+r"(sum), [k] "+q"(carry) : [b] "r"(b), [c] "r"(c) : "cc");
    r += sum + carry;
    uint32_t x = (uint32_t)a;
    __asm__ volatile("neg %[x]\n\tsbb %[x], %[x]" : [x] "+r"(x) : : "cc");
    r += x;
    uint8_t p = 0;
    __asm__ volatile("test %[v], %[v]\n\tsetp %[p]" : [p] "=q"(p) : [v] "r"(a & 0xff) : "cc");
    r += p;
    return r;
}

/* Shifts, inc/dec, cmov, setcc, lea forms, memory operands. */
T uint64_t t_misc(uint64_t a, uint64_t b, uint64_t c) {
    uint64_t mem[8];
    for (int i = 0; i < 8; ++i) mem[i] = a * (uint64_t)(i + 1) ^ b;
    uint64_t r = 0;
    for (int i = 0; i < 8; ++i) {
        mem[i] += c;
        mem[(i + 3) & 7] ^= mem[i] >> 3;
        uint32_t w = (uint32_t)mem[i];
        w <<= 3; w >>= 1;
        int32_t sw = (int32_t)mem[i];
        sw >>= 5;
        r += w + (uint64_t)sw;
        r += (mem[i] & 1) ? mem[i] : r;
        uint8_t byte = (uint8_t)mem[i];
        ++byte;
        r += byte;
        uint16_t half = (uint16_t)(mem[i] >> 16);
        --half;
        r ^= half;
    }
    int64_t s = (int64_t)a;
    r += (uint64_t)(s * 3 + (int64_t)b * 5 + 77);
    r += (uint64_t)((int32_t)a * (int32_t)b);
    r += (uint64_t)((int64_t)a * 1234567);
    r += a > b ? a : b;
    r += (int64_t)a < 0 ? 1 : 0;
    return r;
}

/* Recursion, a jump table, indirect calls. */
T static uint64_t fib(uint64_t n) { return n < 2 ? n : fib(n - 1) + fib(n - 2); }
T static uint64_t add3(uint64_t x) { return x + 3; }
T static uint64_t mul5(uint64_t x) { return x * 5; }
T uint64_t t_control(uint64_t a, uint64_t b, uint64_t c) {
    uint64_t (*volatile ops[2])(uint64_t) = {add3, mul5};
    uint64_t r = 0;
    for (uint64_t k = 0; k < (a & 31) + 10; ++k) {
        switch ((k + b) & 7) {
        case 0: r += k * 3; break;
        case 1: r ^= k << 4; break;
        case 2: r -= c; break;
        case 3: r = r * 5 + 1; break;
        case 4: r += fib(k & 15); break;
        case 5: r ^= r >> 11; break;
        case 6: r += (uint64_t)(int64_t)(int32_t)k; break;
        default: r -= 77; break;
        }
        r = ops[k & 1](r);
    }
    return r;
}

T uint64_t t_vector(uint64_t a, uint64_t b, uint64_t c) {
    float fa = (float)(a & 0xffff) / 7.0f, fb = (float)(b & 0xffff) / 3.0f, fc = (float)(c & 0xff) - 100.0f;
    __m128 x = _mm_set_ps(fa, fb, fc, fa - fb);
    __m128 y = _mm_set_ps(fc, fa, fb * 2.0f, 0.5f);
    __m128 z = _mm_add_ps(_mm_mul_ps(x, y), _mm_shuffle_ps(x, y, 0x1b));
    z = _mm_max_ps(z, _mm_min_ps(x, y));
    __m128i i = _mm_cvttps_epi32(_mm_mul_ps(z, _mm_set1_ps(100.0f)));
    i = _mm_add_epi32(i, _mm_shuffle_epi32(i, 0x4e));
    float out[4];
    _mm_storeu_ps(out, z);
    uint64_t r = (uint64_t)_mm_cvtsi128_si64(i) ^ (uint64_t)_mm_extract_epi64(i, 1);
    r += (uint64_t)_mm_movemask_ps(_mm_cmplt_ps(x, y));
    r += (uint64_t)(int64_t)((out[0] + out[1] * 3.0f + out[2] * 5.0f + out[3] * 7.0f) * 8.0f);
    double d = (double)a / (double)(b | 1);
    r += (uint64_t)(int64_t)(d * 1024.0);
    return r;
}

/* Scalar float paths: arithmetic, min/max with NaN, comparisons feeding branches, conversions. */
T uint64_t t_float(uint64_t a, uint64_t b, uint64_t c) {
    union { uint32_t u; float f; } fu = {.u = (uint32_t)a};
    float x = (float)(int32_t)a / 4096.0f, y = (float)(int32_t)b / 1024.0f, z = fu.f; /* z: any bits, NaN too */
    double dx = (double)(int64_t)c / 3.0, dy = (double)x * 1.5;
    uint64_t r = 0;
    __m128 vx = _mm_set_ss(x), vy = _mm_set_ss(y), vz = _mm_set_ss(z);
    r += (uint64_t)_mm_cvtsi128_si32(_mm_castps_si128(_mm_min_ss(vx, vz)));
    r += (uint64_t)_mm_cvtsi128_si32(_mm_castps_si128(_mm_max_ss(vz, vy)));
    r += (uint64_t)_mm_cvtsi128_si32(_mm_castps_si128(_mm_min_ps(_mm_set_ps(x, y, z, 1.0f), _mm_set_ps(z, x, y, -1.0f))));
    for (int i = 0; i < 8; ++i) {
        x = x * 1.25f - y / (float)(i + 1);
        if (x > y) r += 1; else if (x < y) r += 2; else r += 3;
        if (z == z) r += 5;
        if (!(x >= z)) r += 7;
        dy = dy * 0.75 + dx;
        if (dy < dx) r ^= (uint64_t)i << 9;
    }
    r += (uint64_t)(int64_t)_mm_cvtt_ss2si(vz) + (uint64_t)(int64_t)_mm_cvttss_si64(_mm_set_ss(x * 1e12f));
    r += (uint64_t)(int64_t)_mm_cvttsd_si32(_mm_set_sd(dy)) + (uint64_t)(int64_t)(float)dy;
    union { float f; uint32_t u; } out = {.f = x + y * 0.5f};
    r += out.u + (uint64_t)(int64_t)(x * 100.0f) + (uint64_t)(int64_t)(dx * 7.0);
    unsigned s = (unsigned)(b & 63), s32 = (unsigned)(b & 31);
    r += (c << s) + (c >> s) + (uint64_t)((int64_t)c >> s) + ((uint32_t)c << s32) + ((uint32_t)c >> s32);
    r += (uint64_t)(((int64_t)a >> (c & 63)) & 1) + ((a >> (b & 63)) & 1);
    return r;
}

/* MXCSR FTZ/DAZ (a & 3: none, FTZ, DAZ, both), as bbcpu reproduces them on arm64
 * (bbcpu_apply_host_fp; tools/cpu/test_jit.sh runs with BBCPU_FTZ=1, so the FTZ-only and
 * DAZ-only cases use the approximate mappings, which are off by default): under FTZ a denormal
 * result of normal inputs is flushed, under DAZ a denormal input reads as zero. Not checked: a
 * denormal input under FTZ only and a denormal result under DAZ only (FPCR.FZ flushes both;
 * x86 flushes neither; with FEAT_AFP, DAZ only uses FIZ and is exact). */
static inline unsigned getcsr(void) { unsigned v; __asm__ volatile("stmxcsr %0" : "=m"(v)); return v; }
static inline void setcsr(unsigned v) { __asm__ volatile("ldmxcsr %0" : : "m"(v) : "memory"); }
T uint64_t t_mxcsr(uint64_t a, uint64_t b, uint64_t c) {
    const unsigned saved = getcsr(), mode = (unsigned)a & 3;
    const int ftz = mode & 1, daz = (mode >> 1) & 1;
    union { uint32_t u; float f; } den = {.u = 1u + ((uint32_t)b & 0x7ffffeu)},  /* denormal */
        low = {.u = 0x00800000u | ((uint32_t)c & 0x7fffffu)};                    /* smallest exponent */
    union { uint64_t u; double d; } dden = {.u = 1u + (b & 0xffffffffffffeull)},
        dlow = {.u = 0x0010000000000000ull | (c & 0xfffffffffffffull)};
    volatile float big = 1.2676506e30f, half = 0.5f; /* 2^100: a denormal input gives a normal result */
    volatile double dbig = 1.2676506002282294e30, dhalf = 0.5;
    setcsr((saved & ~0x8040u) | (ftz ? 0x8000u : 0) | (daz ? 0x40u : 0));
    union { float f; uint32_t u; } r1 = {0}, r2 = {0};
    union { double d; uint64_t u; } r3 = {0}, r4 = {0};
    union { __m128 v; uint32_t u[4]; } r5 = {0};
    if (daz || !ftz) { /* denormal inputs */
        r1.f = den.f * big;
        r3.d = dden.d * dbig;
        r5.v = _mm_mul_ps(_mm_set_ps(den.f, 1.0f, den.f, 2.0f), _mm_set1_ps(big));
    }
    if (ftz || !daz) { /* denormal results */
        r2.f = low.f * half;
        r4.d = dlow.d * dhalf;
    }
    __asm__ volatile("" : : "m"(r1), "m"(r2), "m"(r3), "m"(r4), "m"(r5) : "memory");
    setcsr(saved);
    return ((uint64_t)r1.u << 32 | r2.u) ^ r3.u ^ (r4.u * 3) ^ ((uint64_t)r5.u[3] << 32 | r5.u[1]) ^
           r5.u[0] ^ r5.u[2];
}

T uint64_t t_string(uint64_t a, uint64_t b, uint64_t c) {
    unsigned char buf[256], out[256];
    for (int k = 0; k < 256; ++k) buf[k] = (unsigned char)((a >> (k & 63)) ^ (uint64_t)k);
    unsigned long n = (unsigned long)(b & 255);
    void *d = out, *s = buf;
    unsigned long cnt = n;
    __asm__ volatile("rep movsb" : "+D"(d), "+S"(s), "+c"(cnt) : : "memory");
    memset(out + n, (int)(c & 0xff), 256 - n);
    uint64_t r = 0;
    for (int k = 0; k < 256; k += 8) { uint64_t v; memcpy(&v, out + k, 8); r = r * 31 + v; }
    return r;
}


/* Memory operand forms of every translated instruction, all sizes, at misaligned addresses (and
 * across 16 bytes), with the flags after each one folded into the result. */
/* FLAGSM: only the flags x86 defines for the instruction (logic: no AF; shifts by more than 1: no
 * OF; by cl: CF/OF undefined past the width; imul: CF and OF). */
#define FLAGSM(r, mask) do { uint64_t f_; __asm__ volatile("pushfq\n\tpopq %0" : "=r"(f_)); (r) = (r) * 33 + (f_ & (mask)); } while (0)
#define FLAGS(r) FLAGSM(r, 0x8d5)
#define MEM_OPS(type, suf, reg)                                                                     \
    do {                                                                                          \
        type *m = (type *)(buf + o);                                                              \
        type v = (type)b, w;                                                                      \
        __asm__ volatile("add" suf " %1, %0" : "+m"(*m) : reg(v) : "cc"); FLAGS(r);               \
        __asm__ volatile("sub" suf " %1, %0" : "+m"(*m) : reg(v) : "cc"); FLAGS(r);               \
        __asm__ volatile("xor" suf " %1, %0" : "+m"(*m) : reg((type)c) : "cc"); FLAGSM(r, 0x8c5);         \
        __asm__ volatile("and" suf " %1, %0" : "+m"(*m) : reg((type)~v) : "cc"); FLAGSM(r, 0x8c5);        \
        __asm__ volatile("or" suf " %1, %0" : "+m"(*m) : reg((type)(v >> 3)) : "cc"); FLAGSM(r, 0x8c5);   \
        __asm__ volatile("stc\n\tadc" suf " %1, %0" : "+m"(*m) : reg(v) : "cc"); FLAGS(r);       \
        __asm__ volatile("stc\n\tsbb" suf " %1, %0" : "+m"(*m) : reg((type)c) : "cc"); FLAGS(r); \
        __asm__ volatile("cmp" suf " %1, %0" : : "m"(*m), reg(v) : "cc"); FLAGS(r);               \
        __asm__ volatile("test" suf " %1, %0" : : "m"(*m), reg(v) : "cc"); FLAGSM(r, 0x8c5);              \
        __asm__ volatile("add" suf " $0x7f, %0" : "+m"(*m) : : "cc"); FLAGS(r);                   \
        __asm__ volatile("sub" suf " $-3, %0" : "+m"(*m) : : "cc"); FLAGS(r);                     \
        __asm__ volatile("cmp" suf " $0x41, %0" : : "m"(*m) : "cc"); FLAGS(r);                    \
        __asm__ volatile("and" suf " $-17, %0" : "+m"(*m) : : "cc"); FLAGSM(r, 0x8c5);                    \
        __asm__ volatile("inc" suf " %0" : "+m"(*m) : : "cc"); FLAGS(r);                          \
        __asm__ volatile("dec" suf " %0" : "+m"(*m) : : "cc"); FLAGS(r);                          \
        __asm__ volatile("neg" suf " %0" : "+m"(*m) : : "cc"); FLAGS(r);                          \
        __asm__ volatile("not" suf " %0" : "+m"(*m) : : "cc");                                    \
        __asm__ volatile("shl" suf " $3, %0" : "+m"(*m) : : "cc"); FLAGSM(r, 0xc5);                      \
        __asm__ volatile("shr" suf " %0" : "+m"(*m) : : "cc"); FLAGSM(r, 0x8c5);                          \
        __asm__ volatile("sar" suf " $2, %0" : "+m"(*m) : : "cc"); FLAGSM(r, 0xc5);                      \
        __asm__ volatile("shl" suf " %%cl, %0" : "+m"(*m) : "c"((uint8_t)(c >> 8)) : "cc"); FLAGSM(r, 0xc4); \
        __asm__ volatile("sar" suf " %%cl, %0" : "+m"(*m) : "c"((uint8_t)(c >> 16)) : "cc"); FLAGSM(r, 0xc4); \
        __asm__ volatile("mov" suf " %1, %0" : "=r"(w) : "m"(*m)); r = r * 7 + (uint64_t)w;       \
        __asm__ volatile("cmp" suf " %1, %2\n\tsetb %0" : "=m"(buf[(o + 40) & 63]) : "m"(*m), reg(v) : "cc"); \
        r = r * 5 + buf[(o + 40) & 63];                                                           \
        __asm__ volatile("mov" suf " %1, %0" : "=m"(*m) : reg((type)(v * 3)));                    \
        r = r * 3 + (uint64_t)*m;                                                                 \
    } while (0)
T uint64_t t_memops(uint64_t a, uint64_t b, uint64_t c) {
    unsigned char buf[64 + 16] __attribute__((aligned(16)));
    for (int k = 0; k < 80; ++k) buf[k] = (unsigned char)((a >> (k & 63)) * 13 + (uint64_t)k);
    const unsigned o = (unsigned)(c & 31) | ((c >> 5) & 1 ? 8 + 4 + 2 + 1 : 0); /* 15 crosses 16 */
    uint64_t r = 0;
    MEM_OPS(uint8_t, "b", "q");
    MEM_OPS(uint16_t, "w", "r");
    MEM_OPS(uint32_t, "l", "r");
    MEM_OPS(uint64_t, "q", "r");
    uint64_t *m64 = (uint64_t *)(buf + o);
    uint32_t *m32 = (uint32_t *)(buf + o + 1);
    uint64_t x;
    uint32_t y;
    __asm__ volatile("movzbl %1, %k0" : "=r"(x) : "m"(buf[o + 2])); r = r * 3 + x;
    __asm__ volatile("movzwq %1, %0" : "=r"(x) : "m"(*(uint16_t *)(buf + o + 3))); r = r * 3 + x;
    __asm__ volatile("movsbq %1, %0" : "=r"(x) : "m"(buf[o + 4])); r = r * 3 + x;
    __asm__ volatile("movswl %1, %0" : "=r"(y) : "m"(*(uint16_t *)(buf + o + 5))); r = r * 3 + y;
    __asm__ volatile("movslq %1, %0" : "=r"(x) : "m"(*m32)); r = r * 3 + x;
    x = a;
    __asm__ volatile("cmp %2, %1\n\tcmovb %3, %0" : "+r"(x) : "r"(b), "r"(c), "m"(*m64) : "cc"); r = r * 3 + x;
    y = (uint32_t)a;
    __asm__ volatile("cmp %2, %1\n\tcmovae %3, %0" : "+r"(y) : "r"((uint32_t)b), "r"((uint32_t)c), "m"(*m32) : "cc"); r = r * 3 + y;
    x = b;
    __asm__ volatile("imulq %1, %0" : "+r"(x) : "m"(*m64) : "cc"); FLAGSM(r, 0x801); r = r * 3 + x;
    __asm__ volatile("imull $-77, %1, %0" : "=r"(y) : "m"(*m32) : "cc"); FLAGSM(r, 0x801); r = r * 3 + y;
    __asm__ volatile("pushq %1\n\tpopq %0" : "=r"(x) : "m"(*m64)); r = r * 3 + x;
    __asm__ volatile("leaq 0x1234(%1,%2,4), %0" : "=r"(x) : "r"(a), "r"(b)); r = r * 3 + x;
    __asm__ volatile("leal -8(%1,%2,8), %k0" : "=r"(x) : "r"(a), "r"(c)); r = r * 3 + x;
    __asm__ volatile("movq $-5, %0" : "=m"(*m64)); r = r * 3 + *m64;
    __asm__ volatile("movl $0x89abcdef, %0" : "=m"(*m32)); r = r * 3 + *m32;
    for (int k = 0; k < 80; k += 8) { uint64_t v = 0; for (int j = 0; j < 8; ++j) v = v << 8 | buf[k + j]; r = r * 31 + v; }
    return r;
}

/* Truncating conversions at the edges of the integer ranges (x86 gives the most negative value
 * for NaN and out of range), to 32 and 64 bits, from double and float: a: an edge or c's bits. */
T uint64_t t_convert(uint64_t a, uint64_t b, uint64_t c) {
    static const double edges[] = {
        2147483647.0, 2147483647.5, 2147483647.999, 2147483648.0, 2147483520.0, -2147483648.0,
        -2147483648.5, -2147483649.0, 9223372036854774784.0, 9223372036854775808.0,
        9223371487098961920.0, -9223372036854775808.0, -9223372036854777856.0, 0.5, -0.5, 1e300,
        -1e300, __builtin_inf(), -__builtin_inf(), __builtin_nan("")};
    union { uint64_t u; double d; } bits = {.u = c};
    union { uint32_t u; float f; } fbits = {.u = (uint32_t)b};
    const double d = a & 1 ? bits.d : edges[(a >> 1) % (sizeof(edges) / sizeof(*edges))];
    const float f = a & 2 ? fbits.f : (float)d;
    uint64_t r = (uint32_t)_mm_cvttsd_si32(_mm_set_sd(d));
    r = r * 31 + (uint64_t)_mm_cvttsd_si64(_mm_set_sd(d));
    r = r * 31 + (uint32_t)_mm_cvtt_ss2si(_mm_set_ss(f));
    return r * 31 + (uint64_t)_mm_cvttss_si64(_mm_set_ss(f));
}

typedef uint64_t (*Test)(uint64_t, uint64_t, uint64_t);
const struct { const char *name; Test fn; } tests[] = {
    {"arith", t_arith}, {"compare", t_compare}, {"misc", t_misc}, {"control", t_control},
    {"vector", t_vector}, {"float", t_float}, {"string", t_string}, {"memops", t_memops}, {"mxcsr", t_mxcsr}, {"convert", t_convert},
};

static uint64_t rng = 0x123456789abcdef1ull;
static uint64_t next(void) { rng ^= rng << 13; rng ^= rng >> 7; rng ^= rng << 17; return rng; }

/* Expected results: "<test> <a> <b> <c> <result>" per line. */
int main(void) {
    for (unsigned t = 0; t < sizeof(tests) / sizeof(*tests); ++t)
        for (int round = 0; round < 500; ++round) {
            uint64_t a = next(), b = next(), c = next();
            if (round % 4 == 1) b &= 0xff;
            if (round % 4 == 2) a = 0;
            if (round % 8 == 3) b = a;
            printf("%s %llx %llx %llx %llx\n", tests[t].name, (unsigned long long)a, (unsigned long long)b,
                   (unsigned long long)c, (unsigned long long)tests[t].fn(a, b, c));
        }
    return 0;
}
