/*
 * Copyright (c) 2015-2016, Intel Corporation
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 *  * Redistributions of source code must retain the above copyright notice,
 *    this list of conditions and the following disclaimer.
 *  * Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *  * Neither the name of Intel Corporation nor the names of its contributors
 *    may be used to endorse or promote products derived from this software
 *    without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 */

/** \file
 * \brief SIMD types and primitive operations.
 */

#ifndef SIMD_UTILS
#define SIMD_UTILS

#if !defined(_WIN32) && !defined(__SSSE3__)
#error SSSE3 instructions must be enabled
#endif

#include "config.h"
#include <string.h> // for memcpy

// more recent headers are bestest, but only if we can use them
#ifdef __cplusplus
# if defined(HAVE_CXX_X86INTRIN_H)
#  define USE_X86INTRIN_H
# endif
#else // C
# if defined(HAVE_C_X86INTRIN_H)
#  define USE_X86INTRIN_H
# endif
#endif

#ifdef __cplusplus
# if defined(HAVE_CXX_INTRIN_H)
#  define USE_INTRIN_H
# endif
#else // C
# if defined(HAVE_C_INTRIN_H)
#  define USE_INTRIN_H
# endif
#endif

#if defined(USE_X86INTRIN_H)
#include <x86intrin.h>
#elif defined(USE_INTRIN_H)
#include <intrin.h>
#else
#error no intrins!
#endif

#include "ue2common.h"
#include "simd_types.h"

// Define a common assume_aligned using an appropriate compiler built-in, if
// it's available. Note that we need to handle C or C++ compilation.
#ifdef __cplusplus
#  ifdef HAVE_CXX_BUILTIN_ASSUME_ALIGNED
#    define assume_aligned(x, y) __builtin_assume_aligned((x), (y))
#  endif
#else
#  ifdef HAVE_CC_BUILTIN_ASSUME_ALIGNED
#    define assume_aligned(x, y) __builtin_assume_aligned((x), (y))
#  endif
#endif

// Fallback to identity case.
#ifndef assume_aligned
#define assume_aligned(x, y) (x)
#endif

#ifdef __cplusplus
extern "C" {
#endif
extern const char vbs_mask_data[];
#ifdef __cplusplus
}
#endif

static really_inline m128 ones128(void) {
#if defined(__GNUC__) || defined(__INTEL_COMPILER)
    /* gcc gets this right */
    return _mm_set1_epi8(0xFF);
#else
    /* trick from Intel's optimization guide to generate all-ones.
     * ICC converts this to the single cmpeq instruction */
    return _mm_cmpeq_epi8(_mm_setzero_si128(), _mm_setzero_si128());
#endif
}

static really_inline m128 zeroes128(void) {
    return _mm_setzero_si128();
}

/** \brief Bitwise not for m128*/
static really_inline m128 not128(m128 a) {
    return _mm_xor_si128(a, ones128());
}

/** \brief Return 1 if a and b are different otherwise 0 */
static really_inline int diff128(m128 a, m128 b) {
    return (_mm_movemask_epi8(_mm_cmpeq_epi8(a, b)) ^ 0xffff);
}

static really_inline int isnonzero128(m128 a) {
    return !!diff128(a, zeroes128());
}

/**
 * "Rich" version of diff128(). Takes two vectors a and b and returns a 4-bit
 * mask indicating which 32-bit words contain differences.
 */
static really_inline u32 diffrich128(m128 a, m128 b) {
    a = _mm_cmpeq_epi32(a, b);
    return ~(_mm_movemask_ps(_mm_castsi128_ps(a))) & 0xf;
}

/**
 * "Rich" version of diff128(), 64-bit variant. Takes two vectors a and b and
 * returns a 4-bit mask indicating which 64-bit words contain differences.
 */
static really_inline u32 diffrich64_128(m128 a, m128 b) {
#if defined(__SSE_41__)
    a = _mm_cmpeq_epi64(a, b);
    return ~(_mm_movemask_ps(_mm_castsi128_ps(a))) & 0x5;
#else
    u32 d = diffrich128(a, b);
    return (d | (d >> 1)) & 0x5;
#endif
}

