// src/verilator/vdp_cartridge_wrapper.cpp
// Clean wrapper for tangnano20k_vdp_cartridge:
// - Single, centralized clock/time model
// - Main clock ≒ 85.90908 MHz (VDP internal clock)
// - slot_clk is an integer divider of the main clock (for visibility only)
// - write_io() generates Z80-like I/O write cycles in half-cycle units
// - SDRAM is modeled as a simple functional VRAM array (timing ignored)

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
 * One full period ≒ 11.64 ns → 11,640 ps.
 */
static const uint64_t MAIN_CYCLE_PS = 11640ULL;              // 11.64 ns (85.9 MHz)
static const uint64_t HALF_CYCLE_PS = MAIN_CYCLE_PS / 2ULL;  // 5,820 ps

/* global sim time in ps */
static uint64_t g_time_ps    = 0;
static uint64_t g_last_dump  = (uint64_t)-1;

/* slot clock: purely derived, visible in VCD, does NOT drive DUT logic */
/* 12 main half-cycles per slot half-period
 * → 24 main half-cycles per slot full period
 * → f_slot ≈ 85.90908 MHz / 24 ≈ 3.5795 MHz
 */
static uint8_t  g_slot_clk         = 0;
static int64_t  g_halfcycle_count  = 0;
static const int SLOT_HALF_COUNT   = 12;

/* optional phase tracking (mostly for debugging / sanity checks) */
static int g_phase = -1;  // -1: last was negedge, 1: last was posedge, 0: init

/* logging controls */
static int g_debug_enabled = 0;  // verbose logs
static int g_mark_enabled  = 0;  // unused at the moment

/* write mode: 0 = old-style negedge (unused), 1 = posedge-based (not used now) */
static int g_write_on_posedge = 0;

/* end-align control (currently unused) */
static int g_end_align_enabled = 0;

/* -------------------------------------------------------------------------
 * Helpers: SDRAM model (simple functional model)
 * -------------------------------------------------------------------------*/
static void vram_write_word(uint32_t addr, uint32_t data, uint8_t mask)
{
    if (addr >= VRAM_WORD_COUNT) return;
    uint32_t cur = g_vram[addr];
    for (int i = 0; i < 4; ++i) {
        if (mask & (1u << i)) {
            ((uint8_t*)&cur)[i] = ((uint8_t*)&data)[i];
        }
    }
    g_vram[addr] = cur;
}

