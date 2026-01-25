// vdp_cartridge_wrapper.cpp
// C++ wrapper around Verilated Vwrapper_top (tangnano20k_vdp_cartridge)
//
// - Provides a simple cycle-based API for driving the MSX slot bus
//   and observing the VDP cartridge behavior.
// - Implements a functional VRAM image (g_vram) for inspection only.
// - Monitor-only mode: the wrapper does NOT drive dbg_vram_rdata or
//   dbg_vram_rdata_en. Instead it observes dbg_vram_* outputs from the DUT
//   and keeps an internal copy of VRAM contents when it sees write requests.
//
// Notes:
// - This file exports a C API (extern "C") so main.o can link without
//   C++ name-mangling. Internal helpers remain C++.
// - Because the wrapper does not drive read responses any more, the
//   SDRAM model must be used in the simulation (ip_sdram_simple.v) to
//   produce vram_rdata_en and vram_rdata for vdp_vram_interface to latch.

// MOD/vdp_cartridge_wrapper.v
// --------------------------------------------------------------------
// Original: (upstream) vdp_cartridge_wrapper.v
// Copyright: preserved from original source
// SPDX-License-Identifier: (preserve original license in repository root)
//
// [MOD] 

#include "vdp_cartridge_wrapper.h"
#include "Vwrapper_top.h"
#include "verilated.h"
#include "verilated_vcd_c.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <limits.h>
#include <assert.h>
#include <inttypes.h>
#include <vector>
#include <algorithm>

#include <stdlib.h>
#include <cstdio>
#include <cstdint>

