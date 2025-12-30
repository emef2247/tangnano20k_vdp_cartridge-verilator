// main.cpp
#include <iostream>
#include <random>
#include <cstdint>
#include "vdp_cartridge_wrapper.h"

int main(int argc, char** argv) {
    (void)argc; (void)argv;

    // Initialize wrapper + DUT
    vdp_cartridge_init();

    // Open full VCD dump (will produce dump.vcd). Ignore failure if tracing unavailable.
    if (vdp_cartridge_trace_open("dump.vcd") != 0) {
        std::cerr << "Warning: failed to open VCD (trace not available)\n";
    }

    // Ensure known input defaults (wrapper also does initialization)
    vdp_cartridge_set_button(0);
    vdp_cartridge_set_dipsw(0);

    // Reset sequence: repeat(10) @( posedge clk14m );
    for (int i = 0; i < 10; ++i) vdp_cartridge_step_clk_1cycle();

    // Perform an explicit reset (wrapper will run cycles internally)
    vdp_cartridge_reset();

    // Wait a bit more as in tb.sv: repeat(10) @( posedge clk14m );
    for (int i = 0; i < 10; ++i) vdp_cartridge_step_clk_1cycle();

    std::cout << "[test---] Wait initialization\n";

    // tb.sv waits until slot_wait == 0; vdp_cartridge_write_io already waits for slot_wait when needed.
    // Here add a modest fixed wait before starting register writes.
    for (int i = 0; i < 20; ++i) vdp_cartridge_step_clk_1cycle();

    // Use the same IO port addresses as tb.sv:
    // tb.sv: localparam vdp_io0 = 8'h88; vdp_io1 = vdp_io0 + 1 = 8'h89
    const uint16_t vdp_io0 = 0x88;
    const uint16_t vdp_io1 = 0x89;

    // Helper lambda to call wrapper write_io
    auto write_io = [](uint16_t addr, uint8_t data) {
        vdp_cartridge_write_io(addr, data);
    };

    // Reproduce the tb.sv initial register sequence for SCREEN5
    // VDP R#0 = 0x06
    write_io(vdp_io1, 0x06);
    write_io(vdp_io1, 0x80);
    // VDP R#1 = 0x40
    write_io(vdp_io1, 0x40);
    write_io(vdp_io1, 0x81);
    // VDP R#2 = 0x1F
    write_io(vdp_io1, 0x1F);
    write_io(vdp_io1, 0x82);
    // VDP R#5 = 0xEF
    write_io(vdp_io1, 0xEF);
    write_io(vdp_io1, 0x85);
    // VDP R#6 = 0x0F
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
    write_io(vdp_io1, static_cast<uint8_t>(0x80 + 20));
    // VDP R#21 = 0x00
    write_io(vdp_io1, 0x00);
    write_io(vdp_io1, static_cast<uint8_t>(0x80 + 21));

    std::cout << "[test001] Write VRAM\n";

    // VRAM write setup (from tb.sv)
    write_io(vdp_io1, 0x00);
    write_io(vdp_io1, 0x8E);
    write_io(vdp_io1, 0x00);
    write_io(vdp_io1, 0x40);

    // Fill VRAM (same count as tb.sv: 128 * 32)
    std::random_device rd;
    std::mt19937 gen(rd());
    std::uniform_int_distribution<> rnd_delay(0, 39);
    const int VRAM_FILL = 128 * 32;
    for (int i = 0; i < VRAM_FILL; ++i) {
        write_io(vdp_io0, static_cast<uint8_t>(i & 0xFF));
        // tb.sv does: repeat($urandom(40)) @( posedge clk14m );
        int wait_cycles = rnd_delay(gen);
        for (int w = 0; w < wait_cycles; ++w) vdp_cartridge_step_clk_1cycle();
    }

    // Continue sequence after VRAM writes
    write_io(vdp_io1, 0x01);
    write_io(vdp_io1, 0x8E);
    write_io(vdp_io1, 0x00);
    write_io(vdp_io1, 0x76);

    // Plane#0..3 (as in tb.sv)
    write_io(vdp_io0, 0x00); write_io(vdp_io0, 0x00); write_io(vdp_io0, 0x04); write_io(vdp_io0, 0x0F);
    write_io(vdp_io0, 0x00); write_io(vdp_io0, 0x20); write_io(vdp_io0, 0x04); write_io(vdp_io0, 0x0F);
    write_io(vdp_io0, 0x00); write_io(vdp_io0, 0x40); write_io(vdp_io0, 0x04); write_io(vdp_io0, 0x0F);
    write_io(vdp_io0, 0x00); write_io(vdp_io0, 0x60); write_io(vdp_io0, 0x04); write_io(vdp_io0, 0x0F);

    // repeat(100) @( posedge clk14m );
    for (int i = 0; i < 100; ++i) vdp_cartridge_step_clk_1cycle();

    // Long wait — same as tb.sv (warning: very long)
    const long long LONG_WAIT = 1368LL * 16LL * 200LL;
    std::cout << "[test] Long wait cycles: " << LONG_WAIT << " (this may take long time)\n";
    for (long long i = 0; i < LONG_WAIT; ++i) vdp_cartridge_step_clk_1cycle();

    std::cout << "[test---] All tests completed\n";

    // final waits and finish
    for (int i = 0; i < 100; ++i) vdp_cartridge_step_clk_1cycle();

    // Close trace and release resources
    vdp_cartridge_trace_close();
    vdp_cartridge_release();

    return 0;
}