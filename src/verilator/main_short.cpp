// main_short.cpp
// Short test to exercise a single VDP register write (R#0 = 0x06 then command 0x80)
// Use this to generate a small VCD for quick waveform checks.

#include <iostream>
#include "vdp_cartridge_wrapper.h"

int main(int argc, char** argv) {
    (void)argc; (void)argv;

    // Initialize DUT wrapper
    vdp_cartridge_init();
    vdp_cartridge_set_debug(1); // enable runtime debug
	vdp_cartridge_set_reset_cycles(0);
	vdp_cartridge_set_write_on_posedge(1);
	vdp_cartridge_set_end_align(0);
    vdp_cartridge_trace_open("dump_short.vcd");
	
    // Open trace (dump.vcd)
    if (vdp_cartridge_trace_open("dump_short.vcd") != 0) {
        std::cerr << "Warning: failed to open VCD (trace not available)\n";
    }

    // Ensure inputs
    vdp_cartridge_set_button(0);
    vdp_cartridge_set_dipsw(0);

    // Small reset sequence similar to tb.sv: repeat(10) @( posedge clk14m );
    for (int i = 0; i < 10; ++i) vdp_cartridge_step_clk_1cycle();

    // Release reset using wrapper helper (runs a few cycles)
    vdp_cartridge_reset();

    // Wait a few more cycles (tb does repeat(10))
    for (int i = 0; i < 10; ++i) vdp_cartridge_step_clk_1cycle();

    std::cout << "[short-test] Waiting for slot_wait deassert (if any)\n";
    // Wait until slot_wait == 0 like tb.sv would
    // Use the provided getter to poll the DUT busy flag
    int safety = 0;
    while (vdp_cartridge_get_slot_wait() == 1 && safety < 1000) {
        vdp_cartridge_step_clk_1cycle();
        ++safety;
    }

    // Use same IO addresses as tb.sv (SCREEN5 test top)
    const uint16_t vdp_io0 = 0x88;
    const uint16_t vdp_io1 = 0x89;

    // Perform the minimal register write sequence:
    // Write R#0 = 0x06, then send the write command 0x80 to apply it.
    std::cout << "[short-test] Write R#0 = 0x06\n";
    vdp_cartridge_write_io(vdp_io1, 0x06);
    vdp_cartridge_write_io(vdp_io1, 0x80);

    // Let the DUT settle a few cycles so changes propagate to internal registers
    for (int i = 0; i < 20; ++i) vdp_cartridge_step_clk_1cycle();

    std::cout << "[short-test] Done. Closing trace and exiting.\n";

    // Close trace and release resources
    vdp_cartridge_trace_close();
    vdp_cartridge_release();

    return 0;
}

