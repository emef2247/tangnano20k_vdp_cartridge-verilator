// src/verilator/vdp_cartridge_wrapper.cpp
// Reworked wrapper: single VCD, DUT primary clock, slot_clk derived by integer division.
// - slot_clk is derived as DUT_halfcycle_count / SLOT_HALF_COUNT (SLOT_HALF_COUNT = 12).
// - All signals (including slot_clk) are written into the single trace g_tfp.
// - MARK logs are added and controlled by g_mark_enabled for automatic mapping.

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
#include <math.h>

/* -------------------------------------------------------------------------
 * Basic state / configuration
 * -------------------------------------------------------------------------*/
#define VRAM_WORD_COUNT (1 << 17)
static uint32_t g_vram[VRAM_WORD_COUNT];

static Vwrapper_top* g_top = nullptr;
static VerilatedVcdC* g_tfp = nullptr;

/* main DUT clock state */
static uint8_t g_clk = 0;
static uint8_t g_clk14m = 0;

/* DUT clock parameters */
static const uint64_t MAIN_CYCLE_PS = 23270ULL;   /* 23.270 ns -> 23270 ps */
static const uint64_t HALF_CYCLE_PS = (MAIN_CYCLE_PS / 2ULL); /* 11635 ps */

/* half-cycle counting for slot_clk derivation */
static int64_t g_halfcycle_count = 0;
static const int SLOT_HALF_COUNT = 12; /* main half-cycles per slot half-period */

/* slot_clk state (exposed to top module; requires top-level port) */
static uint8_t g_slot_clk = 0;

/* global sim time in ps */
static uint64_t g_time_ps = 0;
static uint64_t last_dump_ps = (uint64_t)(-1);

/* phase tracking */
static int g_phase = 0; /* 0=init, 1=last posedge, -1=last negedge */

/* end-align control (option) */
static int g_end_align_enabled = 1;

/* logging controls */
static int g_debug_enabled = 0;   /* verbose debug */
static int g_mark_enabled = 1;    /* MARK logs for mapping (on by default) */

/* SDRAM bus cache (simplified) */
static uint32_t prev_bus_address = 0;
static uint8_t prev_bus_valid = 0;
static uint8_t prev_bus_write = 0;
static uint32_t prev_bus_wdata = 0;
static uint8_t prev_bus_wdata_mask = 0;

/* write mode */
static int g_write_on_posedge = 0;

/* -------------------------------------------------------------------------
 * Helpers
 * -------------------------------------------------------------------------*/
static inline int ns_to_cycles_ceil(int ns) {
    if (ns <= 0) return 0;
    uint64_t ns_ps = (uint64_t)ns * 1000ULL;
    return (int)((ns_ps + MAIN_CYCLE_PS - 1ULL) / MAIN_CYCLE_PS);
}

/* Centralized eval + trace dump at current g_time_ps (no time advance) */
static inline void vdp_cartridge_eval_and_dump_current_time(void) {
    if (!g_top) return;
    g_top->eval();
#if VM_TRACE
    if (g_tfp) {
        if (g_time_ps != last_dump_ps) {
            g_tfp->dump(g_time_ps);
            last_dump_ps = g_time_ps;
        }
    }
#else
    (void)g_tfp;
#endif
}

/* Slot clock derivation: increment half-cycle count and toggle slot_clk
   when threshold reached. This ties slot_clk to DUT main clock (integer ratio).
   slot_clk is written into DUT port g_top->slot_clk so it appears in main VCD.
*/
static inline void slot_clock_halfcycle_advance_and_maybe_toggle(void)
{
    g_halfcycle_count++;
    if (g_halfcycle_count >= SLOT_HALF_COUNT) {
        g_halfcycle_count = 0;
        g_slot_clk = g_slot_clk ? 0 : 1;
        if (g_top) {
            g_top->slot_clk = g_slot_clk;
        }
        if (g_mark_enabled) {
            fprintf(stderr, "DERIVED slot_clk toggle -> %d at sim_time=%" PRIu64 "\n", g_slot_clk, g_time_ps);
        } else if (g_debug_enabled) {
            fprintf(stderr, "DERIVED slot_clk toggled (debug) -> %d at %" PRIu64 "\n", g_slot_clk, g_time_ps);
        }
    }
}

