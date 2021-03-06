/*
 * File:   zsnprintf.c
 * Author: Michael Sandstedt
 *
 * Created on January 1, 2017, 11:38 AM
 *
 * A feature-rich, reentrant, self-contained, high-performance snprintf.
 * Performance of up * 80x faster has been observed as compared to library
 * implementations on some microcontrollers in floating point output scenarios.
 *
 * Supported features:
 *
 * * %u, i, d, x, X, o, f, e, E, g, G, a, A, s, p format specifiers
 * * ll, l, h, hh, L, j, z, t length specifiers
 * * zero-padding and '+' format modifiers
 *
 * In addition, 32-bit doubles, which are rare but do exist, are properly
 * supported.  Microchip's XC16 compiler uses these by default.
 *
 * Limitations:
 *
 *    * C99 %a/%A format specifiers are interpreted as %e/%E
 *    * %f produces %e output for abs(float) > INT32_MAX
 *    * the '-' flag (left justify) is ignored
 *    * the '#' alternate form flag is ignored
 *    * %g/%G aren't guaranteed to produce the most compact output,
 *      and may be printed with trailing zeros
 *    * printed precision for 64 bit doubles can be less the full 53 bits of the
 *      mantissa; some usages will produce output precision limited to 32 bits
 *    * long doubles (%LF) are read correctly, but cast to double
 *    * floating point output is not 100% conformant to IEEE-754
 */

#include <float.h>
#include <limits.h>
#include <math.h>
#include <tgmath.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include "config.h"
#include "zsnprintf.h"

#define MAX_WIDTH_SUB_SPEC "0-+ #"MAX_DEC_FMT_I32"."MAX_DEC_FMT_I32"ll"
#define DTOCHAR(_d) ((_d) + '0')
#define ARG_SPECIFIED -2
#define PRECISION_UNSPECIFIED -1
#define DEFAULT_PRECISION 4
#define MAX_DEC_FMT_I32 "-2147483648"
#define MAX_DEC_FMT_I16 "-32767"

#if UINT_MAX == UINT16_MAX
#define zxtoa(_buf, _n, _width, _flags) (zx64toa(_buf, ZS16, _n, _width, _flags, false))
#define zXtoa(_buf, _n, _width, _flags) (zx64toa(_buf, ZS16, _n, _width, _flags, true))
#define zotoa(_buf, _n, _width, _flags) (zo64toa(_buf, ZS16, _n, _width, _flags))
#define zitoa zi16toa
#define zutoa zu16toa
#elif UINT_MAX == UINT32_MAX
#define zxtoa(_buf, _n, _width, _flags) (zx64toa(_buf, ZS32, _n, _width, _flags, false))
#define zXtoa(_buf, _n, _width, _flags) (zx64toa(_buf, ZS32, _n, _width, _flags, true))
#define zotoa(_buf, _n, _width, _flags) (zo64toa(_buf, ZS32, _n, _width, _flags))
#define zitoa zi32toa
#define zutoa zu32toa
#elif UINT_MAX == UINT64_MAX
#define zxtoa(_buf, _n, _width, _flags) (zx64toa(_buf, ZS64, _n, _width, _flags, false))
#define zXtoa(_buf, _n, _width, _flags) (zx64toa(_buf, ZS64, _n, _width, _flags, true))
#define zotoa(_buf, _n, _width, _flags) (zo64toa(_buf, ZS64, _n, _width, _flags))
#define zitoa zi64toa
#define zutoa zu64toa
#else
#error UINT_MAX unsupported
#endif

#if ULONG_MAX == UINT32_MAX
#define zlxtoa(_buf, _n, _width, _flags) (zx64toa(_buf, ZS32, _n, _width, _flags, false))
#define zlXtoa(_buf, _n, _width, _flags) (zx64toa(_buf, ZS32, _n, _width, _flags, true))
#define zlotoa(_buf, _n, _width, _flags) (zo64toa(_buf, ZS32, _n, _width, _flags))
#define zltoa zi32toa
#define zultoa zu32toa
#elif ULONG_MAX == UINT64_MAX
#define zlxtoa(_buf, _n, _width, _flags) (zx64toa(_buf, ZS64, _n, _width, _flags, false))
#define zlXtoa(_buf, _n, _width, _flags) (zx64toa(_buf, ZS64, _n, _width, _flags, true))
#define zlotoa(_buf, _n, _width, _flags) (zo64toa(_buf, ZS64, _n, _width, _flags))
#define zltoa zi64toa
#define zultoa zu64toa
#else
#error ULONG_MAX unsupported
#endif

#define zllxtoa(_buf, _n, _width, _flags) (zx64toa(_buf, ZS64, _n, _width, _flags, false))
#define zllXtoa(_buf, _n, _width, _flags) (zx64toa(_buf, ZS64, _n, _width, _flags, true))
#define zllotoa(_buf, _n, _width, _flags) (zo64toa(_buf, ZS64, _n, _width, _flags))
#define zlltoa zi64toa
#define zulltoa zu64toa

#ifdef __GNUC__

#if (__DBL_MANT_DIG__ == __FLT_MANT_DIG__)
#define zftoa zftoaf // 32-bit doubles
#else
#define zftoa zftoal
#endif

#else

#if defined(DBL_MANT_DIG) && defined(FLT_MANT_DIG) && (DBL_MANT_DIG == FLT_MANT_DIG)
#define zftoa zftoaf // 32-bit doubles
#else
#define zftoa zftoal
#endif

#endif

#define GMINF 0.0001 // min for %f output style when %g specified
#define GMAXF 999999.9 // max for %f output style when %g specified

typedef enum sign_e {
    auto_sign,
    always_sign,
    sign_or_space,
} sign_t;

typedef enum exp_form_e {
    exp_none,
    exp_e,
    exp_E,
} exp_form_t;

typedef struct flags_s {
    unsigned leftAlign : 1;
    unsigned sign : 2;
    unsigned altForm : 1;
    unsigned zeropad : 1;
    unsigned exp : 2;
} fmt_flags_t;

