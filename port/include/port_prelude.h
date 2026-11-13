/* Included before every game source when building the 3DS port with GCC (-include). */
#ifndef PORT_PRELUDE_H
#define PORT_PRELUDE_H

/* NitroSDK's math.h has Metrowerks inline asm for CLZ: use GCC's builtin instead. */
#define PLATFORM_INTRINSIC_FUNCTION_BIT_CLZ32(x) ((u32)((x) ? __builtin_clz(x) : 32))


/* Metrowerks __declspec(...) attributes, as GCC ones. */
#define __declspec(x) __port_declspec_##x
#define __port_declspec_weak __attribute__((weak))
#define __port_declspec_force_export __attribute__((used))

#endif