static uint32_t vram_read_word(uint32_t addr)
{
    if (addr >= VRAM_WORD_COUNT) return 0;
    return g_vram[addr];
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
 * Half-cycle step: single source of time / clock progression
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

    vdp_cartridge_vram_bus_eval();
    eval_and_dump_current_time();
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
 * SDRAM <-> VRAM functional bridge (timing-agnostic)
 * -------------------------------------------------------------------------*/
void vdp_cartridge_dram_write(uint32_t addr, uint32_t data, uint8_t mask)
{
    vram_write_word(addr, data, mask);
}

uint32_t vdp_cartridge_dram_read(uint32_t addr)
{
    return vram_read_word(addr);
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

/* Functional SDRAM model: watch DUT SDRAM pins and mirror to g_vram */
void vdp_cartridge_sdram_bus_eval(void)
{
    if (!g_top) return;

    uint32_t addr      = g_top->O_sdram_addr;
    uint32_t ba        = g_top->O_sdram_ba;
    uint32_t full_addr = (ba << 11) | addr;

    uint32_t wdata = g_top->IO_sdram_dq;
    uint8_t  dqm   = g_top->O_sdram_dqm;

    uint8_t cs_n  = g_top->O_sdram_cs_n;
    uint8_t ras_n = g_top->O_sdram_ras_n;
    uint8_t cas_n = g_top->O_sdram_cas_n;
    uint8_t we_n  = g_top->O_sdram_wen_n;

    // Simplified SDRAM command decode:
    // WRITE: Cs_n=0, Ras_n=1, Cas_n=0, We_n=0
    // READ : Cs_n=0, Ras_n=1, Cas_n=0, We_n=1
    bool is_write_cmd = (cs_n == 0 && ras_n == 1 && cas_n == 0 && we_n == 0);
    bool is_read_cmd  = (cs_n == 0 && ras_n == 1 && cas_n == 0 && we_n == 1);

    if (is_write_cmd) {
        vram_write_word(full_addr, wdata, dqm);
    } else if (is_read_cmd) {
        uint32_t rdata = vram_read_word(full_addr);
        // NOTE: in real SDRAM this would be delayed by CAS latency.
        g_top->IO_sdram_dq = rdata;
    }
}

/* -------------------------------------------------------------------------
 * write_io: generate Z80-like I/O timing in half-cycle units
 * -------------------------------------------------------------------------*/

// one full main clock cycle = 2 half-cycles
static inline void step_full_cycle(void)
{
    vdp_cartridge_step_clk_posedge();
    vdp_cartridge_step_clk_negedge();
}

void vdp_cartridge_write_io(uint16_t address, uint8_t wdata)
{
    if (!g_top) return;

    // ざっくり half-cycle 数（ModelSim の ns 計測から多めにマージン）
    const int H_ADDR_SETUP = 30;  // addr/data セット後、/WR アサートまで
    const int H_WR_WIDTH   = 40;  // /WR=0 の期間
    const int H_IORQ_DELAY = 4;   // /WR↑ から /IORQ↓ まで
    const int H_IORQ_WIDTH = 8;   // /IORQ=0 の期間
    const int H_RECOVERY   = 16;  // サイクル後のリカバリ

    // idle state
    g_top->slot_iorq_n      = 1;
    g_top->slot_wr_n        = 1;
    g_top->slot_rd_n        = 1;
    g_top->slot_data_dir    = 1;    // input
    g_top->cpu_drive_en     = 0;
    g_top->slot_a           = 0;
    g_top->cpu_ff_slot_data = 0;

    // 1. アドレス & データをセット、バスドライブ ON
    g_top->slot_a           = (uint8_t)(address & 0xFF);
    g_top->cpu_ff_slot_data = wdata;
    g_top->slot_data_dir    = 0;     // output to slot
    g_top->cpu_drive_en     = 1;

    for (int h = 0; h < H_ADDR_SETUP; ++h) {
        step_full_cycle();
    }

    // 2. /WR, /IORQ をアサート（tb.sv と同様ほぼ同時でよい）
    g_top->slot_wr_n   = 0;
    g_top->slot_iorq_n = 0;

    for (int h = 0; h < H_WR_WIDTH; ++h) {
        step_full_cycle();
    }

    // 3. /WR を先に戻す
    g_top->slot_wr_n = 1;

    for (int h = 0; h < H_IORQ_DELAY; ++h) {
        step_full_cycle();
    }

    // 4. /IORQ を戻す
    g_top->slot_iorq_n = 1;

    for (int h = 0; h < H_IORQ_WIDTH; ++h) {
        step_full_cycle();
    }

    // 5. バス開放＋リカバリ
    g_top->cpu_drive_en  = 0;
    g_top->slot_data_dir = 1;  // input

    for (int h = 0; h < H_RECOVERY; ++h) {
        step_full_cycle();
    }
}

/* -------------------------------------------------------------------------
 * Trace control & sim-time getters
 * -------------------------------------------------------------------------*/
int vdp_cartridge_trace_open(const char* path)
{
    if (!g_top) return -1;
    if (g_tfp) return 0;

    Verilated::traceEverOn(true);
    g_tfp = new VerilatedVcdC;

#ifdef VM_TRACE
    g_top->trace(g_tfp, 99);
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
 * dbg_vram_* bridge
 * -------------------------------------------------------------------------*/
void vdp_cartridge_vram_bus_eval(void)
{
    if (!g_top) return;

    uint32_t addr18   = g_top->dbg_vram_address;  // 18bit word address想定
    uint32_t wdata    = g_top->dbg_vram_wdata;
    uint8_t  valid    = g_top->dbg_vram_valid;
    uint8_t  write    = g_top->dbg_vram_write;
    uint8_t  rdata_en = g_top->dbg_vram_rdata_en; // 画面側では 0 かもしれない

    if (!valid) return;

    uint32_t word_addr = addr18;

    if (write) {
        if (word_addr < VRAM_WORD_COUNT) {
            g_vram[word_addr] = wdata;
        }
    } else {
        uint32_t rdata = 0;
        if (word_addr < VRAM_WORD_COUNT) {
            rdata = g_vram[word_addr];
        }
        g_top->dbg_vram_rdata = rdata;

        // --- デバッグログ: 先頭ページだけ軽く見る ---
        // 多量に出るので、必要に応じて if (g_debug_enabled) でガードしてください。
        if (word_addr < 0x0800) {
            fprintf(stderr,
                    "[VRAM-R] t=%" PRIu64 "ps addr=%05x data=%08x sel=%d bg=%d spr=%d cpu=%d cmd=%d\n",
                    g_time_ps,
                    word_addr,
                    rdata,
                    (int)g_top->u_dut__DOT__u_v9958__DOT__u_vram_interface__DOT__ff_vram_rdata_sel_d1,
                    (int)(g_top->u_dut__DOT__u_v9958__DOT__u_vram_interface__DOT__ff_vram_rdata_sel_d1 == 1),
                    (int)(g_top->u_dut__DOT__u_v9958__DOT__u_vram_interface__DOT__ff_vram_rdata_sel_d1 == 2),
                    (int)(g_top->u_dut__DOT__u_v9958__DOT__u_vram_interface__DOT__ff_vram_rdata_sel_d1 == 3),
                    (int)(g_top->u_dut__DOT__u_v9958__DOT__u_vram_interface__DOT__ff_vram_rdata_sel_d1 == 4));
        }
    }
}

/* -------------------------------------------------------------------------
 * Video helpers
 * -------------------------------------------------------------------------*/
void vdp_render_frame_rgb(uint8_t* dst, int pitch)
{
    if (!g_top || !dst) return;

    VdpVideoMode mode;
    vdp_get_video_mode(&mode);
    const int W = mode.width;
    const int H = mode.height;

    int x = 0;
    int y = 0;

    bool logged = false;

    while (y < H) {
        vdp_cartridge_step_clk_1cycle();

        if (!g_top->display_en) continue;

        uint8_t r = g_top->display_r;
        uint8_t g = g_top->display_g;
        uint8_t b = g_top->display_b;

        if (!logged && y < 4 && x < 16) {
            fprintf(stderr, "[PIX] y=%3d x=%3d rgb=%02x%02x%02x\n", y, x, r, g, b);
            if (y == 3 && x == 15) logged = true;
        }

        uint8_t* p = dst + y * pitch + x * 3;
        p[0] = r;
        p[1] = g;
        p[2] = b;

        if (++x >= W) {
            x = 0;
            ++y;
        }
    }
}