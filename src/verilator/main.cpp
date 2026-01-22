// main.cpp — merged runner supporting both normal CSV-driven run and VRAM test mode
// Copyright (c) 2026 emef2247
// SPDX-License-Identifier: MIT
//
// Usage:
//   ./Vwrapper_top [--vramtest] [--dump-screen] [--csv=path] [--dump-screen=0|1]
//
// - --vramtest : run the VRAM fill/read-check scenario (from main_vramtest.cpp)
// - otherwise : normal mode (CSV-run or default scenario), then dump VRAM/PPM
//
// See project README for more usage notes.

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
// SystemC shim
// ----------------------------------------------------------------------
double sc_time_stamp() {
    return 0.0;
}

// ----------------------------------------------------------------------
// Utility: step cycles by calling wrapper clock step
// ----------------------------------------------------------------------
static void step_cycles(int cycles)
{
    for (int i = 0; i < cycles; ++i) {
        vdp_cartridge_step_clk_1cycle();
    }
}

// ----------------------------------------------------------------------
// CSV runner (single implementation used by both modes)
// ----------------------------------------------------------------------
static inline std::string trim(const std::string &s) {
    size_t a = 0;
    while (a < s.size() && std::isspace((unsigned char)s[a])) ++a;
    size_t b = s.size();
    while (b > a && std::isspace((unsigned char)s[b-1])) --b;
    return s.substr(a, b - a);
}

