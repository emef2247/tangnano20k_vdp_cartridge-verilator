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
// Replace existing run_testpattern_csv with this updated implementation
// Replace or add this updated run_testpattern_csv implementation in main.cpp
// Supports IO,port,value (direct port write), IO,address,<value> (legacy),
// WRITE_ADDR / WRITE_DATA (legacy), ADDRESS, CYCLE, INFO records.
// Simple CSV runner: minimal, easy-to-read implementation.
// Replace the existing run_testpattern_csv in your main.cpp with this function.
//
// Behavior:
// - ADDRESS,0xHHHH        -> set internal 'address' variable (legacy support)
// - CYCLE,<n>             -> call step_cycles(n) (n already converted to wrapper cycles)
// - INFO,"...text..."      -> print the quoted text (or raw field) to stdout
// - IO,port,value         -> call vdp_cartridge_write_io(port, value)
//   - Also supports legacy form IO,address,value (writes to current 'address' and increments it)
//
// This keeps the replay faithful (always calls vdp_cartridge_write_io) and prints INFO lines
// exactly as they appear in the CSV, which is what you requested.
// Updated run_testpattern_csv with 0x9x -> 0x8x port mapping.
// Replace the previous run_testpattern_csv in main.cpp with this function.

// Updated run_testpattern_csv with 0x9x -> 0x8x port mapping.
// Replace the previous run_testpattern_csv in main.cpp with this function.

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
                std::string msg = fields[1];
                // remove surrounding quotes if present
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
            if (fields.size() < 3) {
                std::fprintf(stderr, "[CSV] line %" PRIu64 ": IO missing operands\n", line_no);
                continue;
            }

            // IO,port,value -> map 0x9x -> 0x8x before calling
            uint64_t port_v = 0;
            if (!parse_uint64_from_token(fields[1], port_v)) {
                std::fprintf(stderr, "[CSV] line %" PRIu64 ": IO port parse error '%s'\n", line_no, fields[1].c_str());
                continue;
            }
            uint8_t val = parse_u8_from_token(fields[2]);

            uint16_t orig_port = static_cast<uint16_t>(port_v & 0xFFFF);
            uint16_t mapped_port = orig_port;
            if ((orig_port & 0xF0) == 0x90) {
                // map 0x9x -> 0x8x by subtracting 0x10
                mapped_port = static_cast<uint16_t>(orig_port - 0x10);
            }

            // Replay the IO to the DUT using mapped port
            vdp_cartridge_write_io(mapped_port, val);
            std::fprintf(stderr, "[CSV] line %" PRIu64 ": IO write orig_port=0x%02x mapped_port=0x%02x value=0x%02x\n",
                        line_no, static_cast<int>(orig_port & 0xFF), static_cast<int>(mapped_port & 0xFF), val);
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

    // parse simple command-line option: --dump-screen or --dump-screen=0/1
    int requested_dump_screen = -1; // -1 = not specified, 0/1 explicit
    for (int ai = 1; ai < argc; ++ai) {
        const char* a = argv[ai];
        if (std::strcmp(a, "--dump-screen") == 0 || std::strcmp(a, "--dump_screen") == 0) {
            requested_dump_screen = 1;
        } else if (std::strncmp(a, "--dump-screen=", 14) == 0) {
            requested_dump_screen = std::atoi(a + 14) ? 1 : 0;
        } else if (std::strncmp(a, "--dump_screen=", 14) == 0) {
            requested_dump_screen = std::atoi(a + 14) ? 1 : 0;
        }
    }

    // --------------------------------------------------------------------
    // Init / trace
    // --------------------------------------------------------------------
    vdp_cartridge_init();

    // Apply command-line override for dump_screen (if given). This takes precedence
    // over DUMP_SCREEN environment variable which the wrapper also checks.
    if (requested_dump_screen != -1) {
        vdp_cartridge_set_dump_screen(requested_dump_screen);
        std::fprintf(stderr, "[main] dump_screen set to %d via command-line\n", requested_dump_screen);
    }
	
    vdp_cartridge_set_debug(0);
    vdp_cartridge_set_write_on_posedge(1);  // use tb.sv-like write_io
    vdp_cartridge_set_end_align(0);         // we handle phase in write_io
    vdp_cartridge_set_vcd_enabled(1, "dump.vcd"); // VCD is disabled at the beginning of the simualtion 

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
	

    // --------------------------------------------------------------------
    // TEST SCENARIO
    // --------------------------------------------------------------------
    std::cout << "[test] Test Scenario Start\n";
	// Converted test pattern (auto-generated)

	//run_testpattern_csv("./tests/csv/test_vdp_SCREEN1_SP.csv");
	//run_testpattern_csv("./tests/csv/test_vdp_SCREEN7_VRAM.csv");
	run_testpattern_csv("./tests/csv/frame_036.csv");
	

	step_cycles(1433664);  // 追加
    // --------------------------------------------------------------------
    // Let the display run and dump VRAM / RGB frames
    // --------------------------------------------------------------------
    std::cout << "[main] Run display and dump VRAM / RGB frames\n";

	char vram_ppm[64];
	char vram_screen5_pages[64];
	std::snprintf(vram_ppm, sizeof(vram_ppm), "vram_%03d.ppm", vdp_cartridge_get_frame_no());
	//std::snprintf(vram_screen5_pages, sizeof(vram_screen5_pages), "vram_screen5_%03d.ppm", vdp_cartridge_get_frame_no());

	dump_vram_as_ppm(vram_ppm);
	//dump_vram_screen5_pages(vram_screen5_pages);

    std::cout << "[main] All tests completed\n";

    vdp_cartridge_trace_close();
    vdp_cartridge_release();
    return 0;
}