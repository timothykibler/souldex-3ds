/* NitroSDK's static initializer hook, for GCC: a pointer in .sinit that the port's overlay
   loader calls when the overlay is "loaded" (each overlay's .sinit range is known from the link). */
static void NitroStaticInit(void);
__attribute__((used, section(".sinit"))) static void (*NitroStaticInit_[])(void) = { NitroStaticInit };
