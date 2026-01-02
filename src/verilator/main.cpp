// main.cpp
// Verilator-based test that mirrors tb.sv:test_top_SCREEN5_SP2 SCREEN5 scenario
//
// - Uses vdp_cartridge_wrapper.* API
// - Replays the same sequence of VDP register and VRAM writes as tb.sv
// - Generates a single VCD (dump.vcd) for waveform inspection

#include <iostream>
#include <cstdlib>
#include <cstdint>

#include "vdp_cartridge_wrapper.h"

static void step_cycles(int cycles)
{
    for (int i = 0; i < cycles; ++i) {
        vdp_cartridge_step_clk_1cycle();
    }
}

int main(int argc, char** argv)
{
    (void)argc; (void)argv;

    // --------------------------------------------------------------------
    // Init / trace
    // --------------------------------------------------------------------
    vdp_cartridge_init();
    vdp_cartridge_set_debug(0);
    vdp_cartridge_set_write_on_posedge(1);  // use tb.sv-like write_io
    vdp_cartridge_set_end_align(0);         // we handle phase in write_io
    vdp_cartridge_trace_open("dump.vcd");

    // Inputs
    vdp_cartridge_set_button(0);
    vdp_cartridge_set_dipsw(0);

    // --------------------------------------------------------------------
    // Reset sequence (mirror tb.sv)
    // tb.sv:
    //   clk_dummy = 0; clk14m = 0; slot_reset_n = 0; ...
    //   repeat(10) @(posedge clk14m);
    //   slot_reset_n = 1;
    //   repeat(10) @(posedge clk14m);
    // --------------------------------------------------------------------
    // We use vdp_cartridge_reset() which does 8 cycles low/high internally,
    // plus a few extra cycles before/after to approximate tb.sv.
    step_cycles(10);
    vdp_cartridge_reset();
    step_cycles(10);

    std::cout << "[main] Wait initialization (slot_wait deassert)\n";
    // while (slot_wait == 1) @(posedge clk14m);
    while (vdp_cartridge_get_slot_wait() == 1) {
        step_cycles(1);
    }
    step_cycles(10);

    // --------------------------------------------------------------------
    // Constants (same as tb.sv)
    // --------------------------------------------------------------------
    const uint16_t vdp_io0 = 0x88;            // vdp_io0 in tb.sv
    const uint16_t vdp_io1 = vdp_io0 + 0x01;  // vdp_io1 in tb.sv

    // --------------------------------------------------------------------
    // SCREEN5 register setup (mirror tb.sv lines 483–511)
    //
    // tb.sv:
    //   write_io( vdp_io1, 8'h06 );  // R#0 = 0x06
    //   write_io( vdp_io1, 8'h80 );
    //   ...
    // --------------------------------------------------------------------
    auto write_io = [](uint16_t addr, uint8_t data) {
        vdp_cartridge_write_io(addr, data);
    };

    std::cout << "[main] SCREEN5: set VDP registers\n";

    // VDP R#0 = 0x06
    write_io(vdp_io1, 0x06);
    write_io(vdp_io1, 0x80);

    // VDP R#1 = 0x40
    write_io(vdp_io1, 0x40);
    write_io(vdp_io1, 0x81);

    // VDP R#2 = 0x1F
    write_io(vdp_io1, 0x1F);
    write_io(vdp_io1, 0x82);

    // VDP R#5 = 0xEF : Sprite Attribute = 7600h
    write_io(vdp_io1, 0xEF);
    write_io(vdp_io1, 0x85);

    // VDP R#6 = 0x0F : Sprite Pattern = 7800h
    write_io(vdp_io1, 0x0F);
    write_io(vdp_io1, 0x86);

    // VDP R#8 = 0x08
    write_io(vdp_io1, 0x08);
    write_io(vdp_io1, 0x88);

    // VDP R#11 = 0x00
    write_io(vdp_io1, 0x00);
    write_io(vdp_io1, 0x8B);

    // VDP R#20 = 0x00
    write_io(vdp_io1, 0x00);
    write_io(vdp_io1, 0x80 + 20);

    // VDP R#21 = 0x00
    write_io(vdp_io1, 0x00);
    write_io(vdp_io1, 0x80 + 21);

    // --------------------------------------------------------------------
    // VRAM clear and pattern write (mirror tb.sv lines 512–523)
    //
    // tb.sv:
    //   write_io( vdp_io1, 8'h00 );
    //   write_io( vdp_io1, 8'h8E );
    //   write_io( vdp_io1, 8'h00 );
    //   write_io( vdp_io1, 8'h40 );
    //   for (i = 0; i < (128*32); i++) begin
    //     write_io( vdp_io0, (i & 255) );
    //     repeat( $urandom(40) ) @(posedge clk14m);
    //   end
    // --------------------------------------------------------------------
    std::cout << "[main] Write VRAM pattern (SCREEN5)\n";

    // VRAM 0x00000 ... 0x07FFF = 0x00 (setup)
    write_io(vdp_io1, 0x00);
    write_io(vdp_io1, 0x8E);
    write_io(vdp_io1, 0x00);
    write_io(vdp_io1, 0x40);

    const int vram_words = 128 * 32;  // 4096
    for (int i = 0; i < vram_words; ++i) {
        write_io(vdp_io0, static_cast<uint8_t>(i & 0xFF));

        // In tb.sv: repeat($urandom(40)) @(posedge clk14m);
        // Here we just insert a small, deterministic delay for now.
        step_cycles(4);
    }

    // --------------------------------------------------------------------
    // Sprite plane setup (mirror tb.sv lines 524–551)
    // --------------------------------------------------------------------
    std::cout << "[main] Setup sprite planes\n";

    // Plane #0
    write_io(vdp_io0, 0);
    write_io(vdp_io0, 0);
    write_io(vdp_io0, 4);
    write_io(vdp_io0, 15);

    // Plane #1
    write_io(vdp_io0, 0);
    write_io(vdp_io0, 32);
    write_io(vdp_io0, 4);
    write_io(vdp_io0, 15);

    // Plane #2
    write_io(vdp_io0, 0);
    write_io(vdp_io0, 64);
    write_io(vdp_io0, 4);
    write_io(vdp_io0, 15);

    // Plane #3
    write_io(vdp_io0, 0);
    write_io(vdp_io0, 96);
    write_io(vdp_io0, 4);
    write_io(vdp_io0, 15);

    // --------------------------------------------------------------------
    // Let the display run for a while (tb.sv lines 553–559)
    //
    // tb.sv:
    //   repeat(100) @(posedge clk14m);
    //   repeat(1368 * 16 * 200) @(posedge clk14m);
    //   [test---] All tests completed
    //   repeat(100) @(posedge clk14m);
    // --------------------------------------------------------------------
    std::cout << "[main] Run display for a while\n";

    step_cycles(100);
    // This is a lot of cycles; you can reduce for faster runs if needed.
    const int display_cycles = 1368 * 16 * 200;
    step_cycles(display_cycles);
    step_cycles(100);

    std::cout << "[main] All tests completed\n";

    vdp_cartridge_trace_close();
    vdp_cartridge_release();
    return 0;
}