/* The run-time helpers clang calls in code for the game's ARM11 (armv6k has
 * no divide; struct copies and clears go through the ARM EABI's memory
 * functions). sdk/native.py links these when the port's code needs one that
 * no symbol file gives (a game's own, as `3dsrecomp symbols` finds them in
 * its static.crs, are used when known). Plain loops: nothing here may call
 * what it defines. Built with -ffreestanding, which keeps clang from
 * turning the loops back into calls. */

#include "../include/port.h"

typedef unsigned int size;

/* memory */

void *memcpy(void *dst, const void *src, size n) {
    u8 *d = dst;
    const u8 *s = src;
    while (n--) *d++ = *s++;
    return dst;
}

void *memmove(void *dst, const void *src, size n) {
    u8 *d = dst;
    const u8 *s = src;
    if (d < s) {
        while (n--) *d++ = *s++;
    } else {
        d += n;
        s += n;
        while (n--) *--d = *--s;
    }
    return dst;
}

void *memset(void *dst, int c, size n) {
    u8 *d = dst;
    while (n--) *d++ = (u8)c;
    return dst;
}

/* the EABI's: memset's arguments in another order, memclr without a value */
void __aeabi_memcpy(void *dst, const void *src, size n) { memcpy(dst, src, n); }
void __aeabi_memcpy4(void *dst, const void *src, size n) { memcpy(dst, src, n); }
void __aeabi_memcpy8(void *dst, const void *src, size n) { memcpy(dst, src, n); }
void __aeabi_memmove(void *dst, const void *src, size n) { memmove(dst, src, n); }
void __aeabi_memmove4(void *dst, const void *src, size n) { memmove(dst, src, n); }
void __aeabi_memmove8(void *dst, const void *src, size n) { memmove(dst, src, n); }
void __aeabi_memset(void *dst, size n, int c) { memset(dst, c, n); }
void __aeabi_memset4(void *dst, size n, int c) { memset(dst, c, n); }
void __aeabi_memset8(void *dst, size n, int c) { memset(dst, c, n); }
void __aeabi_memclr(void *dst, size n) { memset(dst, 0, n); }
void __aeabi_memclr4(void *dst, size n) { memset(dst, 0, n); }
void __aeabi_memclr8(void *dst, size n) { memset(dst, 0, n); }

/* division, a bit at a time; by zero gives 0 */

static u32 udivmod32(u32 n, u32 d, u32 *rem) {
    u64 r = 0;
    u32 q = 0;
    if (!d) {
        *rem = n;
        return 0;
    }
    for (int i = 31; i >= 0; i--) {
        r = (r << 1) | ((n >> i) & 1);
        if (r >= d) {
            r -= d;
            q |= 1u << i;
        }
    }
    *rem = (u32)r;
    return q;
}

static u64 udivmod64(u64 n, u64 d, u64 *rem) {
    u64 r = 0, q = 0;
    if (!d) {
        *rem = n;
        return 0;
    }
    for (int i = 63; i >= 0; i--) {
        u64 top = r >> 63;
        r = (r << 1) | ((n >> i) & 1);
        if (top || r >= d) {
            r -= d;
            q |= 1ull << i;
        }
    }
    *rem = r;
    return q;
}

u32 __aeabi_uidiv(u32 n, u32 d) {
    u32 r;
    return udivmod32(n, d, &r);
}

s32 __aeabi_idiv(s32 n, s32 d) {
    u32 r, q = udivmod32(n < 0 ? -(u32)n : (u32)n, d < 0 ? -(u32)d : (u32)d, &r);
    return (n < 0) != (d < 0) ? -(s32)q : (s32)q;
}

/* the quotient in r0, the remainder in r1: a u64's halves */
u64 __aeabi_uidivmod(u32 n, u32 d) {
    u32 r, q = udivmod32(n, d, &r);
    return (u64)r << 32 | q;
}

u64 __aeabi_idivmod(s32 n, s32 d) {
    u32 r, q = udivmod32(n < 0 ? -(u32)n : (u32)n, d < 0 ? -(u32)d : (u32)d, &r);
    s32 quotient = (n < 0) != (d < 0) ? -(s32)q : (s32)q, remainder = n < 0 ? -(s32)r : (s32)r;
    return (u64)(u32)remainder << 32 | (u32)quotient;
}

/* 64 bits: the quotient in r0 and r1, the remainder in r2 and r3, which C
 * cannot return; the C below takes where to leave the remainder, and the
 * EABI's entries move it into r2 and r3 */

__attribute__((used)) u64 __bergamota_uldivmod(u64 n, u64 d, u64 *rem) { return udivmod64(n, d, rem); }

__attribute__((used)) s64 __bergamota_ldivmod(s64 n, s64 d, s64 *rem) {
    u64 r, q = udivmod64(n < 0 ? -(u64)n : (u64)n, d < 0 ? -(u64)d : (u64)d, &r);
    *rem = n < 0 ? -(s64)r : (s64)r;
    return (n < 0) != (d < 0) ? -(s64)q : (s64)q;
}

#if defined(__arm__)
#define DIVMOD64(name, body)                                                  \
    __attribute__((naked)) void name(void) {                                  \
        __asm__("push {r4, lr}\n"                                             \
                "sub sp, sp, #16\n"                                           \
                "add r4, sp, #8\n" /* the remainder's place */                \
                "str r4, [sp]\n"   /* the third argument, on the stack */     \
                "bl " body "\n"                                               \
                "ldr r2, [sp, #8]\n"                                          \
                "ldr r3, [sp, #12]\n"                                         \
                "add sp, sp, #16\n"                                           \
                "pop {r4, pc}\n");                                            \
    }

DIVMOD64(__aeabi_uldivmod, "__bergamota_uldivmod")
DIVMOD64(__aeabi_ldivmod, "__bergamota_ldivmod")
#endif