typedef enum int_size_e {
    ZS64,
    ZS32,
    ZS16,
} int_size_t;

static char xTOCHAR(uint8_t x)
{
    return x >= 0xA ? x - 0xA + 'a' : DTOCHAR(x);
}

static char XTOCHAR(uint8_t x)
{
    return x >= 0xA ? x - 0xA + 'A' : DTOCHAR(x);
}

static char *zx64toa(char *buf, int_size_t size, uint64_t n, unsigned width, fmt_flags_t flags, bool upper)
{
    uint8_t d15=0, d14=0, d13=0, d12=0, d11=0, d10=0, d9=0, d8=0, d7=0, d6=0, d5=0, d4=0, d3=0, d2=0, d1=0, d0=0;
    unsigned first_digit = 0;
    char (*tochar)(uint8_t) = upper ? &XTOCHAR : &xTOCHAR;

    d0 = n & 0xF;
    d1 = (n >> 4) & 0xF; if (d1) { first_digit = 1; }
    d2 = (n >> 8) & 0xF; if (d2) { first_digit = 2; }
    d3 = (n >> 12) & 0xF; if (d3) { first_digit = 3; }
    if (size >= ZS32) {
        d4 = (n >> 16) & 0xF; if (d4) { first_digit = 4; }
        d5 = (n >> 20) & 0xF; if (d5) { first_digit = 5; }
        d6 = (n >> 24) & 0xF; if (d6) { first_digit = 6; }
        d7 = (n >> 28) & 0xF; if (d7) { first_digit = 7; }
    }
    if (size >= ZS64) {
        d8 = (n >> 32) & 0xF; if (d8) { first_digit = 8; }
        d9 = (n >> 36) & 0xF; if (d9) { first_digit = 9; }
        d10 = (n >> 40) & 0xF; if (d10) { first_digit = 10; }
        d11 = (n >> 44) & 0xF; if (d11) { first_digit = 11; }
        d12 = (n >> 48) & 0xF; if (d12) { first_digit = 12; }
        d13 = (n >> 52) & 0xF; if (d13) { first_digit = 13; }
        d14 = (n >> 56) & 0xF; if (d14) { first_digit = 14; }
        d15 = (n >> 60) & 0xF; if (d15) { first_digit = 15; }
    }

    if (width) {
        // width is 1-based; change to 0-based
        if (width > 15) {
            width = 15;
        } else {
            --width;
        }
        if (flags.zeropad) {
            for (unsigned i = width; i > first_digit; --i) {
                *buf = '0';
                ++buf;
            }
        } else {
            for (unsigned i = width; i > first_digit; --i) {
                *buf = ' ';
                ++buf;
            }
        }
    }

    switch (first_digit) {
        case 15: *buf = (*tochar)(d15); ++buf;
        case 14: *buf = (*tochar)(d14); ++buf;
        case 13: *buf = (*tochar)(d13); ++buf;
        case 12: *buf = (*tochar)(d12); ++buf;
        case 11: *buf = (*tochar)(d11); ++buf;
        case 10: *buf = (*tochar)(d10); ++buf;
        case 9: *buf = (*tochar)(d9); ++buf;
        case 8: *buf = (*tochar)(d8); ++buf;
        case 7: *buf = (*tochar)(d7); ++buf;
        case 6: *buf = (*tochar)(d6); ++buf;
        case 5: *buf = (*tochar)(d5); ++buf;
        case 4: *buf = (*tochar)(d4); ++buf;
        case 3: *buf = (*tochar)(d3); ++buf;
        case 2: *buf = (*tochar)(d2); ++buf;
        case 1: *buf = (*tochar)(d1); ++buf;
    }
    *buf = (*tochar)(d0); ++buf;
    *buf = '\0';
    return buf;
}

static char *zo64toa(char *buf, int_size_t size, uint64_t n, unsigned width, fmt_flags_t flags)
{
    uint8_t d21=0, d20=0, d19=0, d18=0, d17=0, d16=0, d15=0, d14=0, d13=0, d12=0, d11=0, d10=0, d9=0, d8=0, d7=0, d6=0, d5=0, d4=0, d3=0, d2=0, d1=0, d0=0;
    unsigned first_digit = 0;

    d0 = n & 0x7;
    d1 = (n >> 3) & 0x7; if (d1) { first_digit = 1; }
    d2 = (n >> 6) & 0x7; if (d2) { first_digit = 2; }
    d3 = (n >> 9) & 0x7; if (d3) { first_digit = 3; }
    d4 = (n >> 12) & 0x7; if (d4) { first_digit = 4; }
    d5 = (n >> 15) & 0x7; if (d5) { first_digit = 5; }
    if (size >= ZS32) {
        d6 = (n >> 18) & 0x7; if (d6) { first_digit = 6; }
        d7 = (n >> 21) & 0x7; if (d7) { first_digit = 7; }
        d8 = (n >> 24) & 0x7; if (d8) { first_digit = 8; }
        d9 = (n >> 27) & 0x7; if (d9) { first_digit = 9; }
        d10 = (n >> 30) & 0x7; if (d10) { first_digit = 10; }
    }
    if (size >= ZS64) {
        d11 = (n >> 33) & 0x7; if (d11) { first_digit = 11; }
        d12 = (n >> 36) & 0x7; if (d12) { first_digit = 12; }
        d13 = (n >> 39) & 0x7; if (d13) { first_digit = 13; }
        d14 = (n >> 42) & 0x7; if (d14) { first_digit = 14; }
        d15 = (n >> 45) & 0x7; if (d15) { first_digit = 15; }
        d16 = (n >> 48) & 0x7; if (d16) { first_digit = 16; }
        d17 = (n >> 51) & 0x7; if (d17) { first_digit = 17; }
        d18 = (n >> 54) & 0x7; if (d18) { first_digit = 18; }
        d19 = (n >> 57) & 0x7; if (d19) { first_digit = 19; }
        d20 = (n >> 60) & 0x7; if (d20) { first_digit = 20; }
        d21 = (n >> 63) & 0x7; if (d21) { first_digit = 21; }
    }

    if (width) {
        // width is 1-based; change to 0-based
        if (width > 21) {
            width = 21;
        } else {
            --width;
        }
        if (flags.zeropad) {
            for (unsigned i = width; i > first_digit; --i) {
                *buf = '0';
                ++buf;
            }
        } else {
            for (unsigned i = width; i > first_digit; --i) {
                *buf = ' ';
                ++buf;
            }
        }
    }

    switch (first_digit) {
        case 21: *buf = XTOCHAR(d21); ++buf;
        case 20: *buf = XTOCHAR(d20); ++buf;
        case 19: *buf = XTOCHAR(d19); ++buf;
        case 18: *buf = XTOCHAR(d18); ++buf;
        case 17: *buf = XTOCHAR(d17); ++buf;
        case 16: *buf = XTOCHAR(d16); ++buf;
        case 15: *buf = XTOCHAR(d15); ++buf;
        case 14: *buf = XTOCHAR(d14); ++buf;
        case 13: *buf = XTOCHAR(d13); ++buf;
        case 12: *buf = XTOCHAR(d12); ++buf;
        case 11: *buf = XTOCHAR(d11); ++buf;
        case 10: *buf = XTOCHAR(d10); ++buf;
        case 9: *buf = XTOCHAR(d9); ++buf;
        case 8: *buf = XTOCHAR(d8); ++buf;
        case 7: *buf = XTOCHAR(d7); ++buf;
        case 6: *buf = XTOCHAR(d6); ++buf;
        case 5: *buf = XTOCHAR(d5); ++buf;
        case 4: *buf = XTOCHAR(d4); ++buf;
        case 3: *buf = XTOCHAR(d3); ++buf;
        case 2: *buf = XTOCHAR(d2); ++buf;
        case 1: *buf = XTOCHAR(d1); ++buf;
    }
    *buf = XTOCHAR(d0); ++buf;
    *buf = '\0';
    return buf;
}