#define lshift64_m128(a, b) _mm_slli_epi64((a), (b))
#define rshift64_m128(a, b) _mm_srli_epi64((a), (b))
#define eq128(a, b)      _mm_cmpeq_epi8((a), (b))
#define movemask128(a)  ((u32)_mm_movemask_epi8((a)))

static really_inline m128 set16x8(u8 c) {
    return _mm_set1_epi8(c);
}

static really_inline u32 movd(const m128 in) {
    return _mm_cvtsi128_si32(in);
}

static really_inline u64a movq(const m128 in) {
#if defined(ARCH_X86_64)
    return _mm_cvtsi128_si64(in);
#else // 32-bit - this is horrific
    u32 lo = movd(in);
    u32 hi = movd(_mm_srli_epi64(in, 32));
    return (u64a)hi << 32 | lo;
#endif
}

#define rshiftbyte_m128(a, count_immed) _mm_srli_si128(a, count_immed)
#define lshiftbyte_m128(a, count_immed) _mm_slli_si128(a, count_immed)

#if !defined(__AVX2__)
// TODO: this entire file needs restructuring - this carveout is awful
#define extractlow64from256(a) movq(a.lo)
#define extractlow32from256(a) movd(a.lo)
#if defined(__SSE4_1__)
#define extract32from256(a, imm) _mm_extract_epi32((imm >> 2) ? a.hi : a.lo, imm % 4)
#define extract64from256(a, imm) _mm_extract_epi64((imm >> 2) ? a.hi : a.lo, imm % 2)
#else
#define extract32from256(a, imm) movd(_mm_srli_si128((imm >> 2) ? a.hi : a.lo, (imm % 4) * 8))
#define extract64from256(a, imm) movq(_mm_srli_si128((imm >> 2) ? a.hi : a.lo, (imm % 2) * 8))
#endif

#endif // !AVX2

static really_inline m128 and128(m128 a, m128 b) {
    return _mm_and_si128(a,b);
}

static really_inline m128 xor128(m128 a, m128 b) {
    return _mm_xor_si128(a,b);
}

static really_inline m128 or128(m128 a, m128 b) {
    return _mm_or_si128(a,b);
}

static really_inline m128 andnot128(m128 a, m128 b) {
    return _mm_andnot_si128(a, b);
}

// aligned load
static really_inline m128 load128(const void *ptr) {
    assert(ISALIGNED_N(ptr, alignof(m128)));
    ptr = assume_aligned(ptr, 16);
    return _mm_load_si128((const m128 *)ptr);
}

// aligned store
static really_inline void store128(void *ptr, m128 a) {
    assert(ISALIGNED_N(ptr, alignof(m128)));
    ptr = assume_aligned(ptr, 16);
    *(m128 *)ptr = a;
}

// unaligned load
static really_inline m128 loadu128(const void *ptr) {
    return _mm_loadu_si128((const m128 *)ptr);
}

// unaligned store
static really_inline void storeu128(void *ptr, m128 a) {
    _mm_storeu_si128 ((m128 *)ptr, a);
}

// packed unaligned store of first N bytes
static really_inline
void storebytes128(void *ptr, m128 a, unsigned int n) {
    assert(n <= sizeof(a));
    memcpy(ptr, &a, n);
}

// packed unaligned load of first N bytes, pad with zero
static really_inline
m128 loadbytes128(const void *ptr, unsigned int n) {
    m128 a = zeroes128();
    assert(n <= sizeof(a));
    memcpy(&a, ptr, n);
    return a;
}

// switches on bit N in the given vector.
static really_inline
void setbit128(m128 *ptr, unsigned int n) {
    assert(n < sizeof(*ptr) * 8);
    // We should be able to figure out a better way than this.
    union {
        m128 simd;
        u8 bytes[sizeof(m128)];
    } x;
    x.simd = *ptr;

    u8 *b = &x.bytes[n / 8];
    *b |= 1U << (n % 8);

    *ptr = x.simd;
}