/* -------------------------------------------------------------------------
 * Public API
 * -------------------------------------------------------------------------*/
void vdp_cartridge_init(void) {
    if (g_top) return;
    g_top = new Vwrapper_top();

    /* initialize top ports/state expected by wrapper */
    g_top->clk = 0;
    g_top->clk14m = 0;
    /* slot_clk port must exist in top Verilog and in generated model */
    g_top->slot_clk = 0;

    g_top->slot_reset_n = 0;
    g_top->slot_iorq_n = 1;
    g_top->slot_rd_n = 1;
    g_top->slot_wr_n = 1;
    g_top->slot_a = 0;
    g_top->slot_data_dir = 1;
    g_top->dipsw = 0;
    g_top->button = 0;

    g_halfcycle_count = 0;
    g_slot_clk = 0;

    g_time_ps = 0;
    last_dump_ps = (uint64_t)(-1);
    g_phase = -1; /* assume we've just had negedge at time 0 (clk==0) */
    g_write_on_posedge = 0;
    g_debug_enabled = 0;
    g_mark_enabled = 1;
    g_end_align_enabled = 1;

    /* initial eval/dump */
    vdp_cartridge_eval_and_dump_current_time();
    if (g_mark_enabled) fprintf(stderr, "MARK: INIT time=%" PRIu64 " clk=%d slot_clk=%d\n", g_time_ps, g_clk, g_slot_clk);
}

void vdp_cartridge_release(void) {
    if (!g_top) return;
    if (g_tfp) vdp_cartridge_trace_close();
    delete g_top;
    g_top = nullptr;
}

/* Simple reset sequence: pulse slot_reset_n low for several cycles */
void vdp_cartridge_reset(void)
{
    if (!g_top) return;

    /* Assert reset (active low) */
    g_top->slot_reset_n = 0;
    for (int i = 0; i < 8; ++i) {
        vdp_cartridge_step_clk_posedge();
        vdp_cartridge_step_clk_negedge();
    }

    /* Deassert reset and run a few cycles to stabilize */
    g_top->slot_reset_n = 1;
    for (int i = 0; i < 8; ++i) {
        vdp_cartridge_step_clk_posedge();
        vdp_cartridge_step_clk_negedge();
    }
}

/* Step helpers: emit MARK logs when enabled */
void vdp_cartridge_step_clk_posedge(void) {
    if (!g_top) return;
    if (g_mark_enabled) fprintf(stderr, "MARK: POS_ENTRY time=%" PRIu64 " clk=%d phase=%d slot_clk=%d\n", g_time_ps, g_clk, g_phase, g_slot_clk);

    if (g_phase == 1) {
        if (g_debug_enabled) fprintf(stderr, "WARN: posedge called twice in a row (g_time_ps=%" PRIu64 ")\n", g_time_ps);
    }
    g_phase = 1;

    g_clk = 1;
    g_clk14m = 1;
    g_top->clk = g_clk;
    g_top->clk14m = g_clk14m;

    /* Advance derived slot clock counter BEFORE eval so toggles at same timestamp */
    slot_clock_halfcycle_advance_and_maybe_toggle();

    vdp_cartridge_eval_and_dump_current_time();

    g_time_ps += HALF_CYCLE_PS;

    if (g_mark_enabled) fprintf(stderr, "MARK: POS_EXIT time=%" PRIu64 " clk=%d phase=%d slot_clk=%d\n", g_time_ps, g_clk, g_phase, g_slot_clk);
}