#define addSign(_buf, _n, _flags) {\
    if (_n < 0) {\
        *_buf = '-';\
        ++_buf;\
    } else if (_flags.sign == always_sign) {\
        *_buf = '+';\
        ++_buf;\
    } else if (_flags.sign == sign_or_space) {\
        *_buf = ' ';\
        ++_buf;\
    }\
}

static char *zi16toa(char *buf, int16_t n, unsigned width, fmt_flags_t flags)
{
    uint8_t d4, d3, d2, d1, q; // yes, 8 bits are enough for these
    uint16_t d0;
    unsigned first_digit = 0;

    uint16_t absn = n < 0 ? -n : n;
    d0 = absn & 0xF;
    d1 = (absn >> 4) & 0xF;
    d2 = (absn >> 8) & 0xF;
    d3 = (absn >> 12) & 0xF;

    d0 = 6*(d3 + d2 + d1) + d0;
    q = d0 / 10;
    d0 = d0 % 10;

    d1 = q + 9*d3 + 5*d2 + d1;
    q = d1 / 10;
    d1 = d1 % 10;
    if (d1) { first_digit = 1; }

    d2 = q + 2*d2;
    q = d2 / 10;
    d2 = d2 % 10;
    if (d2) { first_digit = 2; }

    d3 = q + 4*d3;
    q = d3 / 10;
    d3 = d3 % 10;
    if (d3) { first_digit = 3; }

    d4 = q;
    if (d4) { first_digit = 4; }

    if (width) {
        // width is 1-based; change to 0-based
        if (width > 4) {
            width = 4;
        } else {
            --width;
        }
        if (flags.zeropad) {
            addSign(buf, n, flags);
            for (unsigned i = width; i > first_digit; --i) {
                *buf = '0';
                ++buf;
            }
        } else {
            for (unsigned i = width; i > first_digit; --i) {
                *buf = ' ';
                ++buf;
            }
            addSign(buf, n, flags);
        }
    } else {
        addSign(buf, n, flags);
    }

    switch (first_digit) {
        case 4: *buf = DTOCHAR(d4); ++buf;
        case 3: *buf = DTOCHAR(d3); ++buf;
        case 2: *buf = DTOCHAR(d2); ++buf;
        case 1: *buf = DTOCHAR(d1); ++buf;
    }
    *buf = DTOCHAR(d0); ++buf;
    *buf = '\0';
    return buf;
}

static char *zu16toa(char *buf, uint16_t n, unsigned width, fmt_flags_t flags)
{
    uint8_t d4, d3, d2, d1, q; // yes, 8 bits are enough for these
    uint16_t d0;
    unsigned first_digit = 0;

    d0 = n & 0xF;
    d1 = (n >> 4) & 0xF;
    d2 = (n >> 8) & 0xF;
    d3 = (n >> 12) & 0xF;

    d0 = 6*(d3 + d2 + d1) + d0;
    q = d0 / 10;
    d0 = d0 % 10;

    d1 = q + 9*d3 + 5*d2 + d1;
    q = d1 / 10;
    d1 = d1 % 10;
    if (d1) { first_digit = 1; }

    d2 = q + 2*d2;
    q = d2 / 10;
    d2 = d2 % 10;
    if (d2) { first_digit = 2; }

    d3 = q + 4*d3;
    q = d3 / 10;
    d3 = d3 % 10;
    if (d3) { first_digit = 3; }

    d4 = q;
    if (d4) { first_digit = 4; }

    if (width) {
        // width is 1-based; change to 0-based
        if (width > 4) {
            width = 4;
        } else {
            --width;
        }
        if (flags.zeropad) {
            for (unsigned i = width; i > first_digit; --i) {
                *buf = '0';
                ++buf;
            }
        } else {
            for (unsigned i = width; i > first_digit; --i) {
                *buf = ' ';
                ++buf;
            }
        }
    }

    switch (first_digit) {
        case 4: *buf = DTOCHAR(d4); ++buf;
        case 3: *buf = DTOCHAR(d3); ++buf;
        case 2: *buf = DTOCHAR(d2); ++buf;
        case 1: *buf = DTOCHAR(d1); ++buf;
    }
    *buf = DTOCHAR(d0); ++buf;
    *buf = '\0';
    return buf;
}