// switches off bit N in the given vector.
static really_inline
void clearbit128(m128 *ptr, unsigned int n) {
    assert(n < sizeof(*ptr) * 8);
    // We should be able to figure out a better way than this.
    union {
        m128 simd;
        u8 bytes[sizeof(m128)];
    } x;
    x.simd = *ptr;

    u8 *b = &x.bytes[n / 8];
    *b &= ~(1U << (n % 8));

    *ptr = x.simd;
}

// tests bit N in the given vector.
static really_inline
char testbit128(const m128 *ptr, unsigned int n) {
    assert(n < sizeof(*ptr) * 8);
    // We should be able to figure out a better way than this.
    const char *bytes = (const char *)ptr;
    return !!(bytes[n / 8] & (1 << (n % 8)));
}

// offset must be an immediate
#define palignr(r, l, offset) _mm_alignr_epi8(r, l, offset)

static really_inline
m128 pshufb(m128 a, m128 b) {
    m128 result;
    result = _mm_shuffle_epi8(a, b);
    return result;
}

static really_inline
m256 vpshufb(m256 a, m256 b) {
#if defined(__AVX2__)
    return _mm256_shuffle_epi8(a, b);
#else
    m256 rv;
    rv.lo = pshufb(a.lo, b.lo);
    rv.hi = pshufb(a.hi, b.hi);
    return rv;
#endif
}

static really_inline
m128 variable_byte_shift_m128(m128 in, s32 amount) {
    assert(amount >= -16 && amount <= 16);
    m128 shift_mask = loadu128(vbs_mask_data + 16 - amount);
    return pshufb(in, shift_mask);
}


/****
 **** 256-bit Primitives
 ****/

#if defined(__AVX2__)
#define lshift64_m256(a, b) _mm256_slli_epi64((a), (b))
#define rshift64_m256(a, b) _mm256_srli_epi64((a), (b))

static really_inline
m256 set32x8(u32 in) {
    return _mm256_set1_epi8(in);
}

#define eq256(a, b)     _mm256_cmpeq_epi8((a), (b))
#define movemask256(a)  ((u32)_mm256_movemask_epi8((a)))

static really_inline
m256 set2x128(m128 a) {
    return _mm256_broadcastsi128_si256(a);
}

#else

static really_inline
m256 lshift64_m256(m256 a, int b) {
    m256 rv = a;
    rv.lo = lshift64_m128(rv.lo, b);
    rv.hi = lshift64_m128(rv.hi, b);
    return rv;
}

static really_inline
m256 rshift64_m256(m256 a, int b) {
    m256 rv = a;
    rv.lo = rshift64_m128(rv.lo, b);
    rv.hi = rshift64_m128(rv.hi, b);
    return rv;
}
static really_inline
m256 set32x8(u32 in) {
    m256 rv;
    rv.lo = set16x8((u8) in);
    rv.hi = rv.lo;
    return rv;
}

#endif

static really_inline m256 zeroes256(void) {
#if defined(__AVX2__)
    return _mm256_setzero_si256();
#else
    m256 rv = {zeroes128(), zeroes128()};
    return rv;
#endif
}

static really_inline m256 ones256(void) {
#if defined(__AVX2__)
    m256 rv = _mm256_set1_epi8(0xFF);
#else
    m256 rv = {ones128(), ones128()};
#endif
    return rv;
}

#if defined(__AVX2__)
static really_inline m256 and256(m256 a, m256 b) {
    return _mm256_and_si256(a, b);
}
#else
static really_inline m256 and256(m256 a, m256 b) {
    m256 rv;
    rv.lo = and128(a.lo, b.lo);
    rv.hi = and128(a.hi, b.hi);
    return rv;
}
#endif

#if defined(__AVX2__)
static really_inline m256 or256(m256 a, m256 b) {
    return _mm256_or_si256(a, b);
}
#else
static really_inline m256 or256(m256 a, m256 b) {
    m256 rv;
    rv.lo = or128(a.lo, b.lo);
    rv.hi = or128(a.hi, b.hi);
    return rv;
}
#endif

#if defined(__AVX2__)
static really_inline m256 xor256(m256 a, m256 b) {
    return _mm256_xor_si256(a, b);
}
#else
static really_inline m256 xor256(m256 a, m256 b) {
    m256 rv;
    rv.lo = xor128(a.lo, b.lo);
    rv.hi = xor128(a.hi, b.hi);
    return rv;
}
#endif

