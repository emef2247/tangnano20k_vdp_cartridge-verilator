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
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>
#include <cctype>
#include <limits>  
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

// ----------------------------------------------------------------------
// CSV-driven testpattern runner
// ----------------------------------------------------------------------
// Supported CSV records:
//   ADDRESS,0x0000[,0xHH,0xLL]  -> set internal address variable
//   CYCLE,<decimal>             -> call step_cycles(<decimal>)
//   INFO,"some text"            -> print the quoted text to cout
//   IO,address,0xVV[,0xVV]      -> write value to current address (or IO,<addr_hex>,<val>)
// Notes:
// - Fields may be quoted with " to include commas inside INFO string.
// - Hex parsing supports 0x.. or decimal numbers.
// - This function executes actions immediately (i.e. advances simulation).
// ----------------------------------------------------------------------

static inline std::string trim(const std::string &s) {
    size_t a = 0;
    while (a < s.size() && std::isspace((unsigned char)s[a])) ++a;
    size_t b = s.size();
    while (b > a && std::isspace((unsigned char)s[b-1])) --b;
    return s.substr(a, b - a);
}

// Simple CSV line splitter that understands double-quoted fields (no escapes).
static std::vector<std::string> split_csv_line(const std::string &line) {
    std::vector<std::string> out;
    std::string cur;
    bool inquote = false;
    for (size_t i = 0; i < line.size(); ++i) {
        char c = line[i];
        if (inquote) {
            if (c == '"') {
                // if a double quote is doubled, treat as literal quote ("" -> ")
                if (i + 1 < line.size() && line[i+1] == '"') {
                    cur.push_back('"');
                    ++i;
                } else {
                    inquote = false;
                }
            } else {
                cur.push_back(c);
            }
        } else {
            if (c == '"') {
                inquote = true;
            } else if (c == ',') {
                out.push_back(trim(cur));
                cur.clear();
            } else {
                cur.push_back(c);
            }
        }
    }
    out.push_back(trim(cur));
    return out;
}

static bool parse_uint64_from_token(const std::string &t, uint64_t &out)
{
    std::string s = trim(t);
    if (s.empty()) return false;
    char *endp = nullptr;
    // allow 0x.. hex
    if (s.size() > 2 && s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) {
        unsigned long long v = strtoull(s.c_str(), &endp, 16);
        if (endp == s.c_str()) return false;
        out = v;
        return true;
    } else {
        unsigned long long v = strtoull(s.c_str(), &endp, 10);
        if (endp == s.c_str()) return false;
        out = v;
        return true;
    }
}

static uint8_t parse_u8_from_token(const std::string &t)
{
    uint64_t v = 0;
    if (!parse_uint64_from_token(t, v)) return 0;
    return static_cast<uint8_t>(v & 0xFF);
}