char *zi32toa(char *buf, int32_t n, unsigned width, fmt_flags_t flags)
{
    uint8_t n0, n1, n2, n3, n4, n5, n6, n7;
    uint8_t a8, a7, a6, a5, q; // yes, 8 bits are enough for these
    uint16_t a4, a3, a2, a1, a0;
    uint8_t d0, d1, d2, d3, d4, d5, d6, d7, d8, d9;
    unsigned first_digit = 0;

    uint32_t absn = n < 0 ? -n : n;
    n0 = absn & 0xF;
    n1 = (absn >> 4) & 0xF;
    n2 = (absn >> 8) & 0xF;
    n3 = (absn >> 12) & 0xF;
    n4 = (absn >> 16) & 0xF;
    n5 = (absn >> 20) & 0xF;
    n6 = (absn >> 24) & 0xF;
    n7 = (absn >> 28) & 0xF;

    a0 = 6 * (n7 + n6 + n5 + n4 + n3 + n2 + n1) + n0;
    if (a0) {
        q = a0 / 10;
        d0 = a0 % 10;
    } else {
        q = d0 = 0;
    }

    a1 = q + 5*n7 + n6 + 7*n5 + 3*n4 + 9*n3 + 5*n2 + n1;
    if (a1) {
        q = a1 / 10;
        d1 = a1 % 10;
        if (d1) { first_digit = 1; }
    } else {
        q = d1 = 0;
    }

    a2 = q + 4*n7 + 2*n6 + 5*n5 + 5*n4 + 2*n2;
    if (a2) {
        q = a2 / 10;
        d2 = a2 % 10;
        if (d2) { first_digit = 2; }
    } else {
        q = d2 = 0;
    }

    a3 = q + 5*n7 + 7*n6 + 8*n5 + 5*n4 + 4*n3;
    if (a3) {
        q = a3 / 10;
        d3 = a3 % 10;
        if (d3) { first_digit = 3; }
    } else {
        q = d3 = 0;
    }

    a4 = q + 3*n7 + 7*n6 + 4*n5 + 6*n4;
    if (a4) {
        q = a4 / 10;
        d4 = a4 % 10;
        if (d4) { first_digit = 4; }
    } else {
        q = d4 = 0;
    }

    a5 = q + 4*n7 + 7*n6;
    if (a5) {
        q = a5 / 10;
        d5 = a5 % 10;
        if (d5) { first_digit = 5; }
    } else {
        q = d5 = 0;
    }

    a6 = q + 8*n7 + 6*n6 + n5;
    if (a6) {
        q = a6 / 10;
        d6 = a6 % 10;
        if (d6) { first_digit = 6; }
    } else {
        q = d6 = 0;
    }

    a7 = q + 6*n7 + n6;
    if (a7) {
        q = a7 / 10;
        d7 = a7 % 10;
        if (d7) { first_digit = 7; }
    } else {
        q = d7 = 0;
    }

    a8 = q + 2*n7;
    if (a8) {
        q = a8 / 10;
        d8 = a8 % 10;
        if (d8) { first_digit = 8; }
    } else {
        q = d8 = 0;
    }

    d9 = q;
    if (d9) { first_digit = 9; }

    if (width) {
        // width is 1-based; change to 0-based
        if (width > 9) {
            width = 9;
        } else {
            --width;
        }
        if (flags.zeropad) {
            addSign(buf, n, flags);
            for (unsigned i = width; i > first_digit; --i) {
                *buf = '0';
                ++buf;
            }
        } else {
            for (unsigned i = width; i > first_digit; --i) {
                *buf = ' ';
                ++buf;
            }
            addSign(buf, n, flags);
        }
    } else {
        addSign(buf, n, flags);
    }

    switch (first_digit) {
        case 9: *buf = DTOCHAR(d9); ++buf;
        case 8: *buf = DTOCHAR(d8); ++buf;
        case 7: *buf = DTOCHAR(d7); ++buf;
        case 6: *buf = DTOCHAR(d6); ++buf;
        case 5: *buf = DTOCHAR(d5); ++buf;
        case 4: *buf = DTOCHAR(d4); ++buf;
        case 3: *buf = DTOCHAR(d3); ++buf;
        case 2: *buf = DTOCHAR(d2); ++buf;
        case 1: *buf = DTOCHAR(d1); ++buf;
    }
    *buf = DTOCHAR(d0); ++buf;
    *buf = '\0';
    return buf;
}