#if defined(__AVX2__)
static really_inline m256 not256(m256 a) {
    return _mm256_xor_si256(a, ones256());
}
#else
static really_inline m256 not256(m256 a) {
    m256 rv;
    rv.lo = not128(a.lo);
    rv.hi = not128(a.hi);
    return rv;
}
#endif

#if defined(__AVX2__)
static really_inline m256 andnot256(m256 a, m256 b) {
    return _mm256_andnot_si256(a, b);
}
#else
static really_inline m256 andnot256(m256 a, m256 b) {
    m256 rv;
    rv.lo = andnot128(a.lo, b.lo);
    rv.hi = andnot128(a.hi, b.hi);
    return rv;
}
#endif

static really_inline int diff256(m256 a, m256 b) {
#if defined(__AVX2__)
    return !!(_mm256_movemask_epi8(_mm256_cmpeq_epi8(a, b)) ^ (int)-1);
#else
    return diff128(a.lo, b.lo) || diff128(a.hi, b.hi);
#endif
}

static really_inline int isnonzero256(m256 a) {
#if defined(__AVX2__)
    return !!diff256(a, zeroes256());
#else
    return isnonzero128(or128(a.lo, a.hi));
#endif
}

/**
 * "Rich" version of diff256(). Takes two vectors a and b and returns an 8-bit
 * mask indicating which 32-bit words contain differences.
 */
static really_inline u32 diffrich256(m256 a, m256 b) {
#if defined(__AVX2__)
    a = _mm256_cmpeq_epi32(a, b);
    return ~(_mm256_movemask_ps(_mm256_castsi256_ps(a))) & 0xFF;
#else
    m128 z = zeroes128();
    a.lo = _mm_cmpeq_epi32(a.lo, b.lo);
    a.hi = _mm_cmpeq_epi32(a.hi, b.hi);
    m128 packed = _mm_packs_epi16(_mm_packs_epi32(a.lo, a.hi), z);
    return ~(_mm_movemask_epi8(packed)) & 0xff;
#endif
}

/**
 * "Rich" version of diff256(), 64-bit variant. Takes two vectors a and b and
 * returns an 8-bit mask indicating which 64-bit words contain differences.
 */
static really_inline u32 diffrich64_256(m256 a, m256 b) {
    u32 d = diffrich256(a, b);
    return (d | (d >> 1)) & 0x55555555;
}

// aligned load
static really_inline m256 load256(const void *ptr) {
#if defined(__AVX2__)
    assert(ISALIGNED_N(ptr, alignof(m256)));
    return _mm256_load_si256((const m256 *)ptr);
#else
    assert(ISALIGNED_N(ptr, alignof(m128)));
    m256 rv = { load128(ptr), load128((const char *)ptr + 16) };
    return rv;
#endif
}

// aligned load  of 128-bit value to low and high part of 256-bit value
static really_inline m256 load2x128(const void *ptr) {
#if defined(__AVX2__)
    return set2x128(load128(ptr));
#else
    assert(ISALIGNED_N(ptr, alignof(m128)));
    m256 rv;
    rv.hi = rv.lo = load128(ptr);
    return rv;
#endif
}

// aligned store
static really_inline void store256(void *ptr, m256 a) {
#if defined(__AVX2__)
    assert(ISALIGNED_N(ptr, alignof(m256)));
    _mm256_store_si256((m256 *)ptr, a);
#else
    assert(ISALIGNED_16(ptr));
    ptr = assume_aligned(ptr, 16);
    *(m256 *)ptr = a;
#endif
}

// unaligned load
static really_inline m256 loadu256(const void *ptr) {
#if defined(__AVX2__)
    return _mm256_loadu_si256((const m256 *)ptr);
#else
    m256 rv = { loadu128(ptr), loadu128((const char *)ptr + 16) };
    return rv;
#endif
}

// packed unaligned store of first N bytes
static really_inline
void storebytes256(void *ptr, m256 a, unsigned int n) {
    assert(n <= sizeof(a));
    memcpy(ptr, &a, n);
}