// Execute actions described in CSV file (path). Returns true on success.
void run_testpattern_csv(const char* csv_path)
{
    std::ifstream ifs(csv_path);
    if (!ifs) {
        std::fprintf(stderr, "run_testpattern_csv: failed to open %s\n", csv_path);
        return;
    }

    uint16_t address = 0;
    std::string line;
    uint64_t line_no = 0;

    while (std::getline(ifs, line)) {
        ++line_no;
        std::string sline = trim(line);
        if (sline.empty()) continue;
        // skip comment lines starting with // or #
        if (sline.size() >= 2 && sline[0] == '/' && sline[1] == '/') continue;
        if (sline.size() >= 1 && sline[0] == '#') continue;

        auto fields = split_csv_line(sline);
        if (fields.empty()) continue;

        std::string cmd = fields[0];
        // uppercase cmd for safety
        for (auto &c: cmd) c = static_cast<char>(std::toupper((unsigned char)c));

        if (cmd == "ADDRESS") {
            if (fields.size() >= 2) {
                uint64_t v = 0;
                if (parse_uint64_from_token(fields[1], v)) {
                    address = static_cast<uint16_t>(v & 0xFFFF);
                    std::fprintf(stderr, "[CSV] line %" PRIu64 ": ADDRESS <- 0x%04x\n", line_no, address);
                } else {
                    std::fprintf(stderr, "[CSV] line %" PRIu64 ": ADDRESS parse error: '%s'\n", line_no, fields[1].c_str());
                }
            } else {
                std::fprintf(stderr, "[CSV] line %" PRIu64 ": ADDRESS missing operand\n", line_no);
            }
            continue;
        }

        if (cmd == "CYCLE") {
            if (fields.size() >= 2) {
                uint64_t v = 0;
                if (parse_uint64_from_token(fields[1], v)) {
                    // step_cycles takes int; clamp to INT_MAX if too large
                    const long long max_int = std::numeric_limits<int>::max();
                    if (v > static_cast<uint64_t>(max_int)) {
                        std::fprintf(stderr, "[CSV] line %" PRIu64 ": CYCLE value %" PRIu64 " too large, clamped to %lld\n", line_no, v, (long long)max_int);
                        step_cycles(static_cast<int>(max_int));
                    } else {
                        step_cycles(static_cast<int>(v));
                    }
                } else {
                    std::fprintf(stderr, "[CSV] line %" PRIu64 ": CYCLE parse error: '%s'\n", line_no, fields[1].c_str());
                }
            } else {
                std::fprintf(stderr, "[CSV] line %" PRIu64 ": CYCLE missing operand\n", line_no);
            }
            continue;
        }

        if (cmd == "INFO") {
            if (fields.size() >= 2) {
                // print the raw second field as-is (it may be quoted)
                std::string msg = fields[1];
                // If the message still has surrounding quotes, remove them
                if (msg.size() >= 2 && msg.front() == '"' && msg.back() == '"') {
                    msg = msg.substr(1, msg.size()-2);
                }
                std::cout << msg << std::endl;
            } else {
                std::fprintf(stderr, "[CSV] line %" PRIu64 ": INFO missing message\n", line_no);
            }
            continue;
        }

        if (cmd == "IO") {
            // two expected forms:
            // 1) IO,address,<value>   -> writes <value> to current address then address++
            // 2) IO,<addr_hex>,<value> -> writes <value> to specified addr (does NOT change internal address)
            if (fields.size() >= 3) {
                std::string f1 = fields[1];
                // lowercase
                for (auto &c: f1) c = static_cast<char>(std::tolower((unsigned char)c));
                if (f1 == "address") {
                    // IO,address,<value>
                    uint8_t v = parse_u8_from_token(fields[2]);
                    vdp_cartridge_write_io(address, v);
                    if (vdp_cartridge_get_sim_time) {
                        // optionally print debug
                        std::fprintf(stderr, "[CSV] IO write @address=0x%04x value=0x%02x\n", address, v);
                    }
                    address = (address + 1) & 0xFFFF;
                } else {
                    // IO,<addr_hex>,<value>
                    uint64_t a = 0;
                    if (!parse_uint64_from_token(fields[1], a)) {
                        std::fprintf(stderr, "[CSV] line %" PRIu64 ": IO address parse error '%s'\n", line_no, fields[1].c_str());
                        continue;
                    }
                    uint8_t v = 0;
                    if (fields.size() >= 3) v = parse_u8_from_token(fields[2]);
                    vdp_cartridge_write_io(static_cast<uint16_t>(a & 0xFFFF), v);
                    std::fprintf(stderr, "[CSV] IO write @address=0x%04x value=0x%02x\n", static_cast<int>(a & 0xFFFF), v);
                }
            } else {
                std::fprintf(stderr, "[CSV] line %" PRIu64 ": IO missing operands\n", line_no);
            }
            continue;
        }

        // Unknown command -> warn, ignore
        std::fprintf(stderr, "[CSV] line %" PRIu64 ": unknown cmd '%s'\n", line_no, cmd.c_str());
    }
}

// ----------------------------------------------------------------------
// g_vram を 256x212 の簡易 SCREEN5 4bpp とみなして PGM 出力
// (以下は既存コード、変更無し)
// ----------------------------------------------------------------------
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