static char *zu32toa(char *buf, uint32_t n, unsigned width, fmt_flags_t flags)
{
    uint8_t n0, n1, n2, n3, n4, n5, n6, n7;
    uint8_t a8, a7, a6, a5, q; // yes, 8 bits are enough for these
    uint16_t a4, a3, a2, a1, a0;
    uint8_t d0, d1, d2, d3, d4, d5, d6, d7, d8, d9;
    unsigned first_digit = 0;

    n0 = n & 0xF;
    n1 = (n >> 4) & 0xF;
    n2 = (n >> 8) & 0xF;
    n3 = (n >> 12) & 0xF;
    n4 = (n >> 16) & 0xF;
    n5 = (n >> 20) & 0xF;
    n6 = (n >> 24) & 0xF;
    n7 = (n >> 28) & 0xF;

    a0 = 6 * (n7 + n6 + n5 + n4 + n3 + n2 + n1) + n0;
    if (a0) {
        q = a0 / 10;
        d0 = a0 % 10;
    } else {
        q = d0 = 0;
    }

    a1 = q + 5*n7 + n6 + 7*n5 + 3*n4 + 9*n3 + 5*n2 + n1;
    if (a1) {
        q = a1 / 10;
        d1 = a1 % 10;
        if (d1) { first_digit = 1; }
    } else {
        q = d1 = 0;
    }

    a2 = q + 4*n7 + 2*n6 + 5*n5 + 5*n4 + 2*n2;
    if (a2) {
        q = a2 / 10;
        d2 = a2 % 10;
        if (d2) { first_digit = 2; }
    } else {
        q = d2 = 0;
    }

    a3 = q + 5*n7 + 7*n6 + 8*n5 + 5*n4 + 4*n3;
    if (a3) {
        q = a3 / 10;
        d3 = a3 % 10;
        if (d3) { first_digit = 3; }
    } else {
        q = d3 = 0;
    }

    a4 = q + 3*n7 + 7*n6 + 4*n5 + 6*n4;
    if (a4) {
        q = a4 / 10;
        d4 = a4 % 10;
        if (d4) { first_digit = 4; }
    } else {
        q = d4 = 0;
    }

    a5 = q + 4*n7 + 7*n6;
    if (a5) {
        q = a5 / 10;
        d5 = a5 % 10;
        if (d5) { first_digit = 5; }
    } else {
        q = d5 = 0;
    }

    a6 = q + 8*n7 + 6*n6 + n5;
    if (a6) {
        q = a6 / 10;
        d6 = a6 % 10;
        if (d6) { first_digit = 6; }
    } else {
        q = d6 = 0;
    }

    a7 = q + 6*n7 + n6;
    if (a7) {
        q = a7 / 10;
        d7 = a7 % 10;
        if (d7) { first_digit = 7; }
    } else {
        q = d7 = 0;
    }

    a8 = q + 2*n7;
    if (a8) {
        q = a8 / 10;
        d8 = a8 % 10;
        if (d8) { first_digit = 8; }
    } else {
        q = d8 = 0;
    }

    d9 = q;
    if (d9) { first_digit = 9; }

    if (width) {
        // width is 1-based; change to 0-based
        if (width > 9) {
            width = 9;
        } else {
            --width;
        }
        if (flags.zeropad) {
            for (unsigned i = width; i > first_digit; --i) {
                *buf = '0';
                ++buf;
            }
        } else {
            for (unsigned i = width; i > first_digit; --i) {
                *buf = ' ';
                ++buf;
            }
        }
    }

    switch (first_digit) {
        case 9: *buf = DTOCHAR(d9); ++buf;
        case 8: *buf = DTOCHAR(d8); ++buf;
        case 7: *buf = DTOCHAR(d7); ++buf;
        case 6: *buf = DTOCHAR(d6); ++buf;
        case 5: *buf = DTOCHAR(d5); ++buf;
        case 4: *buf = DTOCHAR(d4); ++buf;
        case 3: *buf = DTOCHAR(d3); ++buf;
        case 2: *buf = DTOCHAR(d2); ++buf;
        case 1: *buf = DTOCHAR(d1); ++buf;
    }
    *buf = DTOCHAR(d0); ++buf;
    *buf = '\0';
    return buf;
}

/**
 * Reverse the characters in the passed buffer.
 *
 * @param buf (in/out) buffer of characters to reverse
 * @param end index of the last character in the passed buffer to reverse
 */
static inline void reverse(char *buf, unsigned end)
{
    unsigned start = 0;
    while (start < end) {
        char temp = buf[start];
        buf[start] = buf[end];
        buf[end] = temp;
        ++start;
        --end;
    }
}

/**
 * Print a signed radix decimal 64-bit integer.  The caller is responsible for
 * ensuring the buffer is large enough.
 *
 * This isn't heavily optimized like our 16 and 32-bit functions, but it works.
 * Since this isn't heavily optimized, we call into our 32-bit function instead
 * for integers that will fit into that.
 *
 * @param buf buffer to print to
 * @param n integer to print
 * @param width 1-based width for printf (e.g. %4lld)
 * @param flags flags optionally specifying printf-style 0-pad (e.g. %04lld)
 * @return pointer to next character in the printed buffer
 */
static char *zi64toa(char *buf, int64_t n, unsigned width, fmt_flags_t flags)
{
    if (n >= INT32_MIN && n <= INT32_MAX) {
        return zi32toa(buf, n, width, flags);
    }
    unsigned first_digit = 0;
    char tmp[19];
    if (n == 0) {
        tmp[first_digit] = '0';
    } else {
        uint64_t _n;
        if (n < 0) {
            _n = -n;
        } else {
            _n = n;
        }
        while (_n) {
            int rem = _n % 10;
            tmp[first_digit] = rem + '0';
            _n = _n / 10;
            ++first_digit;
        }
        --first_digit;
    }
    reverse(tmp, first_digit);
    if (width) {
        // width is 1-based; change to 0-based
        if (width > 19) {
            width = 19;
        } else {
            --width;
        }
        if (flags.zeropad) {
            addSign(buf, n, flags);
            for (unsigned i = width; i > first_digit; --i) {
                *buf = '0';
                ++buf;
            }
        } else {
            for (unsigned i = width; i > first_digit; --i) {
                *buf = ' ';
                ++buf;
            }
            addSign(buf, n, flags);
        }
    } else {
        addSign(buf, n, flags);
    }
    memcpy(buf, tmp, first_digit + 1);
    buf += first_digit + 1;
    *buf = '\0';
    return buf;
}

/**
 * Print an unsigned radix decimal 64-bit integer.  The caller is responsible
 * for ensuring the buffer is large enough.
 *
 * This isn't heavily optimized like our 16 and 32-bit functions, but it works.
 * Since this isn't heavily optimized, we call into our 32-bit function instead
 * for integers that will fit into that.
 *
 * @param buf buffer to print to
 * @param n integer to print
 * @param width 1-based width for printf (e.g. %4llu)
 * @param flags flags optionally specifying printf-style 0-pad (e.g. %04llu)
 * @return pointer to next character in the printed buffer
 */
