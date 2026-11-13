/* NitroSDK's basic types, with the 64-bit ones aligned as Metrowerks aligned them: to 4 bytes.
 * (ARM EABI GCC aligns them to 8, which pads every struct holding one differently from the DS:
 * the save chunks no longer matched their footers, and the assembly reads such structs at
 * Metrowerks' offsets.) A typedef can lower a type's alignment in GCC. */
#ifndef PORT_NITRO_TYPES_H
#define PORT_NITRO_TYPES_H

#define u64 port_u64_natural_
#define s64 port_s64_natural_
#define vu64 port_vu64_natural_
#define vs64 port_vs64_natural_
#define REGType64 port_REGType64_natural_
#define REGType64v port_REGType64v_natural_
#include_next <nitro/types.h>
#undef u64
#undef s64
#undef vu64
#undef vs64
#undef REGType64
#undef REGType64v

typedef unsigned long long int u64 __attribute__((aligned(4)));
typedef signed long long int s64 __attribute__((aligned(4)));
typedef volatile u64 vu64;
typedef volatile s64 vs64;
typedef u64 REGType64;
typedef vu64 REGType64v;

#endif