#ifdef __cplusplus
extern "C" {
#endif

/* -------------------------------------------------------------------------
 * Basic state / configuration
 * -------------------------------------------------------------------------*/
#define VRAM_WORD_COUNT (1u << 17)   // 128k words (matches original testbench SDRAM map)
static uint32_t g_vram[VRAM_WORD_COUNT];

static Vwrapper_top*   g_top  = nullptr;
static VerilatedVcdC*  g_tfp  = nullptr;

/* main DUT clock state (external pins) */
static uint8_t g_clk    = 0;
static uint8_t g_clk14m = 0;

/* DUT clock parameters
 * Main clock targets the internal VDP clock (clk85m ≒ 85.90908 MHz).
 * One full period ≒ 11.64 ns -> 11,640 ps.
 */
static const uint64_t MAIN_CYCLE_PS = 11640ULL;              // 11.64 ns (85.9 MHz)
static const uint64_t HALF_CYCLE_PS = MAIN_CYCLE_PS / 2ULL;  // 5,820 ps

/* global sim time in ps */
static uint64_t g_time_ps    = 0;
static uint64_t g_last_dump  = (uint64_t)-1;

/* slot clock: purely derived, visible in VCD, does NOT drive DUT logic */
static uint8_t  g_slot_clk         = 0;
static int64_t  g_halfcycle_count  = 0;
static const int SLOT_HALF_COUNT   = 2;

/* optional phase tracking (mostly for debugging / sanity checks) */
static int g_phase = -1;  // -1: last was negedge, 1: last was posedge, 0: init

/* logging controls */
static int g_debug_enabled = 0;  // verbose logs
static int g_mark_enabled  = 0;  // unused at the moment

/* write mode: 0 = old-style negedge (unused), 1 = posedge-based (not used now) */
static int g_write_on_posedge = 0;

/* end-align control (currently unused) */
static int g_end_align_enabled = 0;

/* trace depth of vcd */
static int g_vcd_dump_enabled = 1; // 1 = actually call g_tfp->dump(), 0 = suppress dump even if trace open
static int g_vcd_trace_depth = 1;

static int g_dump_screen = 0;   // if 1, dump each finished frame as .ppm

/* Setter to toggle dump_screen at runtime */
void vdp_cartridge_set_dump_screen(int enable)
{
    g_dump_screen = enable ? 1 : 0;
}

static uint64_t g_frame_no = 0;

void vdp_cartridge_set_dump_frame_no(uint64_t frame_no)
{
    g_frame_no = frame_no;
}

uint64_t vdp_cartridge_get_frame_no(void)
{
    return g_frame_no;
}

/* -------------------------------------------------------------------------
 * Helpers: VRAM image for inspection (monitor-only mode)
 * -------------------------------------------------------------------------*/
static void vram_write_word_monitored(uint32_t addr, uint32_t data)
{
    if (addr >= VRAM_WORD_COUNT) return;
    g_vram[addr] = data;
}

static uint32_t vram_read_word_monitored(uint32_t addr)
{
    if (addr >= VRAM_WORD_COUNT) return 0;
    return g_vram[addr];
}

void writePPM(
    const std::string& filename,
    unsigned width,
    unsigned height,
    const std::vector<uint32_t>& pixels
) {
    FILE* fp = fopen(filename.c_str(), "wb");
    if (!fp) return;

    // P6 header
    fprintf(fp, "P6\n%d %d\n255\n", width, height);

    for (unsigned y = 0; y < height; ++y) {
        for (unsigned x = 0; x < width; ++x) {
            uint32_t argb = pixels[y * width + x];
            uint8_t r = (argb >> 16) & 0xFF;
            uint8_t g = (argb >> 8 ) & 0xFF;
            uint8_t b = (argb      ) & 0xFF;
            fputc(r, fp);
            fputc(g, fp);
            fputc(b, fp);
        }
    }

    fclose(fp);
}

// -----------------------
//  VDP raw pixel capture
// -----------------------
struct FrameState {
    uint32_t logical_x = 0;
    uint32_t logical_y = 0;

    uint32_t width  = 0;
    uint32_t height = 0;

    uint32_t prev_pixel_x = 0xFFFFFFFF;
    uint32_t prev_pixel_y = 0xFFFFFFFF;
};

static FrameState g_frame;
static std::vector<uint32_t> g_framebuffer;

uint32_t packPixel(uint8_t r, uint8_t g, uint8_t b)
{
    return (0xFFu << 24) | (r << 16) | (g << 8) | b;
}

void vdp_cartridge_screen_pixel_sample(void)
{
    if (!g_top) return;

    const uint32_t x = g_top->pixel_pos_x;
    const uint32_t y = g_top->pixel_pos_y;

    const bool screen_in_active = g_top->screen_in_active;
    const bool intr_frame       = g_top->intr_frame;

    const uint8_t r = g_top->vdp_r;
    const uint8_t g = g_top->vdp_g;
    const uint8_t b = g_top->vdp_b;

    /* ===== Pixel tick detection (1 pixel == x changes) ===== */
    const bool pixel_tick = (x != g_frame.prev_pixel_x);
    g_frame.prev_pixel_x = x;

    /* ===== Capture pixel ===== */
    if (screen_in_active && pixel_tick) {

        /* --- New logical line --- */
        if (g_frame.prev_pixel_y != 0xFFFFFFFF &&
            y != g_frame.prev_pixel_y)
        {
            if (g_frame.width == 0) {
                // First completed line defines width
                g_frame.width = g_frame.logical_x;
            }
            g_frame.logical_x = 0;
            g_frame.logical_y++;
        }

        g_framebuffer.push_back(packPixel(r, g, b));
        g_frame.logical_x++;

        g_frame.prev_pixel_y = y;
    }

    /* ===== Frame end (intr_frame is last pixel!) ===== */
    if (intr_frame) {

        if (g_frame.width == 0) {
            g_frame.width = g_frame.logical_x;
        }
        g_frame.height = g_frame.logical_y + 1;

		if (g_dump_screen) {
			char name[64];
			sprintf(name, "frame_%06llu.ppm", (unsigned long long)g_frame_no);

			writePPM(
				name,
				g_frame.width,
				g_frame.height,
				g_framebuffer
			);
		}

        /* ---- Reset for next frame ---- */
        g_framebuffer.clear();
        g_frame = FrameState{};
        g_frame_no++;
    }
}


/* -------------------------------------------------------------------------
 * Centralized eval + trace dump at current g_time_ps (no time advance)
 * -------------------------------------------------------------------------*/
static inline void eval_and_dump_current_time(void)
{
    if (!g_top) return;

    g_top->eval();
#if VM_TRACE
    if (g_tfp) {
        if (g_time_ps != g_last_dump) {
            g_tfp->dump(g_time_ps);
            g_last_dump = g_time_ps;
        }
    }
#else
    (void)g_tfp;
#endif
}

/* -------------------------------------------------------------------------
 * Monitor-only dbg_vram_* bridge
 * - Observes dbg_vram_valid/ dbg_vram_write/ dbg_vram_wdata and updates
 *   the local g_vram image on writes. Does NOT drive dbg_vram_rdata or
 *   dbg_vram_rdata_en; those are expected to be produced by ip_sdram_simple.v.
 * - For reads it only logs the request for debugging/inspection.
 * -------------------------------------------------------------------------*/
void vdp_cartridge_vram_bus_eval(void)
{
    if (!g_top) return;

    uint32_t addr18 = (uint32_t)g_top->dbg_vram_address;
    uint32_t wdata  = (uint32_t)g_top->dbg_vram_wdata;
    uint8_t  valid  = g_top->dbg_vram_valid ? 1 : 0;
    uint8_t  write  = g_top->dbg_vram_write ? 1 : 0;

    if (valid) {
        uint32_t word_addr = addr18;
        if (write) {
            // Update local image for inspection (no drive-back to DUT)
            vram_write_word_monitored(word_addr, wdata);
            if (g_debug_enabled) {
                fprintf(stderr,
                        "[VRAM-MON-WR] t=%" PRIu64 "ps addr=%05x data=%08x\n",
                        g_time_ps, word_addr, wdata);
            }
        } else {
            // Read request observed: log only. ip_sdram_simple.v should respond.
            if (g_debug_enabled) {
                fprintf(stderr,
                        "[VRAM-MON-RD] t=%" PRIu64 "ps addr=%05x (observe only)\n",
                        g_time_ps, word_addr);
            }
        }
    }
}

/* -------------------------------------------------------------------------
 * Half-cycle step: single source of time / clock progression
 * - IMPORTANT: we evaluate DUT first (eval_and_dump_current_time) so
 *   DUT outputs (including dbg_vram_*) are stable when we monitor them.
 * -------------------------------------------------------------------------*/
static inline void step_halfcycle(int level)
{
    if (!g_top) return;

    uint8_t prev_clk = g_clk;
    uint8_t new_clk  = (uint8_t)(level ? 1 : 0);

    /* External clocks */
    g_clk    = new_clk;
    g_clk14m = new_clk;
    g_top->clk    = g_clk;
    g_top->clk14m = g_clk14m;

    /* Derived slot clock */
    g_halfcycle_count++;
    if (g_halfcycle_count >= SLOT_HALF_COUNT) {
        g_halfcycle_count = 0;
        g_slot_clk ^= 1u;
        g_top->slot_clk = g_slot_clk;
    }

    if (g_debug_enabled) {
        fprintf(stderr,
                "HALF: time=%" PRIu64 "ps prev_clk=%d new_clk=%d slot_clk=%d phase=%d\n",
                g_time_ps, prev_clk, new_clk, g_slot_clk, g_phase);
    }

    // 1) Evaluate DUT so dbg_vram_* and video signals reflect current half-cycle
    eval_and_dump_current_time();

    // 2) Monitor VRAM bus outputs from DUT (post-eval)
    vdp_cartridge_vram_bus_eval();

    // 3) Video sampling (sample after eval). 
    if (new_clk == 1) {
        if (g_top) {
			vdp_cartridge_screen_pixel_sample();   // VDP native
        }
    }

    g_time_ps += HALF_CYCLE_PS;
}

/* Convenience wrappers for posedge / negedge */
void vdp_cartridge_step_clk_posedge(void)
{
    if (!g_top) return;
    g_phase = 1;
    step_halfcycle(1);
}

void vdp_cartridge_step_clk_negedge(void)
{
    if (!g_top) return;
    g_phase = -1;
    step_halfcycle(0);
}

/* Runtime helpers: set/get VCD enable */
int vdp_cartridge_set_vcd_enabled(int enable, const char* path)
{
    if (!g_top) {
        fprintf(stderr, "vdp_cartridge_set_vcd_enabled: g_top not initialized\n");
        return -1;
    }

    if (enable) {
        if (g_tfp) return 0;
        return vdp_cartridge_trace_open(path);
    } else {
        if (g_tfp) {
            vdp_cartridge_trace_close();
        }
        return 0;
    }
}

int vdp_cartridge_is_vcd_enabled(void)
{
    return g_tfp ? 1 : 0;
}

/* -------------------------------------------------------------------------
 * Public API: init / release / reset
 * -------------------------------------------------------------------------*/
void vdp_cartridge_init(void)
{
    if (g_top) return;

    g_top = new Vwrapper_top();

    /* Initialize external pins */
    g_clk      = 0;
    g_clk14m   = 0;
    g_slot_clk = 0;

    g_top->clk         = g_clk;
    g_top->clk14m      = g_clk14m;
    g_top->slot_clk    = g_slot_clk;

    g_top->slot_reset_n  = 0;
    g_top->slot_iorq_n   = 1;
    g_top->slot_rd_n     = 1;
    g_top->slot_wr_n     = 1;
    g_top->slot_a        = 0;
    g_top->slot_data_dir = 1;
    g_top->dipsw         = 0;
    g_top->button        = 0;

    g_halfcycle_count   = 0;
    g_time_ps           = 0;
    g_last_dump         = (uint64_t)-1;
    g_phase             = -1;
    g_write_on_posedge  = 0;
    g_debug_enabled     = 0;
    g_mark_enabled      = 0;
    g_end_align_enabled = 0;

    memset(g_vram, 0, sizeof(g_vram));

    const char* env = getenv("DUMP_SCREEN");
    if (env) {
        int v = atoi(env);
        g_dump_screen = (v != 0) ? 1 : 0;
        if (g_dump_screen) {
            fprintf(stderr, "[init] DUMP_SCREEN env enabled\n");
        }
    }

    eval_and_dump_current_time();
}

void vdp_cartridge_release(void)
{
    if (!g_top) return;
    if (g_tfp) vdp_cartridge_trace_close();
    delete g_top;
    g_top = nullptr;
}

/* Simple reset sequence */
void vdp_cartridge_reset(void)
{
    if (!g_top) return;

    const int cycles = 8;

    g_top->slot_reset_n = 0;
    for (int i = 0; i < cycles; ++i) {
        vdp_cartridge_step_clk_posedge();
        vdp_cartridge_step_clk_negedge();
    }

    g_top->slot_reset_n = 1;
    for (int i = 0; i < cycles; ++i) {
        vdp_cartridge_step_clk_posedge();
        vdp_cartridge_step_clk_negedge();
    }
}

/* -------------------------------------------------------------------------
 * Simple setters / getters
 * -------------------------------------------------------------------------*/
void vdp_cartridge_set_button(uint8_t v)
{
    if (!g_top) return;
    g_top->button = (v & 0x3);
}

void vdp_cartridge_set_dipsw(uint8_t v)
{
    if (!g_top) return;
    g_top->dipsw = (v & 0x3);
}

void vdp_cartridge_set_write_on_posedge(int enable)
{
    g_write_on_posedge = enable ? 1 : 0;
}

void vdp_cartridge_set_debug(int enable)
{
    g_debug_enabled = enable ? 1 : 0;
}

void vdp_cartridge_set_end_align(int enable)
{
    g_end_align_enabled = enable ? 1 : 0;
}

void vdp_cartridge_set_mark_enabled(int enable)
{
    g_mark_enabled = enable ? 1 : 0;
}

/* -------------------------------------------------------------------------
 * SDRAM image helpers (inspection)
 * -------------------------------------------------------------------------*/
void vdp_cartridge_dram_write(uint32_t addr, uint32_t data, uint8_t mask)
{
    // helper that mirrors ip_sdram_simple semantics is intentionally not
    // used for DUT operation in monitor-only mode; keep for tests.
    vram_write_word_monitored(addr, data);
}

uint32_t vdp_cartridge_dram_read(uint32_t addr)
{
    return vram_read_word_monitored(addr);
}

void vdp_cartridge_dram_dump(FILE* fp)
{
    if (!fp) fp = stdout;
    for (uint32_t i = 0; i < 16; ++i) {
        fprintf(fp, "VRAM[%04x]=%08x\n", i, g_vram[i]);
    }
}

void* vdp_cartridge_get_vram_buffer(void)
{
    return (void*)g_vram;
}

size_t vdp_cartridge_get_vram_size(void)
{
    return VRAM_WORD_COUNT * sizeof(uint32_t);
}

void vdp_cartridge_sdram_bus_eval(void)
{
    // Monitor-only mode: leave legacy sdram eval unused.
}

/* -------------------------------------------------------------------------
 * Slot I/O helpers (unchanged)
 * -------------------------------------------------------------------------*/

// one full main clock cycle = 2 half-cycles
static inline void step_full_cycle(void)
{
    vdp_cartridge_step_clk_posedge();
    vdp_cartridge_step_clk_negedge();
}

uint8_t vdp_cartridge_read_io(uint16_t address)
{
    if (!g_top) return 0;

    // Timing parameters — keep in sync with write_io
    const int H_ADDR_SETUP = 30;  // address setup (half-cycle units)
    const int H_RD_WIDTH   = 40;  // /RD low period (half-cycle units)
    const int H_IORQ_DELAY = 4;   // delay between /RD up and /IORQ up
    const int H_IORQ_WIDTH = 8;   // extra cycles after /IORQ released
    const int H_RECOVERY   = 16;  // recovery cycles

    // Ensure CPU not driving the bus
    g_top->slot_iorq_n      = 1;
    g_top->slot_wr_n        = 1;
    g_top->slot_rd_n        = 1;
    g_top->slot_data_dir    = 1;   // indicate slot drives data (DUT -> CPU)
    g_top->cpu_drive_en     = 0;   // C++/CPU not driving
    g_top->cpu_ff_slot_data = 0;

    // Put address on bus (lower 8-bit as in write_io)
    g_top->slot_a = static_cast<uint8_t>(address & 0xFF);

    // hold address/setup
    for (int h = 0; h < H_ADDR_SETUP; ++h) {
        step_full_cycle();
    }

    // Assert /RD and /IORQ (drive read)
    g_top->slot_rd_n   = 0;
    g_top->slot_iorq_n = 0;

    for (int h = 0; h < H_RD_WIDTH; ++h) {
        step_full_cycle();
    }

    // Sample data from DUT-driven slot_d (inout in wrapper_top -> available as g_top->slot_d)
    uint8_t data = static_cast<uint8_t>(g_top->slot_d & 0xFF);

    // Deassert /RD first
    g_top->slot_rd_n = 1;

    for (int h = 0; h < H_IORQ_DELAY; ++h) {
        step_full_cycle();
    }

    // Deassert /IORQ
    g_top->slot_iorq_n = 1;

    for (int h = 0; h < H_IORQ_WIDTH; ++h) {
        step_full_cycle();
    }

    // Recovery
    for (int h = 0; h < H_RECOVERY; ++h) {
        step_full_cycle();
    }

    if (g_debug_enabled) {
        std::fprintf(stderr, "[IO-RD ] port=0x%02x data=0x%02x t=%" PRIu64 "ps\n",
                     address & 0xFF, data, g_time_ps);
    }

    return data;
}

void vdp_cartridge_write_io(uint16_t address, uint8_t wdata)
{
    if (!g_top) return;

    // approximate half-cycle counts (tuned conservatively)
    const int H_ADDR_SETUP = 30;  // addr/data set before /WR assert
    const int H_WR_WIDTH   = 40;  // /WR low period
    const int H_IORQ_DELAY = 4;   // delay between /WR high and /IORQ high
    const int H_IORQ_WIDTH = 8;   // extra cycles after /IORQ released
    const int H_RECOVERY   = 16;  // recovery cycles

    // idle state
    g_top->slot_iorq_n      = 1;
    g_top->slot_wr_n        = 1;
    g_top->slot_rd_n        = 1;
    g_top->slot_data_dir    = 1;    // input
    g_top->cpu_drive_en     = 0;
    g_top->slot_a           = 0;
    g_top->cpu_ff_slot_data = 0;

    // 1. Address & data set, enable CPU drive
    g_top->slot_a           = (uint8_t)(address & 0xFF);
    g_top->cpu_ff_slot_data = wdata;
    g_top->slot_data_dir    = 0;     // output to slot
    g_top->cpu_drive_en     = 1;

    for (int h = 0; h < H_ADDR_SETUP; ++h) {
        step_full_cycle();
    }

    // 2. Assert /WR and /IORQ (near-simultaneous)
    g_top->slot_wr_n   = 0;
    g_top->slot_iorq_n = 0;

    for (int h = 0; h < H_WR_WIDTH; ++h) {
        step_full_cycle();
    }

    // 3. Release /WR first
    g_top->slot_wr_n = 1;

    for (int h = 0; h < H_IORQ_DELAY; ++h) {
        step_full_cycle();
    }

    // 4. Release /IORQ
    g_top->slot_iorq_n = 1;

    for (int h = 0; h < H_IORQ_WIDTH; ++h) {
        step_full_cycle();
    }

    // 5. Release bus + recovery
    g_top->cpu_drive_en  = 0;
    g_top->slot_data_dir = 1;  // input

    for (int h = 0; h < H_RECOVERY; ++h) {
        step_full_cycle();
    }
}

/* -------------------------------------------------------------------------
 * Trace control & sim-time getters
 * -------------------------------------------------------------------------*/
void vdp_cartridge_set_vcd_dump(int enable)
{
    g_vcd_dump_enabled = enable ? 1 : 0;
}

void vdp_cartridge_set_vcd_depth(int depth) {
    if (depth < 0) depth = 0;
    g_vcd_trace_depth = depth;
}

int vdp_cartridge_trace_open(const char* path)
{
    if (!g_top) return -1;
    if (g_tfp) return 0;

    Verilated::traceEverOn(true);
    g_tfp = new VerilatedVcdC;

	fprintf(stderr, "[VCD] VCD is enabled. Name:\"%s\"=, depth:%d\n", path ? path : "dump.vcd", g_vcd_trace_depth);
#ifdef VM_TRACE
	// Use configured trace depth so we can limit to top-level (wrapper_top)
    g_top->trace(g_tfp, g_vcd_trace_depth);
    g_tfp->open(path ? path : "dump.vcd");
    return 0;
#else
    delete g_tfp;
    g_tfp = nullptr;
    return -1;
#endif
}

void vdp_cartridge_trace_close(void)
{
    if (!g_tfp) return;
#ifdef VM_TRACE
    g_tfp->close();
    delete g_tfp;
    g_tfp = nullptr;
#else
    delete g_tfp;
    g_tfp = nullptr;
#endif
}

uint64_t vdp_cartridge_get_sim_time(void)
{
    return g_time_ps;
}

uint8_t vdp_cartridge_get_slot_wait(void)
{
    if (!g_top) return 0;
    return g_top->slot_wait ? 1 : 0;
}

/* -------------------------------------------------------------------------
 * Video helpers
 * -------------------------------------------------------------------------*/

// Temporary: fixed SCREEN5 timing (256x212).
void vdp_get_video_mode(VdpVideoMode* out)
{
    if (!out) return;
    out->width  = 256;
    out->height = 212;
}

/* -------------------------------------------------------------------------
 * Video frame capture
 * -------------------------------------------------------------------------*/

#ifdef __cplusplus
} // extern "C"
#endif

// End of file