static char *zu64toa(char *buf, uint64_t n, unsigned width, fmt_flags_t flags)
{
    if (n <= UINT32_MAX) {
        return zu32toa(buf, n, width, flags);
    }
    unsigned first_digit = 0;
    char tmp[20];
    if (n == 0) {
        tmp[first_digit] = '0';
    } else {
        while (n) {
            int rem = n % 10;
            tmp[first_digit] = rem + '0';
            n = n / 10;
            ++first_digit;
        }
        --first_digit;
    }
    reverse(tmp, first_digit);
    if (width) {
        // width is 1-based; change to 0-based
        if (width > 20) {
            width = 20;
        } else {
            --width;
        }
        if (flags.zeropad) {
            for (unsigned i = width; i > first_digit; --i) {
                *buf = '0';
                ++buf;
            }
        } else {
            for (unsigned i = width; i > first_digit; --i) {
                *buf = ' ';
                ++buf;
            }
        }
    }
    memcpy(buf, tmp, first_digit + 1);
    buf += first_digit + 1;
    *buf = '\0';
    return buf;
}

// NOTE: saturates to INT32_MIN/INT32_MAX; fraction limited to 4 digits
static char *zftoaf(char *buf, float f, unsigned width, unsigned precision, fmt_flags_t flags)
{
    if (!isfinite(f)) {
        if (isnan(f)) {
            memcpy(buf, "NAN", sizeof("NAN"));
            buf += strlen(buf);
            return buf;
        } else if (isinf(f) == -1) {
            memcpy(buf, "-INF", sizeof("-INF"));
            buf += strlen(buf);
            return buf;
        } else {
            memcpy(buf, "INF", sizeof("INF"));
            buf += strlen(buf);
            return buf;
        }
    }
    if (flags.exp == exp_none && fabsf(f) > (INT32_MAX - 1)) {
        flags.exp = exp_e;
    }
    int exponent = 0;
    if (flags.exp) {
        if (fabsf(f) > 0.0) {
            exponent = log10f(fabsf(f));
            f *= powf(10, -exponent);
            if (!(int32_t)f) {
                f *= 10.0;
                --exponent;
            }
        } else {
            exponent = 0;
        }
    }
    float frnd;
    float fmul;
    float rounded;
    switch(precision) {
        case 0: frnd = 0.5e-0; fmul = 1e0; break;
        case 1: frnd = 0.5e-1; fmul = 1e1; break;
        case 2: frnd = 0.5e-2; fmul = 1e2; break;
        case 3: frnd = 0.5e-3; fmul = 1e3; break;
        case 4: frnd = 0.5e-4; fmul = 1e4; break;
        case 5: frnd = 0.5e-5; fmul = 1e5; break;
        case 6: frnd = 0.5e-6; fmul = 1e6; break;
        case 7: frnd = 0.5e-7; fmul = 1e7; break;
        case 8: frnd = 0.5e-8; fmul = 1e8; break;
        default:
            precision = 9;
            frnd = 0.5e-9;
            fmul = 1.e9;
            break;
    }
    if (f < 0.0) {
        rounded = f - frnd;
    } else {
        rounded = f + frnd;
    }
    int32_t whole = rounded;
    if (flags.exp) {
        if (whole >= 10) {
            rounded *= 0.1;
            ++exponent;
            whole = rounded;
        }
    }
    if (!whole && signbit(f)) {
        *buf = '-';
        ++buf;
        flags.sign = auto_sign;
    }
    if (whole > INT16_MAX || width > 4) {
        buf = zltoa(buf, whole, width, flags);
    } else {
        buf = zitoa(buf, whole, width, flags);
    }
    if (precision || flags.exp) {
        *buf = '.'; ++buf;
    }
    if (precision) {
        float fraction = fabsf(fmul * (rounded - whole));
        const fmt_flags_t fraction_flags = { .zeropad = 1 };
        if (fraction > UINT16_MAX || precision > 4) {
            buf = zultoa(buf, fraction, precision, fraction_flags);
        } else {
            buf = zutoa(buf, fraction, precision, fraction_flags);
        }
    }
    if (flags.exp == exp_e) {
        buf[0] = 'e'; ++buf;
        const fmt_flags_t exponent_flags = { .sign = always_sign, .zeropad = 1 };
        zitoa(buf, exponent, 2, exponent_flags);
    } else if (flags.exp == exp_E) {
        buf[0] = 'E'; ++buf;
        const fmt_flags_t exponent_flags = { .sign = always_sign, .zeropad = 1 };
        zitoa(buf, exponent, 2, exponent_flags);
    }
    return buf;
}