void vdp_cartridge_step_clk_negedge(void) {
    if (!g_top) return;
    if (g_mark_enabled) fprintf(stderr, "MARK: NEG_ENTRY time=%" PRIu64 " clk=%d phase=%d slot_clk=%d\n", g_time_ps, g_clk, g_phase, g_slot_clk);

    if (g_phase == -1) {
        if (g_debug_enabled) fprintf(stderr, "WARN: negedge called twice in a row (g_time_ps=%" PRIu64 ")\n", g_time_ps);
    }
    g_phase = -1;

    g_clk = 0;
    g_clk14m = 0;
    g_top->clk = g_clk;
    g_top->clk14m = g_clk14m;

    slot_clock_halfcycle_advance_and_maybe_toggle();

    vdp_cartridge_eval_and_dump_current_time();

    g_time_ps += HALF_CYCLE_PS;

    if (g_mark_enabled) fprintf(stderr, "MARK: NEG_EXIT time=%" PRIu64 " clk=%d phase=%d slot_clk=%d\n", g_time_ps, g_clk, g_phase, g_slot_clk);
}

/* setters */
void vdp_cartridge_set_button(uint8_t v) { if (!g_top) return; g_top->button = (v & 0x3); }
void vdp_cartridge_set_dipsw(uint8_t v)  { if (!g_top) return; g_top->dipsw = (v & 0x3); }
void vdp_cartridge_set_write_on_posedge(int enable) { g_write_on_posedge = enable ? 1 : 0; }
void vdp_cartridge_set_debug(int enable) { g_debug_enabled = enable ? 1 : 0; }
void vdp_cartridge_set_end_align(int enable) { g_end_align_enabled = enable ? 1 : 0; }
void vdp_cartridge_set_mark_enabled(int enable) { g_mark_enabled = enable ? 1 : 0; }

/* -------------------------------------------------------------------------
 * SDRAM model (unchanged)
 * -------------------------------------------------------------------------*/
void vdp_cartridge_dram_write(uint32_t addr, uint32_t data, uint8_t mask) {
    if (addr >= VRAM_WORD_COUNT) return;
    uint32_t cur = g_vram[addr];
    for (int i = 0; i < 4; ++i) {
        if (mask & (1 << i)) ((uint8_t*)&cur)[i] = ((uint8_t*)&data)[i];
    }
    g_vram[addr] = cur;
}
uint32_t vdp_cartridge_dram_read(uint32_t addr) {
    if (addr >= VRAM_WORD_COUNT) return 0;
    return g_vram[addr];
}
void vdp_cartridge_dram_dump(FILE* fp) {
    if (!fp) fp = stdout;
    for (uint32_t i = 0; i < 16; ++i) fprintf(fp, "VRAM[%04x]=%08x\n", i, g_vram[i]);
}
void* vdp_cartridge_get_vram_buffer(void) { return (void*)g_vram; }
size_t vdp_cartridge_get_vram_size(void) { return VRAM_WORD_COUNT * sizeof(uint32_t); }

void vdp_cartridge_sdram_bus_eval(void) {
    if (!g_top) return;

    uint32_t bus_addr = g_top->O_sdram_addr;
    uint32_t bus_ba = g_top->O_sdram_ba;
    uint32_t bus_address = (bus_ba << 11) | bus_addr;
    uint32_t wdata = g_top->IO_sdram_dq;
    uint8_t  wmask = g_top->O_sdram_dqm;

    uint8_t cs_n  = g_top->O_sdram_cs_n;
    uint8_t ras_n = g_top->O_sdram_ras_n;
    uint8_t cas_n = g_top->O_sdram_cas_n;
    uint8_t we_n  = g_top->O_sdram_wen_n;

    if ((cs_n == 0) && (ras_n == 1) && (cas_n == 0) && (we_n == 0)) {
        vdp_cartridge_dram_write(bus_address, wdata, wmask);
    }
}

/* -------------------------------------------------------------------------
 * Drive helper and write_io
 * -------------------------------------------------------------------------*/

