/* Stratagus-only extension to the shared PAX layout. This toolchain emits
 * global C++ destructors in .fini_array, not in newlib's atexit registry.
 * Keep them despite --gc-sections; the persistent launcher never runs CRT. */
SECTIONS
{
    .fini_array ALIGN(4) : {
        PROVIDE_HIDDEN(__fini_array_start = .);
        KEEP(*(SORT_BY_INIT_PRIORITY(.fini_array.*)))
        KEEP(*(.fini_array))
        PROVIDE_HIDDEN(__fini_array_end = .);
    }
}
INSERT AFTER .init_array;