// NOTE: saturates to INT32_MIN/INT32_MAX; fraction limited to 9 digits
static char *zftoal(char *buf, long double f, unsigned width, unsigned precision, fmt_flags_t flags)
{
    if (!isfinite(f)) {
        if (isnan(f)) {
            memcpy(buf, "NAN", sizeof("NAN"));
            buf += strlen(buf);
            return buf;
        }
        if (isinf(f) == -1) {
            memcpy(buf, "-INF", sizeof("-INF"));
            buf += strlen(buf);
            return buf;
        } else {
            memcpy(buf, "INF", sizeof("INF"));
            buf += strlen(buf);
            return buf;
        }
    }
    if (flags.exp == exp_none && fabsl(f) > (INT32_MAX - 1)) {
        flags.exp = exp_e;
    }
    int exponent = 0;
    if (flags.exp) {
        if (fabsl(f) > 0.0) {
            exponent = log10l(fabsl(f));
            f *= powl(10, -exponent);
            if (!(int32_t)f) {
                f *= 10.0;
                --exponent;
            }
        } else {
            exponent = 0;
        }
    }
    long double frnd;
    long double fmul;
    long double rounded;
    switch(precision) {
        case 0: frnd = 0.5e-0; fmul = 1e0; break;
        case 1: frnd = 0.5e-1; fmul = 1e1; break;
        case 2: frnd = 0.5e-2; fmul = 1e2; break;
        case 3: frnd = 0.5e-3; fmul = 1e3; break;
        case 4: frnd = 0.5e-4; fmul = 1e4; break;
        case 5: frnd = 0.5e-5; fmul = 1e5; break;
        case 6: frnd = 0.5e-6; fmul = 1e6; break;
        case 7: frnd = 0.5e-7; fmul = 1e7; break;
        case 8: frnd = 0.5e-8; fmul = 1e8; break;
        default:
            precision = 9;
            frnd = 0.5e-9;
            fmul = 1.e9;
            break;
    }
    if (f < 0.0) {
        rounded = f - frnd;
    } else {
        rounded = f + frnd;
    }
    int32_t whole = rounded;
    if (flags.exp) {
        if (whole >= 10) {
            rounded *= 0.1;
            ++exponent;
            whole = rounded;
        }
    }
    if (!whole && signbit(f)) {
        *buf = '-';
        ++buf;
        flags.sign = auto_sign;
    }
    if (whole > INT16_MAX || width > 4) {
        buf = zltoa(buf, whole, width, flags);
    } else {
        buf = zitoa(buf, whole, width, flags);
    }
    if (precision || flags.exp) {
        *buf = '.'; ++buf;
    }
    if (precision) {
        long double fraction = fabsl(fmul * (rounded - whole));
        const fmt_flags_t fraction_flags = { .zeropad = 1 };
        if (fraction > UINT16_MAX || precision > 4) {
            buf = zultoa(buf, fraction, precision, fraction_flags);
        } else {
            buf = zutoa(buf, fraction, precision, fraction_flags);
        }
    }
    if (flags.exp == exp_e) {
        buf[0] = 'e'; ++buf;
        const fmt_flags_t exponent_flags = { .sign = always_sign, .zeropad = 1 };
        zitoa(buf, exponent, 3, exponent_flags);
    } else if (flags.exp == exp_E) {
        buf[0] = 'E'; ++buf;
        const fmt_flags_t exponent_flags = { .sign = always_sign, .zeropad = 1 };
        zitoa(buf, exponent, 3, exponent_flags);
    }
    return buf;
}

// endptr must be non-null
static inline fmt_flags_t getFlags(char *subspec, char **endptr)
{
    fmt_flags_t flags = { 0 };
    char *flag;
    char *end_of_flags = strpbrk(subspec, ".123456789");
    char flag_dlm = 0;
    if (end_of_flags) {
        // cap the flags string
        flag_dlm = *end_of_flags;
        *end_of_flags = '\0';
    }
    while ((flag = strpbrk(subspec, "0-+ #"))) {
        if (*flag == '-') {
            flags.leftAlign = 1; // '-' flag not currently supported
        } else if (*flag == '+') {
            flags.sign = always_sign;
        } else if (*flag == ' ' && flags.sign != always_sign) {
            flags.sign = sign_or_space;
        } else if (*flag == '#') { // '#' flag not currently supported
            flags.altForm = 1;
        } else if (*flag == '0') {
            flags.zeropad = 1;
        }
        ++subspec;
    }
    if (end_of_flags) {
        // restore sub-spec
        *end_of_flags = flag_dlm;
    }
    *endptr = subspec;
    return flags;
}

// endptr must be non-null
static inline int getWidth(char *subspec, char **endptr)
{
    if (*subspec == '*') {
        // special case; grab width from args
        *endptr = subspec + 1;
        return ARG_SPECIFIED;
    }
    *endptr = subspec;
    return strtol(subspec, endptr, 10);
}

// endptr must be non-null
static inline int getPrecision(char *subspec, char **endptr)
{
    char *delimiter = strchr(subspec, '.');
    if (delimiter == NULL) {
        *endptr = subspec;
        return PRECISION_UNSPECIFIED;
    }
    char *precstr = delimiter + 1;
    if (*precstr == '*') {
        // special case; grab width from args
        *endptr = subspec + 1;
        return ARG_SPECIFIED;
    }
    *endptr = precstr;
    int precval = strtol(precstr, endptr, 10);
    if (*endptr == precstr) {
        // couldn't parse precision
        return PRECISION_UNSPECIFIED;
    } else {
        return precval;
    }
}

typedef enum length_e {
    length_int,
    length_long,
    length_long_long,
} length_t;

static inline length_t getLength(const char * subspec) {
    if (strstr(subspec, "ll")) {
        return length_long_long;
    }
    if (strchr(subspec, 'l')) {
        return length_long;
    }
    if (strchr(subspec, 'L')) {
        return length_long;
    }
    if (strstr(subspec, "j")) {
        return length_long_long;
    }
    if (strstr(subspec, "hh")) {
        return length_int;
    }
    if (strstr(subspec, "h")) {
        return length_int;
    }
    #ifdef SIZE_MAX
    if (strstr(subspec, "z")) {
        #if SIZE_MAX <= UINT_MAX
        return length_int;
	#elif SIZE_MAX == ULONG_MAX
        return length_long;
	#elif SIZE_MAX = ULLONG_MAX
        return length_long_long;
	#else
	#error unsupported SIZE_MAX
	#endif
    }
    #endif /* SIZE_MAX */
    #ifdef PTRDIFF_MAX
    if (strstr(subspec, "t")) {
        #if PTRDIFF_MAX <= INT_MAX
        return length_int;
	#elif PTRDIFF_MAX == LONG_MAX
        return length_long;
	#elif PTRDIFF_MAX = LLONG_MAX
        return length_long_long;
	#else
	#error unsupported PTRDIFF_MAX
	#endif
    }
    #endif /* PTRDIFF_MAX */
    return length_int;
}

static inline void getSubspec(char *buf, unsigned maxlen, const char *escape, const char *specifier)
{
    size_t len = specifier - (escape + 1);
    len = len >= maxlen ? maxlen - 1 : len;
    ZCOAP_MEMCPY(buf, escape + 1, len);
    buf[len] = '\0';
}

