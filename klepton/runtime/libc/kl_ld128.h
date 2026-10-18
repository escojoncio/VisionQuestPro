// IEEE binary128 <-> binary64, on raw (lo, hi) halves.
//
// bionic/arm64 `long double` is binary128, passed and returned in a Q register;
// Darwin's is a plain double. A guest's *l math call therefore cannot be
// forwarded (the double would be read from the low half of the quad, i.e. from
// mantissa bits). kl_bionic_extra.c declares the argument as a 128-bit vector,
// which AAPCS64 passes in the same register, converts here, calls the double
// routine and widens the result.
//
// Narrowing is round-to-nearest-even; values beyond double's range become
// infinity or zero. That precision loss is inherent to computing on Darwin and
// is the only divergence from bionic.
#ifndef KL_LD128_H
#define KL_LD128_H
#include <stdint.h>
#include <string.h>

typedef struct { uint64_t lo, hi; } kl_q;

static inline kl_q kl_q_from_double(double d) {
    uint64_t b; memcpy(&b, &d, 8);
    uint64_t sign = b >> 63;
    int64_t exp = (int64_t)((b >> 52) & 0x7FF);
    uint64_t man = b & 0xFFFFFFFFFFFFFull;
    kl_q q;
    if (exp == 0x7FF) {                        // inf / NaN (payload kept, quiet bit set)
        q.hi = (sign << 63) | (0x7FFFull << 48) | (man >> 4);
        q.lo = man << 60;
        if (man) q.hi |= 1ull << 47;
        return q;
    }
    if (exp == 0) {
        if (man == 0) { q.hi = sign << 63; q.lo = 0; return q; }
        // subnormal double: normalise, it is a normal number in binary128
        int sh = __builtin_clzll(man) - 11;    // bring the leading 1 to bit 52
        man <<= sh;
        exp = 1 - sh;
        man &= 0xFFFFFFFFFFFFFull;
    }
    int64_t e = exp - 1023 + 16383;
    q.hi = (sign << 63) | ((uint64_t)e << 48) | (man >> 4);
    q.lo = man << 60;
    return q;
}

static inline double kl_q_to_double(kl_q q) {
    uint64_t sign = q.hi >> 63;
    int64_t e = (int64_t)((q.hi >> 48) & 0x7FFF);
    uint64_t mh = q.hi & 0xFFFFFFFFFFFFull;     // top 48 fraction bits
    uint64_t out;
    if (e == 0x7FFF) {
        if (mh | q.lo)                          // NaN: top payload bits, forced quiet
            out = (sign << 63) | (0x7FFull << 52) | (1ull << 51) | ((mh << 4) | (q.lo >> 60));
        else out = (sign << 63) | (0x7FFull << 52);
        double d; memcpy(&d, &out, 8); return d;
    }
    if (e == 0) { out = sign << 63; double d; memcpy(&d, &out, 8); return d; }   // zero / tiny
    int64_t exp = e - 16383;
    if (exp > 1023) { out = (sign << 63) | (0x7FFull << 52); double d; memcpy(&d, &out, 8); return d; }
    // 53-bit significand with the 60 bits below it as the rounding remainder
    uint64_t frac52 = (mh << 4) | (q.lo >> 60);
    uint64_t rem = q.lo & ((1ull << 60) - 1);
    uint64_t sig = (1ull << 52) | frac52;
    if (exp >= -1022) {
        const uint64_t half = 1ull << 59;
        if (rem > half || (rem == half && (sig & 1))) sig++;
        if (sig >> 53) { sig >>= 1; exp++; }
        if (exp > 1023) { out = (sign << 63) | (0x7FFull << 52); }
        else out = (sign << 63) | ((uint64_t)(exp + 1023) << 52) | (sig & 0xFFFFFFFFFFFFFull);
    } else {
        int64_t shift = -1022 - exp;            // >= 1
        if (shift > 54) { out = sign << 63; }
        else {
            unsigned __int128 full = ((unsigned __int128)sig << 60) | rem;
            int total = 60 + (int)shift;
            unsigned __int128 res = full >> total;
            unsigned __int128 low = full & (((unsigned __int128)1 << total) - 1);
            unsigned __int128 half = (unsigned __int128)1 << (total - 1);
            if (low > half || (low == half && (res & 1))) res++;
            out = (sign << 63) | (uint64_t)res;     // may carry into the smallest normal: still correct
        }
    }
    double d; memcpy(&d, &out, 8); return d;
}
#endif
