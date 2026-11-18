#include <stdint.h>

/* Minimal software helpers when libgcc is unavailable/mismatched for this target. */

unsigned int __clzsi2(unsigned int x) {
    unsigned int n = 0;
    if (x == 0) return 32;
    if ((x & 0xFFFF0000u) == 0) { n += 16; x <<= 16; }
    if ((x & 0xFF000000u) == 0) { n += 8;  x <<= 8;  }
    if ((x & 0xF0000000u) == 0) { n += 4;  x <<= 4;  }
    if ((x & 0xC0000000u) == 0) { n += 2;  x <<= 2;  }
    if ((x & 0x80000000u) == 0) { n += 1; }
    return n;
}


unsigned int __popcountsi2(unsigned int a) {
    unsigned int c = 0;
    while (a) { a &= a - 1; c++; }
    return c;
}

unsigned int __ctzsi2(unsigned int a) {
    if (a == 0) return 32;
    unsigned int c = 0;
    while ((a & 1) == 0) { a >>= 1; c++; }
    return c;
}

unsigned int __bswapsi2(unsigned int a) {
    return ((a & 0x000000ffu) << 24) |
           ((a & 0x0000ff00u) << 8)  |
           ((a & 0x00ff0000u) >> 8)  |
           ((a & 0xff000000u) >> 24);
}

uint64_t __ashldi3(uint64_t a, int b) {
    if (b == 0) return a;
    if (b >= 64) return 0;
    if (b >= 32) return ((uint64_t)(uint32_t)a) << (b - 32);
    return (a << b);
}

uint64_t __lshrdi3(uint64_t a, int b) {
    if (b == 0) return a;
    if (b >= 64) return 0;
    if (b >= 32) return (uint64_t)((uint32_t)(a >> 32) >> (b - 32));
    return (a >> b);
}
