// main.cpp
// Verilator-based test that mirrors tb.sv:test_top_SCREEN5_SP2 SCREEN5 scenario
//
// - Uses vdp_cartridge_wrapper.* API
// - Replays the same sequence of VDP register and VRAM writes as tb.sv
// - Generates a single VCD (dump.vcd) for waveform inspection
// - Additionally:
//   * dumps VRAM as PGM images after the display is running
//   * dumps final RGB frames from VDP display_* as PPM images

#include <iostream>
#include <cstdlib>
#include <cstdint>
#include <cstdio>
#include <inttypes.h>
#include <vector>
#include "vdp_cartridge_wrapper.h"


// ----------------------------------------------------------------------
// Workaround: some generated Verilator code still calls sc_time_stamp(),
// which is a SystemC API. Provide a dummy implementation so linking
// succeeds; we don't rely on this time value anywhere.
// ----------------------------------------------------------------------
double sc_time_stamp() {
    return 0.0;
}

static void step_cycles(int cycles)
{
    for (int i = 0; i < cycles; ++i) {
        vdp_cartridge_step_clk_1cycle();
    }
}

// g_vram を 256x212 の簡易 SCREEN5 4bpp とみなして PGM 出力
static void dump_vram_as_pgm(const char* filename)
{
    FILE* fp = std::fopen(filename, "wb");
    if (!fp) {
        std::fprintf(stderr, "Failed to open %s for write\n", filename);
        return;
    }

    const int W = 256;
    const int H = 212;

    // PGMヘッダ (binary P5)
    std::fprintf(fp, "P5\n%d %d\n255\n", W, H);

    // g_vram をバイト配列として扱う
    const uint8_t* vram_bytes =
        reinterpret_cast<const uint8_t*>(vdp_cartridge_get_vram_buffer());

    // 簡易4bpp SCREEN5 仮定:
    //  - 画面は VRAM 先頭から 256x212/2 = 27136 バイトぶんを使用
    //  - 1バイトに左右2ピクセル (上位4bit, 下位4bit)
    const size_t base = 0;
    const int bytes_per_line = W / 2;

    for (int y = 0; y < H; ++y) {
        uint8_t line[W];
        for (int bx = 0; bx < bytes_per_line; ++bx) {
            uint8_t b = vram_bytes[base + y * bytes_per_line + bx];
            uint8_t left  = (b >> 4) & 0x0F;
            uint8_t right = b & 0x0F;
            // 4bit → 8bit グレースケール (0..15 -> 0..255)
            line[2 * bx]     = static_cast<uint8_t>(left * 17);
            line[2 * bx + 1] = static_cast<uint8_t>(right * 17);
        }
        std::fwrite(line, 1, W, fp);
    }

    std::fclose(fp);
    std::fprintf(stderr, "[dump] wrote %s\n", filename);
}

