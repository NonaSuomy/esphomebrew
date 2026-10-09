#pragma once
#include <stdint.h>
#include <string.h>

// IEEE-754 rounding without entering a CRT routine built for hardware D.
// The P4 has single-precision F only; Lua's math.floor/ceil use double.
static inline double pax_round_integral(double value, int upward) {
    uint64_t bits; memcpy(&bits,&value,sizeof(bits));
    const unsigned sign=unsigned(bits>>63);
    const int exponent=int((bits>>52)&2047)-1023;
    if(exponent>=52 || !(bits&UINT64_C(0x7fffffffffffffff))) return value;
    if(exponent<0) {
        bits=sign?UINT64_C(0x8000000000000000):0;
        if((!sign && upward) || (sign && !upward)) bits|=UINT64_C(0x3ff0000000000000);
    } else {
        const uint64_t unit=UINT64_C(1)<<(52-exponent),mask=unit-1;
        if(!(bits&mask)) return value;
        bits&=~mask;
        if((!sign && upward) || (sign && !upward)) bits+=unit;
    }
    memcpy(&value,&bits,sizeof(value)); return value;
}

static inline double pax_modf(double value, double *integer) {
    uint64_t bits; memcpy(&bits,&value,sizeof(bits));
    const uint64_t sign=bits&UINT64_C(0x8000000000000000);
    const unsigned exponent=unsigned((bits>>52)&2047);
    if(exponent==2047) {
        *integer=value;
        if(bits&UINT64_C(0x000fffffffffffff)) return value; // NaN
        double zero; memcpy(&zero,&sign,sizeof(zero)); return zero;
    }
    // Truncate toward zero, preserving signed zero in both outputs.
    *integer=pax_round_integral(value,sign?1:0);
    if(value==*integer) {
        double zero; memcpy(&zero,&sign,sizeof(zero)); return zero;
    }
    return value-*integer; // software double subtraction for the PAX ABI
}
