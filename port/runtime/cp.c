// The DS's divider and square-root units, in software: CP_SetDiv* / CP_SetSqrt* (the port's
// nitro/cp/divider.h and sqrt.h) call these after writing the operands, and the results land in
// the shadow result registers where CP_GetDivResult* / CP_GetSqrtResult* read them.
#include <stdint.h>
#include <nitro.h>

void port_cp_div_update(void) {
    u32 mode = reg_CP_DIVCNT & REG_CP_DIVCNT_MODE_MASK;
    s64 numer, denom;
    if (mode == CP_DIV_32_32BIT_MODE) {
        numer = *(s32 *)REG_DIV_NUMER_ADDR;
        denom = *(s32 *)REG_DIV_DENOM_ADDR;
    } else if (mode == CP_DIV_64_32BIT_MODE) {
        numer = *(s64 *)REG_DIV_NUMER_ADDR;
        denom = *(s32 *)REG_DIV_DENOM_ADDR;
    } else {
        numer = *(s64 *)REG_DIV_NUMER_ADDR;
        denom = *(s64 *)REG_DIV_DENOM_ADDR;
    }
    s64 q, r;
    reg_CP_DIVCNT &= ~REG_CP_DIVCNT_DIV0_MASK;
    if (denom == 0) {
        // (the hardware's answer: remainder = numerator, quotient = +-1 the wrong way)
        q = numer < 0 ? 1 : -1;
        r = numer;
        reg_CP_DIVCNT |= REG_CP_DIVCNT_DIV0_MASK;
    } else if (numer == INT64_MIN && denom == -1) {
        q = numer;
        r = 0;
    } else {
        q = numer / denom;
        r = numer % denom;
    }
    *(s64 *)REG_DIV_RESULT_ADDR = q;
    *(s64 *)REG_DIVREM_RESULT_ADDR = r;
}

void port_cp_sqrt_update(void) {
    u64 x;
    if ((reg_CP_SQRTCNT & REG_CP_SQRTCNT_MODE_MASK) == CP_SQRT_32BIT_MODE) x = *(u32 *)REG_SQRT_PARAM_ADDR;
    else x = *(u64 *)REG_SQRT_PARAM_ADDR;
    // integer square root (floor), 64-bit input, 32-bit result
    u64 r = 0, bit = 1ULL << 62;
    while (bit > x) bit >>= 2;
    while (bit) {
        if (x >= r + bit) { x -= r + bit; r = (r >> 1) + bit; } else r >>= 1;
        bit >>= 2;
    }
    *(u32 *)REG_SQRT_RESULT_ADDR = (u32)r;
}