// --- add these functions to main_logo.cpp (place near dump_vram_as_pgm) ---
static void dump_vram_as_ppm(const char* filename)
{
    const size_t VRAM_EXPECTED = 128 * 1024; // 131072
    const int W = 512;
    const int H = 256;

    void* buf = vdp_cartridge_get_vram_buffer();
    size_t size = vdp_cartridge_get_vram_size();
    const uint8_t* vram = reinterpret_cast<const uint8_t*>(buf);

    if (!vram || size == 0) {
        std::fprintf(stderr, "[dump] vram buffer is empty\n");
        return;
    }

    // ensure at least W*H bytes (pad with 0 if smaller)
    size_t need = static_cast<size_t>(W) * static_cast<size_t>(H);
    bool padded = false;
    std::vector<uint8_t> padbuf;
    if (size < need) {
        padbuf.resize(need);
        if (size > 0) {
            memcpy(padbuf.data(), vram, size);
        }
        // remainder is already zero-initialized
        vram = padbuf.data();
        padded = true;
    }

    FILE* fp = std::fopen(filename, "wb");
    if (!fp) {
        std::fprintf(stderr, "[dump] failed to open %s for write\n", filename);
        return;
    }

    // PPM header (binary P6)
    std::fprintf(fp, "P6\n%d %d\n255\n", W, H);

    // Write line by line into a temporary buffer (RGB triplets)
    std::vector<uint8_t> linebuf;
    linebuf.resize(W * 3);

    for (int y = 0; y < H; ++y) {
        size_t base = static_cast<size_t>(y) * static_cast<size_t>(W);
        for (int x = 0; x < W; ++x) {
            uint8_t v = vram[base + x];
            size_t idx = static_cast<size_t>(x) * 3;
            linebuf[idx + 0] = v;
            linebuf[idx + 1] = v;
            linebuf[idx + 2] = v;
        }
        size_t written = std::fwrite(linebuf.data(), 1, linebuf.size(), fp);
        if (written != linebuf.size()) {
            std::fprintf(stderr, "[dump] write error at line %d\n", y);
            break;
        }
    }

    std::fclose(fp);
    std::fprintf(stderr, "[dump] wrote %s (full VRAM view %dx%d) using %zu bytes%s\n",
                 filename, W, H, size, padded ? " (padded)" : "");
}