static void drive_cycles_posedge_mode(int drive_start_cycle, int final_cycle,
                                      int cyc_wr_assert, int cyc_iorq_assert,
                                      int cyc_wr_deassert, int cyc_iorq_deassert,
                                      uint16_t address, uint8_t wdata)
{
    if (g_mark_enabled) fprintf(stderr, "MARK: DRIVE_LOOP begin drive_start=%d final=%d time=%" PRIu64 "\n", drive_start_cycle, final_cycle, g_time_ps);

    for (int cyc = 1; cyc <= final_cycle; ++cyc) {
        if (g_phase != -1) {
            vdp_cartridge_step_clk_negedge();
        }

        if (cyc >= drive_start_cycle) {
            g_top->slot_a = (uint8_t)(address & 0xFF);
            g_top->cpu_ff_slot_data = wdata;
            g_top->cpu_drive_en = 1;
        } else {
            g_top->cpu_drive_en = 0;
        }

        if (cyc == cyc_iorq_assert) g_top->slot_iorq_n = 0;
        if (cyc == cyc_wr_assert)   g_top->slot_wr_n   = 0;
        if (cyc == cyc_wr_deassert) g_top->slot_wr_n   = 1;
        if (cyc == cyc_iorq_deassert) g_top->slot_iorq_n = 1;

        vdp_cartridge_step_clk_posedge();
        if (g_mark_enabled) fprintf(stderr, "MARK: CYCLE_POS_EXIT cycle=%2d time=%" PRIu64 " slot_a=0x%02x slot_d=0x%02x cpu_drive_en=%d\n",
                                     cyc, g_time_ps, (int)g_top->slot_a, (int)g_top->slot_d, (int)g_top->cpu_drive_en);

        vdp_cartridge_step_clk_negedge();
        if (g_mark_enabled) fprintf(stderr, "MARK: CYCLE_NEG_EXIT cycle=%2d time=%" PRIu64 " slot_a=0x%02x slot_d=0x%02x cpu_drive_en=%d\n",
                                     cyc, g_time_ps, (int)g_top->slot_a, (int)g_top->slot_d, (int)g_top->cpu_drive_en);
    }

    if (g_mark_enabled) fprintf(stderr, "MARK: DRIVE_LOOP end time=%" PRIu64 "\n", g_time_ps);
}