static std::vector<std::string> split_csv_line(const std::string &line) {
    std::vector<std::string> out;
    std::string cur;
    bool inquote = false;
    for (size_t i = 0; i < line.size(); ++i) {
        char c = line[i];
        if (inquote) {
            if (c == '"') {
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
        if (sline.size() >= 2 && sline[0] == '/' && sline[1] == '/') continue;
        if (sline.size() >= 1 && sline[0] == '#') continue;

        auto fields = split_csv_line(sline);
        if (fields.empty()) continue;

        std::string cmd = fields[0];
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

            uint64_t port_v = 0;
            if (!parse_uint64_from_token(fields[1], port_v)) {
                std::fprintf(stderr, "[CSV] line %" PRIu64 ": IO port parse error '%s'\n", line_no, fields[1].c_str());
                continue;
            }
            uint8_t val = parse_u8_from_token(fields[2]);

            uint16_t orig_port = static_cast<uint16_t>(port_v & 0xFFFF);
            uint16_t mapped_port = orig_port;
            if ((orig_port & 0xF0) == 0x90) {
                mapped_port = static_cast<uint16_t>(orig_port - 0x10);
            }

            vdp_cartridge_write_io(mapped_port, val);
            std::fprintf(stderr, "[CSV] line %" PRIu64 ": IO write orig_port=0x%02x mapped_port=0x%02x value=0x%02x\n",
                        line_no, static_cast<int>(orig_port & 0xFF), static_cast<int>(mapped_port & 0xFF), val);
            continue;
        }

        std::fprintf(stderr, "[CSV] line %" PRIu64 ": unknown cmd '%s'\n", line_no, cmd.c_str());
    }
}

// ----------------------------------------------------------------------
// VRAM / Display dump helpers (PGM/PPM) - kept from original files
// ----------------------------------------------------------------------
static void dump_vram_as_pgm(const char* filename)
{
    FILE* fp = std::fopen(filename, "wb");
    if (!fp) { std::fprintf(stderr, "Failed to open %s for write\n", filename); return; }

    const int W = 256, H = 212;
    std::fprintf(fp, "P5\n%d %d\n255\n", W, H);
    const uint8_t* vram_bytes = reinterpret_cast<const uint8_t*>(vdp_cartridge_get_vram_buffer());
    const int bytes_per_line = W / 2;
    for (int y = 0; y < H; ++y) {
        uint8_t line[W];
        for (int bx = 0; bx < bytes_per_line; ++bx) {
            uint8_t b = vram_bytes[y * bytes_per_line + bx];
            uint8_t left  = (b >> 4) & 0x0F;
            uint8_t right = b & 0x0F;
            line[2 * bx]     = static_cast<uint8_t>(left * 17);
            line[2 * bx + 1] = static_cast<uint8_t>(right * 17);
        }
        std::fwrite(line, 1, W, fp);
    }
    std::fclose(fp);
    std::fprintf(stderr, "[dump] wrote %s\n", filename);
}

static void dump_display_as_ppm(const char* filename)
{
    VdpVideoMode mode;
    vdp_get_video_mode(&mode);
    const int W = mode.width, H = mode.height, pitch = W * 3;
    std::vector<uint8_t> buf(static_cast<size_t>(pitch) * H);
    vdp_render_frame_rgb(buf.data(), pitch);
    FILE* fp = std::fopen(filename, "wb");
    if (!fp) { std::fprintf(stderr, "Failed to open %s for write\n", filename); return; }
    std::fprintf(fp, "P6\n%d %d\n255\n", W, H);
    for (int y = 0; y < H; ++y) std::fwrite(buf.data() + static_cast<size_t>(y) * pitch, 1, pitch, fp);
    std::fclose(fp);
    std::fprintf(stderr, "[dump] wrote %s\n", filename);
}

static void dump_vram_as_ppm(const char* filename)
{
    const int W = 512, H = 256;
    void* buf = vdp_cartridge_get_vram_buffer();
    size_t size = vdp_cartridge_get_vram_size();
    const uint8_t* vram = reinterpret_cast<const uint8_t*>(buf);
    if (!vram || size == 0) { std::fprintf(stderr, "[dump] vram buffer is empty\n"); return; }
    size_t need = static_cast<size_t>(W) * static_cast<size_t>(H);
    bool padded = false;
    std::vector<uint8_t> padbuf;
    if (size < need) {
        padbuf.resize(need);
        if (size > 0) memcpy(padbuf.data(), vram, size);
        vram = padbuf.data();
        padded = true;
    }
    FILE* fp = std::fopen(filename, "wb");
    if (!fp) { std::fprintf(stderr, "[dump] failed to open %s for write\n", filename); return; }
    std::fprintf(fp, "P6\n%d %d\n255\n", W, H);
    std::vector<uint8_t> linebuf(W * 3);
    for (int y = 0; y < H; ++y) {
        size_t base = static_cast<size_t>(y) * static_cast<size_t>(W);
        for (int x = 0; x < W; ++x) {
            uint8_t v = vram[base + x];
            size_t idx = static_cast<size_t>(x) * 3;
            linebuf[idx + 0] = v; linebuf[idx + 1] = v; linebuf[idx + 2] = v;
        }
        fwrite(linebuf.data(), 1, linebuf.size(), fp);
    }
    fclose(fp);
    std::fprintf(stderr, "[dump] wrote %s (full VRAM view %dx%d) using %zu bytes%s\n",
                 filename, W, H, size, padded ? " (padded)" : "");
}

static void dump_vram_screen5_pages(const char* basename)
{
    const int W = 256, H = 212;
    const size_t PAGE_BYTES = (W * H) / 2;
    void* buf = vdp_cartridge_get_vram_buffer();
    size_t size = vdp_cartridge_get_vram_size();
    const uint8_t* vram = reinterpret_cast<const uint8_t*>(buf);
    if (!vram || size == 0) { std::fprintf(stderr, "[dump] vram buffer is empty\n"); return; }
    for (int p = 0; p < 4; ++p) {
        size_t offset = static_cast<size_t>(p) * PAGE_BYTES;
        if (offset + PAGE_BYTES > size) { std::fprintf(stderr, "[dump] skipping page %d: not enough data in VRAM\n", p); continue; }
        char fname[256]; std::snprintf(fname, sizeof(fname), "%s_page%d.ppm", basename, p);
        FILE* fp = std::fopen(fname, "wb");
        if (!fp) { std::fprintf(stderr, "[dump] failed to open %s for write\n", fname); continue; }
        std::fprintf(fp, "P6\n%d %d\n255\n", W, H);
        std::vector<uint8_t> linebuf(W * 3);
        for (int y = 0; y < H; ++y) {
            size_t line_byte_base = offset + static_cast<size_t>(y) * (W / 2);
            for (int bx = 0; bx < (W / 2); ++bx) {
                uint8_t b = vram[line_byte_base + bx];
                uint8_t left = (b >> 4) & 0x0F;
                uint8_t right = b & 0x0F;
                uint8_t l8 = static_cast<uint8_t>(left * 17);
                uint8_t r8 = static_cast<uint8_t>(right * 17);
                size_t idx0 = (2 * bx) * 3;
                linebuf[idx0 + 0] = l8; linebuf[idx0 + 1] = l8; linebuf[idx0 + 2] = l8;
                size_t idx1 = (2 * bx + 1) * 3;
                linebuf[idx1 + 0] = r8; linebuf[idx1 + 1] = r8; linebuf[idx1 + 2] = r8;
            }
            fwrite(linebuf.data(), 1, linebuf.size(), fp);
        }
        fclose(fp);
        std::fprintf(stderr, "[dump] wrote %s (SCREEN5 page %d)\n", fname, p);
    }
}

// ----------------------------------------------------------------------
// Main: merged behavior
// ----------------------------------------------------------------------
int main(int argc, char** argv)
{
    // parse minimal command-line options
    bool vramtest_mode = false;
    int requested_dump_screen = -1; // -1: not specified, else 0/1
    std::string csv_path;
    for (int ai = 1; ai < argc; ++ai) {
        const char* a = argv[ai];
        if (std::strcmp(a, "--vramtest") == 0) {
            vramtest_mode = true;
        } else if (std::strcmp(a, "--dump-screen") == 0 || std::strcmp(a, "--dump_screen") == 0) {
            requested_dump_screen = 1;
        } else if (std::strncmp(a, "--dump-screen=", 14) == 0) {
            requested_dump_screen = std::atoi(a + 14) ? 1 : 0;
        } else if (std::strncmp(a, "--csv=", 6) == 0) {
            csv_path = std::string(a + 6);
        } else if (std::strncmp(a, "--dump_screen=", 14) == 0) {
            requested_dump_screen = std::atoi(a + 14) ? 1 : 0;
        }
    }

    // Init wrapper
    vdp_cartridge_init();

    if (requested_dump_screen != -1) {
        vdp_cartridge_set_dump_screen(requested_dump_screen);
        std::fprintf(stderr, "[main] dump_screen set to %d via command-line\n", requested_dump_screen);
    }

    vdp_cartridge_set_debug(0);
    vdp_cartridge_set_write_on_posedge(1);
    vdp_cartridge_set_end_align(0);

    // Select VCD behaviour similar to original files:
    if (vramtest_mode) {
        vdp_cartridge_set_vcd_enabled(1, "dump.vcd");
    } else {
        vdp_cartridge_set_vcd_enabled(0, "dump.vcd");
    }

    // Inputs
    vdp_cartridge_set_button(0);
    vdp_cartridge_set_dipsw(0);

    // Common reset/wait initialization sequence (mirror tb.sv)
    step_cycles(10);
    vdp_cartridge_reset();
    step_cycles(10);

    std::cout << "[main] Wait initialization (slot_wait deassert)\n";
    while (vdp_cartridge_get_slot_wait() == 1) step_cycles(1);
    step_cycles(10);

    // repeat reset sequence as in original mains
    step_cycles(10);
    vdp_cartridge_reset();
    step_cycles(10);
    std::cout << "[main] Wait initialization (slot_wait deassert)\n";
    while (vdp_cartridge_get_slot_wait() == 1) step_cycles(1);
    step_cycles(10);

    // extra warmup
    step_cycles(1000);

    // common VDP IO ports
    const uint16_t vdp_io0 = 0x88;
    const uint16_t vdp_io1 = vdp_io0 + 0x01;

    if (vramtest_mode) {
        // --- VRAM TEST SCENARIO (from main_vramtest.cpp) ---
        std::cout << "[test] Test Scenario Start\n";

        const uint32_t TEST_LEN = 0x10;

        // Initialize VDP registers (same sequence as earlier)
        vdp_cartridge_write_io(vdp_io1, 0x0A); vdp_cartridge_write_io(vdp_io1, 0x80);
        vdp_cartridge_write_io(vdp_io1, 0x43); vdp_cartridge_write_io(vdp_io1, 0x81);
        vdp_cartridge_write_io(vdp_io1, 0x1F); vdp_cartridge_write_io(vdp_io1, 0x82);
        vdp_cartridge_write_io(vdp_io1, 0xF7); vdp_cartridge_write_io(vdp_io1, 0x85);
        vdp_cartridge_write_io(vdp_io1, 0x1E); vdp_cartridge_write_io(vdp_io1, 0x86);
        vdp_cartridge_write_io(vdp_io1, 0x07); vdp_cartridge_write_io(vdp_io1, 0x87);
        vdp_cartridge_write_io(vdp_io1, 0x08); vdp_cartridge_write_io(vdp_io1, 0x88);
        vdp_cartridge_write_io(vdp_io1, 0x80); vdp_cartridge_write_io(vdp_io1, static_cast<uint8_t>(0x80 + 9));
        vdp_cartridge_write_io(vdp_io1, 0x01); vdp_cartridge_write_io(vdp_io1, static_cast<uint8_t>(0x80 + 11));
        vdp_cartridge_write_io(vdp_io1, 0x00); vdp_cartridge_write_io(vdp_io1, static_cast<uint8_t>(0x80 + 18));
        vdp_cartridge_write_io(vdp_io1, 0x00); vdp_cartridge_write_io(vdp_io1, static_cast<uint8_t>(0x80 + 19));
        vdp_cartridge_write_io(vdp_io1, 0x01); vdp_cartridge_write_io(vdp_io1, static_cast<uint8_t>(0x80 + 20));
        vdp_cartridge_write_io(vdp_io1, 0x00); vdp_cartridge_write_io(vdp_io1, static_cast<uint8_t>(0x80 + 21));
        vdp_cartridge_write_io(vdp_io1, 0x00); vdp_cartridge_write_io(vdp_io1, static_cast<uint8_t>(0x80 + 23));
        vdp_cartridge_write_io(vdp_io1, 0x00); vdp_cartridge_write_io(vdp_io1, static_cast<uint8_t>(0x80 + 25));
        vdp_cartridge_write_io(vdp_io1, 0x00); vdp_cartridge_write_io(vdp_io1, static_cast<uint8_t>(0x80 + 26));
        vdp_cartridge_write_io(vdp_io1, 0x00); vdp_cartridge_write_io(vdp_io1, static_cast<uint8_t>(0x80 + 27));

        // Fill VRAM: set VRAM write address then stream data into data port
        std::fprintf(stderr, "[TEST] Fill VRAM (len=0x%X)\n", TEST_LEN);
        vdp_cartridge_write_io(vdp_io1, 0x00);
        vdp_cartridge_write_io(vdp_io1, 0x8E);
        vdp_cartridge_write_io(vdp_io1, 0x00);
        vdp_cartridge_write_io(vdp_io1, 0x40);

        for (uint32_t i = 0; i < TEST_LEN; ++i) {
            uint8_t value = static_cast<uint8_t>(i & 0xFF);
            vdp_cartridge_write_io(vdp_io0, value);
            if ((i & 0xFF) == 0) {
                std::fprintf(stderr, "[TEST] wrote addr idx=0x%X value=0x%02x\n", i, value);
            }
        }

        // Read back & check
        std::fprintf(stderr, "[TEST] Read and Check VRAM (len=0x%X)\n", TEST_LEN);
        vdp_cartridge_write_io(vdp_io1, 0x00);
        vdp_cartridge_write_io(vdp_io1, 0x8E);
        vdp_cartridge_write_io(vdp_io1, 0x00);
        vdp_cartridge_write_io(vdp_io1, 0x40);

        bool ok = true;
        uint8_t dummy = vdp_cartridge_read_io(vdp_io0); // discard first
        for (uint32_t i = 0; i < TEST_LEN; ++i) {
            uint8_t read_data = vdp_cartridge_read_io(vdp_io0);
            uint8_t expect = static_cast<uint8_t>(i & 0xFF);
            if (read_data != expect) {
                std::fprintf(stderr, "[MISMATCH] idx=0x%X got=0x%02x expected=0x%02x time=%" PRIu64 "ps\n",
                             i, read_data, expect, vdp_cartridge_get_sim_time());
                ok = false;
            }
        }
        if (ok) std::fprintf(stderr, "[TEST] VRAM read-check OK (len=0x%X)\n", TEST_LEN);
        else    std::fprintf(stderr, "[TEST] VRAM read-check FAILED\n");

        // Let the display run and dump VRAM / RGB frames
        std::cout << "[main] Run display and dump VRAM / RGB frames\n";
        char vram_ppm[64];
        std::snprintf(vram_ppm, sizeof(vram_ppm), "vram_%03d.ppm", (int)vdp_cartridge_get_frame_no());
        dump_vram_as_ppm(vram_ppm);

    } else {
        // --- Normal mode ---
        // Optional CSV run if provided, else run a default CSV (if you want).
        if (!csv_path.empty()) {
            run_testpattern_csv(csv_path.c_str());
        } else {
            // You may choose to run a default CSV here; original main.cpp used fs_a1_10s.csv in some runs.
            run_testpattern_csv("./tests/csv/fsa1_deskpack.csv");
        }

        // After scenario, allow VDP to run and dump results
        std::cout << "[main] Run display and dump VRAM / RGB frames\n";
        char vram_ppm[64];
        std::snprintf(vram_ppm, sizeof(vram_ppm), "vram_%03d.ppm", (int)vdp_cartridge_get_frame_no());
        dump_vram_as_ppm(vram_ppm);
    }

    std::cout << "[main] All tests completed\n";
    vdp_cartridge_trace_close();
    vdp_cartridge_release();
    return 0;
}