// Dump SCREEN5 pages: each page = 256x212, 4bpp (1 byte -> two pixels).
// Produces files: <basename>_page0.ppm ... <basename>_page3.ppm
static void dump_vram_screen5_pages(const char* basename)
{
    const int W = 256;
    const int H = 212;
    const size_t PAGE_BYTES = (W * H) / 2; // 27136
    void* buf = vdp_cartridge_get_vram_buffer();
    size_t size = vdp_cartridge_get_vram_size();
    const uint8_t* vram = reinterpret_cast<const uint8_t*>(buf);

    if (!vram || size == 0) {
        std::fprintf(stderr, "[dump] vram buffer is empty\n");
        return;
    }

    // There are 128K / PAGE_BYTES pages available (>=4); we'll export first 4 pages
    for (int p = 0; p < 4; ++p) {
        size_t offset = static_cast<size_t>(p) * PAGE_BYTES;
        if (offset + PAGE_BYTES > size) {
            std::fprintf(stderr, "[dump] skipping page %d: not enough data in VRAM\n", p);
            continue;
        }

        char fname[256];
        std::snprintf(fname, sizeof(fname), "%s_page%d.ppm", basename, p);

        FILE* fp = std::fopen(fname, "wb");
        if (!fp) {
            std::fprintf(stderr, "[dump] failed to open %s for write\n", fname);
            continue;
        }

        std::fprintf(fp, "P6\n%d %d\n255\n", W, H);

        std::vector<uint8_t> linebuf;
        linebuf.resize(W * 3);

        for (int y = 0; y < H; ++y) {
            size_t line_byte_base = offset + static_cast<size_t>(y) * (W / 2);
            for (int bx = 0; bx < (W / 2); ++bx) {
                uint8_t b = vram[line_byte_base + bx];
                uint8_t left = (b >> 4) & 0x0F;
                uint8_t right = b & 0x0F;
                // expand 4bit -> 8bit grayscale
                uint8_t l8 = static_cast<uint8_t>(left * 17);
                uint8_t r8 = static_cast<uint8_t>(right * 17);
                size_t x0 = static_cast<size_t>(2 * bx);
                size_t idx0 = x0 * 3;
                linebuf[idx0 + 0] = l8;
                linebuf[idx0 + 1] = l8;
                linebuf[idx0 + 2] = l8;
                size_t idx1 = (x0 + 1) * 3;
                linebuf[idx1 + 0] = r8;
                linebuf[idx1 + 1] = r8;
                linebuf[idx1 + 2] = r8;
            }
            size_t written = std::fwrite(linebuf.data(), 1, linebuf.size(), fp);
            if (written != linebuf.size()) {
                std::fprintf(stderr, "[dump] write error in %s at line %d\n", fname, y);
                break;
            }
        }

        std::fclose(fp);
        std::fprintf(stderr, "[dump] wrote %s (SCREEN5 page %d)\n", fname, p);
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
    vdp_cartridge_set_vcd_enabled(0, "dump.vcd"); // VCD is disabled at the beginning of the simualtion 

    // Inputs
    vdp_cartridge_set_button(0);
    vdp_cartridge_set_dipsw(0);

    // If you have a CSV testpattern file, you can run it here:
    // run_testpattern_csv("testpattern.csv");
    // (Uncomment and set path as needed)

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
    // Initiali Phase
    // --------------------------------------------------------------------
    const uint16_t vdp_io0 = 0x88;            // vdp_io0 in tb.sv
    const uint16_t vdp_io1 = vdp_io0 + 0x01;  // vdp_io1 in tb.sv

    auto write_io = [](uint16_t addr, uint8_t data) {
        vdp_cartridge_write_io(addr, data);
    };
	
	// Converted test pattern (auto-generated)
	// address variable holds VDP target address; initialized to current log state
	uint16_t address = 0x0000;
	std::cout << "frame=33 time=358416" << std::endl;
	step_cycles(1433664);
	std::cout << "frame=34 time=267804 port=0x99 value=0x00" << std::endl;
	step_cycles(1071216);
	std::cout << "frame=34 time=267960 port=0x99 value=0x92" << std::endl;
	step_cycles(624);
	address = 0x9200;  // set address (high=0x92 low=0x00)
	std::cout << "frame=34 time=267960 reg=0x12 val=0x00" << std::endl;
	std::cout << "frame=34 time=358416" << std::endl;
	step_cycles(361824);
	std::cout << "frame=35 time=317604 port=0x99 value=0x02" << std::endl;
	step_cycles(1270416);
	std::cout << "frame=35 time=317790 port=0x99 value=0x8f" << std::endl;
	step_cycles(744);
	address = 0x8f02;  // set address (high=0x8f low=0x02)
	std::cout << "frame=35 time=317790 reg=0x0f val=0x02" << std::endl;
	std::cout << "frame=35 time=358416" << std::endl;
	step_cycles(162504);
	

    // --------------------------------------------------------------------
    // TEST SCENARIO
    // --------------------------------------------------------------------
    std::cout << "[test] Test Scenario Start\n";
	// Converted test pattern (auto-generated)

	run_testpattern_csv("./tests/csv/frame_no080.csv");

    // --------------------------------------------------------------------
    // Let the display run and dump VRAM / RGB frames
    // --------------------------------------------------------------------
    std::cout << "[main] Run display and dump VRAM / RGB frames\n";

	char vram_ppm[64];
	char vram_screen5_pages[64];
	std::snprintf(vram_ppm, sizeof(vram_ppm), "vram_%03d.ppm", vdp_cartridge_get_frame_no());
	std::snprintf(vram_screen5_pages, sizeof(vram_screen5_pages), "vram_screen5_%03d.ppm", vdp_cartridge_get_frame_no());

	dump_vram_as_ppm(vram_ppm);
	dump_vram_screen5_pages(vram_screen5_pages);

    std::cout << "[main] All tests completed\n";

    vdp_cartridge_trace_close();
    vdp_cartridge_release();
    return 0;
}