// packed unaligned load of first N bytes, pad with zero
static really_inline
m256 loadbytes256(const void *ptr, unsigned int n) {
    m256 a = zeroes256();
    assert(n <= sizeof(a));
    memcpy(&a, ptr, n);
    return a;
}

#if !defined(__AVX2__)
// switches on bit N in the given vector.
static really_inline
void setbit256(m256 *ptr, unsigned int n) {
    assert(n < sizeof(*ptr) * 8);
    m128 *sub;
    if (n < 128) {
        sub = &ptr->lo;
    } else {
        sub = &ptr->hi;
        n -= 128;
    }
    setbit128(sub, n);
}

// switches off bit N in the given vector.
static really_inline
void clearbit256(m256 *ptr, unsigned int n) {
    assert(n < sizeof(*ptr) * 8);
    m128 *sub;
    if (n < 128) {
        sub = &ptr->lo;
    } else {
        sub = &ptr->hi;
        n -= 128;
    }
    clearbit128(sub, n);
}

// tests bit N in the given vector.
static really_inline
char testbit256(const m256 *ptr, unsigned int n) {
    assert(n < sizeof(*ptr) * 8);
    const m128 *sub;
    if (n < 128) {
        sub = &ptr->lo;
    } else {
        sub = &ptr->hi;
        n -= 128;
    }
    return testbit128(sub, n);
}

#else // AVX2

// switches on bit N in the given vector.
static really_inline
void setbit256(m256 *ptr, unsigned int n) {
    assert(n < sizeof(*ptr) * 8);
    // We should be able to figure out a better way than this.
    union {
        m256 simd;
        u8 bytes[sizeof(m256)];
    } x;
    x.simd = *ptr;

    u8 *b = &x.bytes[n / 8];
    *b |= 1U << (n % 8);

    *ptr = x.simd;
}

// TODO: can we do this better in avx-land?
static really_inline
void clearbit256(m256 *ptr, unsigned int n) {
    assert(n < sizeof(*ptr) * 8);
    union {
        m256 simd;
        u8 bytes[sizeof(m256)];
    } x;
    x.simd = *ptr;

    u8 *b = &x.bytes[n / 8];
    *b &= ~(1U << (n % 8));

    *ptr = x.simd;
}

// tests bit N in the given vector.
static really_inline
char testbit256(const m256 *ptr, unsigned int n) {
    assert(n < sizeof(*ptr) * 8);
    const char *bytes = (const char *)ptr;
    return !!(bytes[n / 8] & (1 << (n % 8)));
}

static really_really_inline
m128 movdq_hi(m256 x) {
    return _mm256_extracti128_si256(x, 1);
}

static really_really_inline
m128 movdq_lo(m256 x) {
    return _mm256_extracti128_si256(x, 0);
}

#define cast256to128(a) _mm256_castsi256_si128(a)
#define cast128to256(a) _mm256_castsi128_si256(a)
#define swap128in256(a) _mm256_permute4x64_epi64(a, 0x4E)
#define insert128to256(a, b, imm) _mm256_inserti128_si256(a, b, imm)
#define rshift128_m256(a, count_immed) _mm256_srli_si256(a, count_immed)
#define lshift128_m256(a, count_immed) _mm256_slli_si256(a, count_immed)
#define extract64from256(a, imm) _mm_extract_epi64(_mm256_extracti128_si256(a, imm >> 1), imm % 2)
#define extract32from256(a, imm) _mm_extract_epi32(_mm256_extracti128_si256(a, imm >> 2), imm % 4)
#define extractlow64from256(a) _mm_cvtsi128_si64(cast256to128(a))
#define extractlow32from256(a) movd(cast256to128(a))
#define interleave256hi(a, b) _mm256_unpackhi_epi8(a, b);
#define interleave256lo(a, b) _mm256_unpacklo_epi8(a, b);
#define vpalignr(r, l, offset) _mm256_alignr_epi8(r, l, offset)

#endif //AVX2

/****
 **** 384-bit Primitives
 ****/