size_t zvsnprintf(char *buf, size_t n, const char *fmt, va_list ap)
{
    size_t remain = n, len = 0;
    const char *src = fmt;
    char *dest = buf;
    const char *escape;
    while ((escape = strchr(src, '%'))) {
        size_t toklen = escape - src;
        len += toklen;
        if (toklen >= remain) {
            toklen = remain;
        }
        remain -= toklen;
        ZCOAP_MEMCPY(dest, src, toklen);
        dest += toklen;
        const char *tok = NULL;
        const char *spec = strpbrk(escape + 1, "duxXfFeEgGs%iocpaA");
        if (spec) {
            src = spec + 1;
            char tmp[sizeof(MAX_DEC_FMT_I32"."MAX_DEC_FMT_I32)];
            char subspec[sizeof(MAX_WIDTH_SUB_SPEC)];
            getSubspec(subspec, sizeof(subspec), escape, spec);
            char *endptr = subspec;
            fmt_flags_t flags = getFlags(endptr, &endptr);
            unsigned width = getWidth(endptr, &endptr);
            if (width == ARG_SPECIFIED) { width = va_arg(ap, int); }
            unsigned precision = getPrecision(endptr, &endptr);
            if (precision == ARG_SPECIFIED) { precision = va_arg(ap, int); }
            length_t length = getLength(endptr);
            if (*spec == '%') {
                tok = "%";
            } else if (   *spec == 'd' || *spec == 'i'
                       || *spec == 'u'
                       || *spec == 'x' || *spec == 'X'
                       || *spec == 'o') {
                if (length == length_int) {
                    unsigned val = va_arg(ap, int);
                    if (*spec == 'd' || *spec == 'i') {
                        zitoa(tmp, val, width, flags);
                    } else if (*spec == 'u') {
                        zutoa(tmp, val, width, flags);
                    } else if (*spec == 'x') {
                        zxtoa(tmp, val, width, flags);
                    } else if (*spec == 'X') {
                        zXtoa(tmp, val, width, flags);
		    } else if (*spec == 'o') {
                        zotoa(tmp, val, width, flags);
		    }
                } else if (length == length_long) {
                    long unsigned val = va_arg(ap, long int);
                    if (*spec == 'd' || *spec == 'i') {
                        zltoa(tmp, val, width, flags);
                    } else if (*spec == 'u') {
                        zultoa(tmp, val, width, flags);
                    } else if (*spec == 'x') {
                        zlxtoa(tmp, val, width, flags);
                    } else if (*spec == 'X') {
                        zlXtoa(tmp, val, width, flags);
		    } else if (*spec == 'o') {
                        zlotoa(tmp, val, width, flags);
		    }
                } else if (length == length_long_long) {
                    long long unsigned val = va_arg(ap, long long int);
                    if (*spec == 'd' || *spec == 'i') {
                        zlltoa(tmp, val, width, flags);
                    } else if (*spec == 'u') {
                        zulltoa(tmp, val, width, flags);
                    } else if (*spec == 'x') {
                        zllxtoa(tmp, val, width, flags);
                    } else if (*spec == 'X') {
                        zllXtoa(tmp, val, width, flags);
		    } else if (*spec == 'o') {
                        zllotoa(tmp, val, width, flags);
		    }
                }
                tok = tmp;
            } else if (   *spec == 'f' || *spec == 'F'
                       || *spec == 'e' || *spec == 'E'
                       || *spec == 'g' || *spec == 'G'
                       || *spec == 'a' || *spec == 'A') {
                if (length == length_long) {
                    long double val = va_arg(ap, long double);
                    if (*spec == 'e' || *spec == 'a') {
                        flags.exp = exp_e;
                    } else if (*spec == 'E' || *spec == 'A') {
                        flags.exp = exp_E;
                    } else  if (*spec == 'g' || *spec == 'G') {
                        long double absv = fabsl(val);
                        if (absv < GMINF || absv > GMAXF) {
                            if (*spec == 'g') {
                                flags.exp = exp_e;
                            } else {
                                flags.exp = exp_E;
                            }
                        }
                    }
                    zftoal(tmp, val, width, precision == PRECISION_UNSPECIFIED ? DEFAULT_PRECISION : precision, flags);
                } else {
                    double val = va_arg(ap, double);
                    if (*spec == 'e' || *spec == 'a') {
                        flags.exp = exp_e;
                    } else if (*spec == 'E' || *spec == 'A') {
                        flags.exp = exp_E;
                    } else  if (*spec == 'g' || *spec == 'G') {
                        double absv = fabs(val);
                        if (absv < GMINF || absv > GMAXF) {
                            if (*spec == 'g') {
                                flags.exp = exp_e;
                            } else {
                                flags.exp = exp_E;
                            }
                        }
                    }
                    zftoa(tmp, val, width, precision == PRECISION_UNSPECIFIED ? DEFAULT_PRECISION : precision, flags);
                }
                tok = tmp;
            } else if (*spec == 'p') {
                void *val = va_arg(ap, void *);
                zllxtoa(tmp, (unsigned long long)val, width, flags);
                tok = tmp;
            } else if (*spec == 's') {
                tok = va_arg(ap, const char *);
            } else if (*spec == 'c') {
                tmp[0] = va_arg(ap, int);
                tmp[1] = '\0';
                tok = tmp;
            }
        } else if (strchr(escape + 1, 'n')) {
            src = spec + 1;
            *va_arg(ap, int *) = n - remain;
        } else {
            src = escape + 1;
        }
        if (tok) {
            toklen = strlen(tok);
            len += toklen;
            if (toklen > remain) {
                toklen = remain;
            }
            remain -= toklen;
            ZCOAP_MEMCPY(dest, tok, toklen);
            dest += toklen;
        }
    }
    {
        size_t toklen = strlen(src);
        len += toklen;
        if (toklen >= remain) {
            toklen = remain;
        }
        remain -= toklen;
        ZCOAP_MEMCPY(dest, src, toklen);
        dest += toklen;
        if (remain) {
            *dest = '\0';
        } else if (n) {
            buf[n - 1] = '\0';
        }
    }
    return len;
}

size_t zsnprintf(char *buf, size_t n, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    size_t len = zvsnprintf(buf, n, fmt, ap);
    va_end(ap);
    return len;
}
