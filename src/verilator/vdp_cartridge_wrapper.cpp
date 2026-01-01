// src/verilator/vdp_cartridge_wrapper.cpp
// Clean wrapper for tangnano20k_vdp_cartridge:
// - Single, centralized clock/time model
// - clk / clk14m are always 50% duty
// - slot_clk is an integer divider of the main clock (for visibility only)
// - write_io() approximates tb.sv's write_io timing in a simple half-cycle schedule
// - All MARK logs for edges are aligned to the actual edge time in the VCD

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

/* DUT clock parameters */
static const uint64_t MAIN_CYCLE_PS = 23270ULL;              // 23.270 ns
static const uint64_t HALF_CYCLE_PS = MAIN_CYCLE_PS / 2ULL;  // 11.635 ns

/* global sim time in ps */
static uint64_t g_time_ps    = 0;
static uint64_t g_last_dump  = (uint64_t)-1;

/* slot clock: purely derived, visible in VCD, does NOT drive DUT logic */
static uint8_t  g_slot_clk         = 0;
static int64_t  g_halfcycle_count  = 0;
/* 12 main half-cycles per slot half-period => slot_clk ≒ 3.57954MHz (from ~21.47727MHz) */
static const int SLOT_HALF_COUNT   = 12;

/* optional phase tracking (mostly for debugging / sanity checks) */
static int g_phase = -1;  // -1: last was negedge, 1: last was posedge, 0: init

/* logging controls */
static int g_debug_enabled = 0;  // verbose logs
static int g_mark_enabled  = 1;  // MARK logs for mapping (on by default)

/* write mode: 0 = old-style negedge (not implemented), 1 = posedge-based schedule */
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

    /* External clocks: first set pins to new level */
    g_clk    = new_clk;
    g_clk14m = new_clk;
    g_top->clk    = g_clk;
    g_top->clk14m = g_clk14m;

    g_halfcycle_count++;
    if (g_halfcycle_count >= SLOT_HALF_COUNT) {
        g_halfcycle_count = 0;
        g_slot_clk ^= 1u;
        g_top->slot_clk = g_slot_clk;
    }

    /* Now emit MARK: edge time is current g_time_ps, clk is already new level */
    if (g_mark_enabled) {
        const char* kind = level ? "POS_EDGE" : "NEG_EDGE";
        fprintf(stderr,
            "MARK: %s time=%" PRIu64 " prev_clk=%d new_clk=%d slot_clk=%d phase=%d\n",
            kind, g_time_ps, prev_clk, new_clk, g_slot_clk, g_phase);
    }

    if (g_debug_enabled) {
        fprintf(stderr, "HALF: time=%" PRIu64 "ps level=%d clk=%d slot_clk=%d\n",
                g_time_ps, level, g_clk, g_slot_clk);
    }

    eval_and_dump_current_time();
    g_time_ps += HALF_CYCLE_PS;
}


/* Convenience wrappers for posedge / negedge
 *
 * IMPORTANT:
 *  - MARK logs are emitted at the *edge time* (before time is advanced),
 *    so VCD edge and MARK time are aligned 1:1.
 *  - We no longer log separate ENTRY/EXIT; only POS_EDGE / NEG_EDGE.
 */
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

    g_top->slot_reset_n = 0;
    g_top->slot_iorq_n  = 1;
    g_top->slot_rd_n    = 1;
    g_top->slot_wr_n    = 1;
    g_top->slot_a       = 0;
    g_top->slot_data_dir = 1;
    g_top->dipsw        = 0;
    g_top->button       = 0;

    g_halfcycle_count   = 0;
    g_time_ps           = 0;
    g_last_dump         = (uint64_t)-1;
    g_phase             = -1;
    g_write_on_posedge  = 0;
    g_debug_enabled     = 0;
    g_mark_enabled      = 1;
    g_end_align_enabled = 0;

    /* Clear VRAM model */
    memset(g_vram, 0, sizeof(g_vram));

    /* Initial eval/dump at t=0 */
    eval_and_dump_current_time();

    if (g_mark_enabled) {
        fprintf(stderr, "MARK: INIT time=%" PRIu64 " clk=%d slot_clk=%d\n",
                g_time_ps, g_clk, g_slot_clk);
    }
}

void vdp_cartridge_release(void)
{
    if (!g_top) return;
    if (g_tfp) vdp_cartridge_trace_close();
    delete g_top;
    g_top = nullptr;
}

/* Simple reset sequence:
 * - hold slot_reset_n low for N cycles
 * - then high for N cycles
 * (Here N is fixed small; tb.sv uses repeat(10) before/after)
 */
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
 * SDRAM <-> VRAM functional bridge (simple, timing-agnostic)
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