static really_inline m384 and384(m384 a, m384 b) {
    m384 rv;
    rv.lo = and128(a.lo, b.lo);
    rv.mid = and128(a.mid, b.mid);
    rv.hi = and128(a.hi, b.hi);
    return rv;
}

static really_inline m384 or384(m384 a, m384 b) {
    m384 rv;
    rv.lo = or128(a.lo, b.lo);
    rv.mid = or128(a.mid, b.mid);
    rv.hi = or128(a.hi, b.hi);
    return rv;
}

static really_inline m384 xor384(m384 a, m384 b) {
    m384 rv;
    rv.lo = xor128(a.lo, b.lo);
    rv.mid = xor128(a.mid, b.mid);
    rv.hi = xor128(a.hi, b.hi);
    return rv;
}
static really_inline m384 not384(m384 a) {
    m384 rv;
    rv.lo = not128(a.lo);
    rv.mid = not128(a.mid);
    rv.hi = not128(a.hi);
    return rv;
}
static really_inline m384 andnot384(m384 a, m384 b) {
    m384 rv;
    rv.lo = andnot128(a.lo, b.lo);
    rv.mid = andnot128(a.mid, b.mid);
    rv.hi = andnot128(a.hi, b.hi);
    return rv;
}

// The shift amount is an immediate
static really_really_inline
m384 lshift64_m384(m384 a, unsigned b) {
    m384 rv;
    rv.lo = lshift64_m128(a.lo, b);
    rv.mid = lshift64_m128(a.mid, b);
    rv.hi = lshift64_m128(a.hi, b);
    return rv;
}

static really_inline m384 zeroes384(void) {
    m384 rv = {zeroes128(), zeroes128(), zeroes128()};
    return rv;
}

static really_inline m384 ones384(void) {
    m384 rv = {ones128(), ones128(), ones128()};
    return rv;
}

static really_inline int diff384(m384 a, m384 b) {
    return diff128(a.lo, b.lo) || diff128(a.mid, b.mid) || diff128(a.hi, b.hi);
}

static really_inline int isnonzero384(m384 a) {
    return isnonzero128(or128(or128(a.lo, a.mid), a.hi));
}

/**
 * "Rich" version of diff384(). Takes two vectors a and b and returns a 12-bit
 * mask indicating which 32-bit words contain differences.
 */
static really_inline u32 diffrich384(m384 a, m384 b) {
    m128 z = zeroes128();
    a.lo = _mm_cmpeq_epi32(a.lo, b.lo);
    a.mid = _mm_cmpeq_epi32(a.mid, b.mid);
    a.hi = _mm_cmpeq_epi32(a.hi, b.hi);
    m128 packed = _mm_packs_epi16(_mm_packs_epi32(a.lo, a.mid),
                                  _mm_packs_epi32(a.hi, z));
    return ~(_mm_movemask_epi8(packed)) & 0xfff;
}

/**
 * "Rich" version of diff384(), 64-bit variant. Takes two vectors a and b and
 * returns a 12-bit mask indicating which 64-bit words contain differences.
 */
static really_inline u32 diffrich64_384(m384 a, m384 b) {
    u32 d = diffrich384(a, b);
    return (d | (d >> 1)) & 0x55555555;
}

// aligned load
static really_inline m384 load384(const void *ptr) {
    assert(ISALIGNED_16(ptr));
    m384 rv = { load128(ptr), load128((const char *)ptr + 16),
                load128((const char *)ptr + 32) };
    return rv;
}

// aligned store
static really_inline void store384(void *ptr, m384 a) {
    assert(ISALIGNED_16(ptr));
    ptr = assume_aligned(ptr, 16);
    *(m384 *)ptr = a;
}

// unaligned load
static really_inline m384 loadu384(const void *ptr) {
    m384 rv = { loadu128(ptr), loadu128((const char *)ptr + 16),
                loadu128((const char *)ptr + 32)};
    return rv;
}

// packed unaligned store of first N bytes
static really_inline
void storebytes384(void *ptr, m384 a, unsigned int n) {
    assert(n <= sizeof(a));
    memcpy(ptr, &a, n);
}

