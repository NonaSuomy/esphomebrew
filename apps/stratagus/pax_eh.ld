/* Native Warcraft HUD/menu parsing uses C++ exceptions. The shared PAX
 * script discards unwind tables, so retain them for these two ports only. */
SECTIONS
{
    .eh_frame ALIGN(4) : {
        PROVIDE_HIDDEN(__pax_eh_frame_start = .);
        KEEP(*(.eh_frame))
        LONG(0)
    }
}
INSERT AFTER .rodata;