// VDP display_* 出力から 1フレーム分の RGB を PPM にダンプ
static void dump_display_as_ppm(const char* filename)
{
    VdpVideoMode mode;
    vdp_get_video_mode(&mode);

    const int W = mode.width;
    const int H = mode.height;
    const int pitch = W * 3;

    std::vector<uint8_t> buf(static_cast<size_t>(pitch) * H);

    // 1フレーム分のピクセルを取得
    vdp_render_frame_rgb(buf.data(), pitch);

    FILE* fp = std::fopen(filename, "wb");
    if (!fp) {
        std::fprintf(stderr, "Failed to open %s for write\n", filename);
        return;
    }

    // PPM (binary P6)
    std::fprintf(fp, "P6\n%d %d\n255\n", W, H);
    for (int y = 0; y < H; ++y) {
        std::fwrite(buf.data() + static_cast<size_t>(y) * pitch, 1, pitch, fp);
    }

    std::fclose(fp);
    std::fprintf(stderr, "[dump] wrote %s\n", filename);
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
    vdp_cartridge_set_vcd_enabled(0, "dump.vcd"); // VCD is disabled at the beginning of the simualtion 

    // Inputs
    vdp_cartridge_set_button(0);
    vdp_cartridge_set_dipsw(0);

    // --------------------------------------------------------------------
    // Reset sequence (mirror tb.sv)
    // --------------------------------------------------------------------
    step_cycles(10);
    vdp_cartridge_reset();
    step_cycles(10);

    std::cout << "[main] Wait initialization (slot_wait deassert)\n";
    while (vdp_cartridge_get_slot_wait() == 1) {
        step_cycles(1);
    }
    step_cycles(10);
	
	// Reset sequence (元のまま)
	step_cycles(10);
	vdp_cartridge_reset();
	step_cycles(10);

	std::cout << "[main] Wait initialization (slot_wait deassert)\n";
	while (vdp_cartridge_get_slot_wait() == 1) {
		step_cycles(1);
	}
	step_cycles(10);
	step_cycles(1000);  // 追加



    // --------------------------------------------------------------------
    // Constants (same as tb.sv)
    // --------------------------------------------------------------------
    const uint16_t vdp_io0 = 0x88;            // vdp_io0 in tb.sv
    const uint16_t vdp_io1 = vdp_io0 + 0x01;  // vdp_io1 in tb.sv

    auto write_io = [](uint16_t addr, uint8_t data) {
        vdp_cartridge_write_io(addr, data);
    };

    // --------------------------------------------------------------------
    // SCREEN5 register setup (mirror tb.sv lines 483–511)
    // --------------------------------------------------------------------
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
    // --------------------------------------------------------------------
    std::cout << "[main] Write VRAM pattern (SCREEN5)\n";

    write_io(vdp_io1, 0x00);
    write_io(vdp_io1, 0x8E);
    write_io(vdp_io1, 0x00);
    write_io(vdp_io1, 0x40);
	
	const int vram_words = 128 * 32;  // 4096
	for (int i = 0; i < vram_words; ++i) {
		uint8_t val = static_cast<uint8_t>(i & 0xFF);

		// 元の VDP 経由の書き込み
		write_io(vdp_io0, val);
		step_cycles(4);  // deterministic small delay

		// ★ C++ 側 VRAM (g_vram) にも同じパターンを書き込む
		uint32_t word_addr = static_cast<uint32_t>(i);  // 単純に 1 word = 1 ステップと仮定
		uint32_t data32    = 0x01010101u * val;         // 4 バイト同じ値で埋める
		vdp_cartridge_dram_write(word_addr, data32, 0x0); // mask=0 -> 全バイト書く
	}

    // --- R#14 / second VRAM base setup (tb.sv 相当を追加) ----------------
    //
    // tb.sv:
    //   write_io( vdp_io1, 8'h01 );
    //   write_io( vdp_io1, 8'h8E );
    //   write_io( vdp_io1, 8'h00 );
    //   write_io( vdp_io1, 8'h76 );
    //
    // → R#14 = 1, MA14..MA0 = 0x07600 (sprite attribute table base)
    std::cout << "[main] SCREEN5: set R#14 for SAT base\n";
    write_io(vdp_io1, 0x01);
    write_io(vdp_io1, 0x8E);
    write_io(vdp_io1, 0x00);
    write_io(vdp_io1, 0x76);

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
    // Let the display run and dump VRAM / RGB frames
    // --------------------------------------------------------------------
    std::cout << "[main] Run display and dump VRAM / RGB frames\n";

    // ある程度回してからダンプ
    step_cycles(1368 * 16 * 10);  // ウォームアップ

    const int NUM_FRAMES_TO_DUMP = 3;
    for (int f = 0; f < NUM_FRAMES_TO_DUMP; ++f) {
        char fname_pgm[64];
        char fname_ppm[64];
        std::snprintf(fname_pgm, sizeof(fname_pgm), "frame_%03d.pgm", f);
        std::snprintf(fname_ppm, sizeof(fname_ppm), "frame_rgb_%03d.ppm", f);

        dump_vram_as_pgm(fname_pgm);
        dump_display_as_ppm(fname_ppm);

        std::printf("[main] Dumped %s and %s at sim_time=%" PRIu64 " ps\n",
                    fname_pgm, fname_ppm, vdp_cartridge_get_sim_time());

        // 次の「フレーム相当」まで少し進める
        step_cycles(1368 * 16 * 10);
    }

    // 少し余分に回してから終了
    step_cycles(1000);

    std::cout << "[main] All tests completed\n";

    vdp_cartridge_trace_close();
    vdp_cartridge_release();
    return 0;
}