void vdp_cartridge_sdram_bus_eval(void)
{
    if (!g_top) return;

    uint32_t bus_addr = g_top->O_sdram_addr;
    uint32_t bus_ba   = g_top->O_sdram_ba;
    uint32_t bus_address = (bus_ba << 11) | bus_addr;
    uint32_t wdata    = g_top->IO_sdram_dq;
    uint8_t  wmask    = g_top->O_sdram_dqm;

    uint8_t cs_n  = g_top->O_sdram_cs_n;
    uint8_t ras_n = g_top->O_sdram_ras_n;
    uint8_t cas_n = g_top->O_sdram_cas_n;
    uint8_t we_n  = g_top->O_sdram_wen_n;

    /* Very rough SDRAM write approximation: capture data on write command */
    if ((cs_n == 0) && (ras_n == 1) && (cas_n == 0) && (we_n == 0)) {
        vram_write_word(bus_address, wdata, wmask);
    }
}

/* -------------------------------------------------------------------------
 * write_io: approximate tb.sv's write_io timing in a simple half-cycle schedule
 * -------------------------------------------------------------------------*/
static inline int ns_to_halfs_ceil(int ns)
{
    if (ns <= 0) return 0;
    uint64_t ns_ps = (uint64_t)ns * 1000ULL;
    return (int)((ns_ps + HALF_CYCLE_PS - 1ULL) / HALF_CYCLE_PS);
}

void vdp_cartridge_write_io(uint16_t address, uint8_t wdata)
{
    if (!g_top) return;

    if (!g_write_on_posedge) {
        fprintf(stderr,
                "vdp_cartridge_write_io: negedge-mode not implemented; enable posedge mode.\n");
        return;
    }

    if (g_mark_enabled) {
        fprintf(stderr,
                "MARK: WRITE_START addr=0x%02x data=0x%02x time=%" PRIu64
                " clk=%d slot_clk=%d phase=%d\n",
                address & 0xFF, wdata, g_time_ps, g_clk, g_slot_clk, g_phase);
    }

    /* Translate tb.sv delays into half-cycle indices */
    const int t_addr_ns          = 170;
    const int t_wr_assert_ns     = 125;
    const int t_iorq_assert_ns   = 135;
    const int t_wr_deassert_ns   = 120;
    const int t_iorq_deassert_ns = 145;

    int addr_half        = ns_to_halfs_ceil(t_addr_ns);
    int wr_assert_half   = ns_to_halfs_ceil(t_wr_assert_ns);
    int iorq_assert_half = ns_to_halfs_ceil(t_iorq_assert_ns);
    int wr_deassert_half   = wr_assert_half   + ns_to_halfs_ceil(t_wr_deassert_ns);
    int iorq_deassert_half = iorq_assert_half + ns_to_halfs_ceil(t_iorq_deassert_ns);

    int total_halfs = iorq_deassert_half;
    if (wr_deassert_half > total_halfs) total_halfs = wr_deassert_half;
    if (addr_half        > total_halfs) total_halfs = addr_half;
    total_halfs += 8;  // slack after deassert

    /* Prepare idle state */
    g_top->slot_iorq_n      = 1;
    g_top->slot_wr_n        = 1;
    g_top->cpu_ff_slot_data = wdata;
    g_top->cpu_drive_en     = 0;
    g_top->slot_a           = 0;

    /* For determinism: each transaction starts and ends at clk==0 (low) */
    if (g_clk != 0) {
        vdp_cartridge_step_clk_negedge();
    }

    for (int h = 0; h < total_halfs; ++h) {
        /* Update bus signals at the beginning of this half-cycle */
        if (h == addr_half) {
            g_top->slot_a       = (uint8_t)(address & 0xFF);
            g_top->cpu_drive_en = 1;
        }

        if (h == wr_assert_half)     g_top->slot_wr_n   = 0;
        if (h == wr_deassert_half)   g_top->slot_wr_n   = 1;
        if (h == iorq_assert_half)   g_top->slot_iorq_n = 0;
        if (h == iorq_deassert_half) g_top->slot_iorq_n = 1;

        /* Main clock: even half -> posedge, odd half -> negedge */
        int level = (h & 1) ? 0 : 1;
        if (level) {
            vdp_cartridge_step_clk_posedge();
        } else {
            vdp_cartridge_step_clk_negedge();
        }
    }

    /* After the schedule, ensure we end at clk==0 (low) */
    if (g_clk != 0) {
        vdp_cartridge_step_clk_negedge();
    }

    /* Stop driving the bus */
    g_top->cpu_drive_en = 0;

    if (g_mark_enabled) {
        fprintf(stderr,
                "MARK: WRITE_END addr=0x%02x data=0x%02x time=%" PRIu64
                " clk=%d slot_clk=%d phase=%d slot_a=0x%02x slot_d=0x%02x\n",
                address & 0xFF, wdata, g_time_ps, g_clk, g_slot_clk, g_phase,
                (int)g_top->slot_a, (int)g_top->slot_d);
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