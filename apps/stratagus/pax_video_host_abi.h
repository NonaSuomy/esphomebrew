#pragma once

// ESPHome's P4 firmware uses ilp32f; the engine deliberately uses ilp32.
// A float service call otherwise puts scale in a3 (where the firmware reads
// byte_swap) and leaves fa0 undefined. Integer-only services are unaffected.
// Keep the conversion isolated to this tail-call adapter: no FP arithmetic
// or hard-float ELF objects are linked into the soft-ABI engine.
#if defined(__riscv)
extern "C" void pax_video_host_ilp32f(const uint16_t *, unsigned, unsigned,
	uint32_t, decltype(app_services_t::display_write_frame_custom));
asm(".pushsection .text.pax_video_host_ilp32f,\"ax\",@progbits\n"
    ".option push\n.option arch,+f\n.balign 4\n"
    ".global pax_video_host_ilp32f\n.type pax_video_host_ilp32f,@function\n"
    "pax_video_host_ilp32f:\n"
    "fmv.w.x fa0,a3\n"
    "mv t0,a4\n"
    "li a3,0\n"
    "jr t0\n"
    ".size pax_video_host_ilp32f,.-pax_video_host_ilp32f\n"
    ".option pop\n.popsection\n");
#endif