// packed unaligned load of first N bytes, pad with zero
static really_inline
m384 loadbytes384(const void *ptr, unsigned int n) {
    m384 a = zeroes384();
    assert(n <= sizeof(a));
    memcpy(&a, ptr, n);
    return a;
}

// switches on bit N in the given vector.
static really_inline
void setbit384(m384 *ptr, unsigned int n) {
    assert(n < sizeof(*ptr) * 8);
    m128 *sub;
    if (n < 128) {
        sub = &ptr->lo;
    } else if (n < 256) {
        sub = &ptr->mid;
    } else {
        sub = &ptr->hi;
    }
    setbit128(sub, n % 128);
}

// switches off bit N in the given vector.
static really_inline
void clearbit384(m384 *ptr, unsigned int n) {
    assert(n < sizeof(*ptr) * 8);
    m128 *sub;
    if (n < 128) {
        sub = &ptr->lo;
    } else if (n < 256) {
        sub = &ptr->mid;
    } else {
        sub = &ptr->hi;
    }
    clearbit128(sub, n % 128);
}

// tests bit N in the given vector.
static really_inline
char testbit384(const m384 *ptr, unsigned int n) {
    assert(n < sizeof(*ptr) * 8);
    const m128 *sub;
    if (n < 128) {
        sub = &ptr->lo;
    } else if (n < 256) {
        sub = &ptr->mid;
    } else {
        sub = &ptr->hi;
    }
    return testbit128(sub, n % 128);
}

/****
 **** 512-bit Primitives
 ****/

static really_inline m512 and512(m512 a, m512 b) {
    m512 rv;
    rv.lo = and256(a.lo, b.lo);
    rv.hi = and256(a.hi, b.hi);
    return rv;
}

static really_inline m512 or512(m512 a, m512 b) {
    m512 rv;
    rv.lo = or256(a.lo, b.lo);
    rv.hi = or256(a.hi, b.hi);
    return rv;
}

static really_inline m512 xor512(m512 a, m512 b) {
    m512 rv;
    rv.lo = xor256(a.lo, b.lo);
    rv.hi = xor256(a.hi, b.hi);
    return rv;
}

static really_inline m512 not512(m512 a) {
    m512 rv;
    rv.lo = not256(a.lo);
    rv.hi = not256(a.hi);
    return rv;
}

static really_inline m512 andnot512(m512 a, m512 b) {
    m512 rv;
    rv.lo = andnot256(a.lo, b.lo);
    rv.hi = andnot256(a.hi, b.hi);
    return rv;
}

// The shift amount is an immediate
static really_really_inline
m512 lshift64_m512(m512 a, unsigned b) {
    m512 rv;
    rv.lo = lshift64_m256(a.lo, b);
    rv.hi = lshift64_m256(a.hi, b);
    return rv;
}

static really_inline m512 zeroes512(void) {
    m512 rv = {zeroes256(), zeroes256()};
    return rv;
}

static really_inline m512 ones512(void) {
    m512 rv = {ones256(), ones256()};
    return rv;
}

static really_inline int diff512(m512 a, m512 b) {
    return diff256(a.lo, b.lo) || diff256(a.hi, b.hi);
}

static really_inline int isnonzero512(m512 a) {
#if !defined(__AVX2__)
    m128 x = or128(a.lo.lo, a.lo.hi);
    m128 y = or128(a.hi.lo, a.hi.hi);
    return isnonzero128(or128(x, y));
#else
    m256 x = or256(a.lo, a.hi);
    return !!diff256(x, zeroes256());
#endif
}

/**
 * "Rich" version of diff512(). Takes two vectors a and b and returns a 16-bit
 * mask indicating which 32-bit words contain differences.
 */
static really_inline u32 diffrich512(m512 a, m512 b) {
#if defined(__AVX2__)
    return diffrich256(a.lo, b.lo) | (diffrich256(a.hi, b.hi) << 8);
#else
    a.lo.lo = _mm_cmpeq_epi32(a.lo.lo, b.lo.lo);
    a.lo.hi = _mm_cmpeq_epi32(a.lo.hi, b.lo.hi);
    a.hi.lo = _mm_cmpeq_epi32(a.hi.lo, b.hi.lo);
    a.hi.hi = _mm_cmpeq_epi32(a.hi.hi, b.hi.hi);
    m128 packed = _mm_packs_epi16(_mm_packs_epi32(a.lo.lo, a.lo.hi),
                                  _mm_packs_epi32(a.hi.lo, a.hi.hi));
    return ~(_mm_movemask_epi8(packed)) & 0xffff;
#endif
}