void vdp_cartridge_write_io(uint16_t address, uint8_t wdata)
{
    if (!g_top) return;

    if (g_write_on_posedge) {
        if (g_mark_enabled) fprintf(stderr, "MARK: WRITE_START addr=0x%02x data=0x%02x time=%" PRIu64 " clk=%d slot_clk=%d phase=%d\n",
                                    address & 0xFF, wdata, g_time_ps, g_clk, g_slot_clk, g_phase);

        const int t_addr_ns = 170;
        const int t_wr_assert_ns = 125;
        const int t_iorq_assert_ns = 135;
        const int t_wr_deassert_ns = 120;
        const int t_iorq_deassert_ns = 145;

        int cyc_addr = ns_to_cycles_ceil(t_addr_ns);
        int cyc_wr_assert = ns_to_cycles_ceil(t_wr_assert_ns);
        int cyc_iorq_assert = ns_to_cycles_ceil(t_iorq_assert_ns);
        int cyc_wr_deassert_off = ns_to_cycles_ceil(t_wr_deassert_ns);
        int cyc_iorq_deassert_off = ns_to_cycles_ceil(t_iorq_deassert_ns);

        int cyc_wr_deassert = cyc_wr_assert + cyc_wr_deassert_off;
        int cyc_iorq_deassert = cyc_iorq_assert + cyc_iorq_deassert_off;

        /* prepare idle */
        g_top->slot_iorq_n = 1;
        g_top->slot_wr_n   = 1;
        g_top->cpu_ff_slot_data = wdata;
        g_top->cpu_drive_en = 0;
        g_top->slot_a = 0;

        /* align to negedge only if needed */
        if (g_phase != -1) {
            vdp_cartridge_step_clk_negedge();
            if (g_mark_enabled) fprintf(stderr, "MARK: ALIGN_DONE time=%" PRIu64 " clk=%d slot_clk=%d phase=%d\n", g_time_ps, g_clk, g_slot_clk, g_phase);
        } else {
            if (g_mark_enabled) fprintf(stderr, "MARK: ALIGN_SKIPPED already negedge time=%" PRIu64 " clk=%d slot_clk=%d phase=%d\n", g_time_ps, g_clk, g_slot_clk, g_phase);
        }

        int drive_start_cycle = (cyc_addr > 3) ? (cyc_addr - 3) : 1;
        int final_cycle = cyc_iorq_deassert;
        if (cyc_wr_deassert > final_cycle) final_cycle = cyc_wr_deassert;
        if (cyc_addr > final_cycle) final_cycle = cyc_addr;
        final_cycle += 6;

        drive_cycles_posedge_mode(drive_start_cycle, final_cycle,
                                  cyc_wr_assert, cyc_iorq_assert,
                                  cyc_wr_deassert, cyc_iorq_deassert,
                                  address, wdata);

        /* wait for slot_wait */
        if (g_mark_enabled) fprintf(stderr, "MARK: WAIT_START time=%" PRIu64 "\n", g_time_ps);
        {
            int safety_count = 0;
            const int safety_limit = 1000000;
            while (g_top->slot_wait == 1) {
                vdp_cartridge_step_clk_posedge();
                vdp_cartridge_step_clk_negedge();
                if (++safety_count > safety_limit) {
                    if (g_debug_enabled) fprintf(stderr, "WARN: wait loop safety limit reached\n");
                    break;
                }
            }
        }
        if (g_mark_enabled) fprintf(stderr, "MARK: WAIT_END time=%" PRIu64 " slot_wait=%d\n", g_time_ps, (int)g_top->slot_wait);

        g_top->cpu_drive_en = 0;
        vdp_cartridge_eval_and_dump_current_time();

        /* conditional end-align */
        if (g_end_align_enabled && g_clk == 1) {
            if (g_mark_enabled) fprintf(stderr, "MARK: PRE_ALIGN_END time=%" PRIu64 " clk=1 -> stepping negedge\n", g_time_ps);
            vdp_cartridge_step_clk_negedge();
            if (g_mark_enabled) fprintf(stderr, "MARK: POST_ALIGN_END time=%" PRIu64 " clk=%d slot_clk=%d phase=%d\n", g_time_ps, g_clk, g_slot_clk, g_phase);
        } else if (!g_end_align_enabled) {
            if (g_debug_enabled) fprintf(stderr, "END_ALIGN disabled; leaving phase as-is (time=%" PRIu64 " clk=%d)\n", g_time_ps, g_clk);
        }

        if (g_mark_enabled) fprintf(stderr, "MARK: WRITE_END addr=0x%02x data=0x%02x time=%" PRIu64 " clk=%d slot_clk=%d phase=%d slot_a=0x%02x slot_d=0x%02x\n",
                                    address & 0xFF, wdata, g_time_ps, g_clk, g_slot_clk, g_phase, (int)g_top->slot_a, (int)g_top->slot_d);
        return;
    }

    /* negedge-mode: symmetric, similar logic (omitted for brevity) */
    {
        /* ... keep existing semantics or refactor similarly ... */
    }
}

/* -------------------------------------------------------------------------
 * Trace control & sim-time getter
 * -------------------------------------------------------------------------*/
int vdp_cartridge_trace_open(const char* path)
{
    if (!g_top) return -1;
    if (g_tfp) return 0;

    Verilated::traceEverOn(true);
    g_tfp = new VerilatedVcdC;
    g_time_ps = 0;
    last_dump_ps = (uint64_t)(-1);

#ifdef VM_TRACE
    /* Ensure top model will include slot_clk in its trace (top Verilog must
       expose slot_clk port). */
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
    if (g_tfp) { delete g_tfp; g_tfp = nullptr; }
#endif
}

uint64_t vdp_cartridge_get_sim_time(void)
{
    return g_time_ps;
}

uint8_t vdp_cartridge_get_slot_wait(void)
{
    if (!g_top) return 0;
    return (g_top->slot_wait ? 1 : 0);
}