/**
 * "Rich" version of diffrich(), 64-bit variant. Takes two vectors a and b and
 * returns a 16-bit mask indicating which 64-bit words contain differences.
 */
static really_inline u32 diffrich64_512(m512 a, m512 b) {
    u32 d = diffrich512(a, b);
    return (d | (d >> 1)) & 0x55555555;
}

// aligned load
static really_inline m512 load512(const void *ptr) {
    assert(ISALIGNED_16(ptr));
    m512 rv = { load256(ptr), load256((const char *)ptr + 32) };
    return rv;
}

// aligned store
static really_inline void store512(void *ptr, m512 a) {
#if defined(__AVX2__)
    m512 *x = (m512 *)ptr;
    store256(&x->lo, a.lo);
    store256(&x->hi, a.hi);
#else
    assert(ISALIGNED_16(ptr));
    ptr = assume_aligned(ptr, 16);
    *(m512 *)ptr = a;
#endif
}

// unaligned load
static really_inline m512 loadu512(const void *ptr) {
    m512 rv = { loadu256(ptr), loadu256((const char *)ptr + 32) };
    return rv;
}

// packed unaligned store of first N bytes
static really_inline
void storebytes512(void *ptr, m512 a, unsigned int n) {
    assert(n <= sizeof(a));
    memcpy(ptr, &a, n);
}

// packed unaligned load of first N bytes, pad with zero
static really_inline
m512 loadbytes512(const void *ptr, unsigned int n) {
    m512 a = zeroes512();
    assert(n <= sizeof(a));
    memcpy(&a, ptr, n);
    return a;
}

// switches on bit N in the given vector.
static really_inline
void setbit512(m512 *ptr, unsigned int n) {
    assert(n < sizeof(*ptr) * 8);
#if !defined(__AVX2__)
    m128 *sub;
    if (n < 128) {
        sub = &ptr->lo.lo;
    } else if (n < 256) {
        sub = &ptr->lo.hi;
    } else if (n < 384) {
        sub = &ptr->hi.lo;
    } else {
        sub = &ptr->hi.hi;
    }
    setbit128(sub, n % 128);
#else
    m256 *sub;
    if (n < 256) {
        sub = &ptr->lo;
    } else {
        sub = &ptr->hi;
        n -= 256;
    }
    setbit256(sub, n);
#endif
}

// switches off bit N in the given vector.
static really_inline
void clearbit512(m512 *ptr, unsigned int n) {
    assert(n < sizeof(*ptr) * 8);
#if !defined(__AVX2__)
    m128 *sub;
    if (n < 128) {
        sub = &ptr->lo.lo;
    } else if (n < 256) {
        sub = &ptr->lo.hi;
    } else if (n < 384) {
        sub = &ptr->hi.lo;
    } else {
        sub = &ptr->hi.hi;
    }
    clearbit128(sub, n % 128);
#else
    m256 *sub;
    if (n < 256) {
        sub = &ptr->lo;
    } else {
        sub = &ptr->hi;
        n -= 256;
    }
    clearbit256(sub, n);
#endif
}

// tests bit N in the given vector.
static really_inline
char testbit512(const m512 *ptr, unsigned int n) {
    assert(n < sizeof(*ptr) * 8);
#if !defined(__AVX2__)
    const m128 *sub;
    if (n < 128) {
        sub = &ptr->lo.lo;
    } else if (n < 256) {
        sub = &ptr->lo.hi;
    } else if (n < 384) {
        sub = &ptr->hi.lo;
    } else {
        sub = &ptr->hi.hi;
    }
    return testbit128(sub, n % 128);
#else
    const m256 *sub;
    if (n < 256) {
        sub = &ptr->lo;
    } else {
        sub = &ptr->hi;
        n -= 256;
    }
    return testbit256(sub, n);
#endif
}

#endif
