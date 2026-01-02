// src/verilator/RTL/wrapper_top.sv
// slot_bridge + wrapper_top in one file to ensure wrapper_top exists for verilator
//
// Updated: expose slot_clk as an explicit top-level input so the C++ wrapper
// (vdp_cartridge_wrapper.cpp) can drive it and it will be included in the single
// VCD trace produced by Verilator. This keeps the DUT and CPU/slot clocks in the
// same trace file while avoiding changes to internal DUT RTL.
//
// Note: slot_clk is an input here and is NOT driven inside this RTL file.
// The C++ wrapper will set the corresponding port on the Verilated model so the
// signal appears in the main VCD. If the DUT (tangnano20k_vdp_cartridge)
// actually has a slot_clk port and you want the signal connected into the DUT,
// add the connection in the instantiation below (e.g. .slot_clk(slot_clk)).


// bridge: CPU-side drive -> slot_d
module slot_bridge (
    inout  [7:0] slot_d,           // connect to DUT's slot_d (inout)
    input  [7:0] cpu_ff_slot_data, // data from C++ (driver)
    input        cpu_drive_en      // when 1 -> bridge drives slot_d with cpu_ff_slot_data
);
    assign slot_d = cpu_drive_en ? cpu_ff_slot_data : 8'bz;
endmodule


// wrapper_top: instantiate the original DUT and the bridge.
// Keep port list matching original top where applicable.
module wrapper_top (
    input            clk,
    input            clk14m,

    // NEW: expose slot_clk so the C++ wrapper can drive it and it will be traced.
    // This is intentionally an input and currently left unconnected to internals
    // to avoid touching DUT RTL. If you want the DUT to receive slot_clk, connect
    // it in the DUT instantiation below (only if the DUT has such a port).
    input            slot_clk,

    input            slot_reset_n,
    input            slot_iorq_n,
    input            slot_rd_n,
    input            slot_wr_n,
    output           slot_wait,
    output           slot_intr,
    output           slot_data_dir,
    input  [7:0]     slot_a,
    inout  [7:0]     slot_d,       // external inout
    output           oe_n,
    input  [1:0]     dipsw,
    output           ws2812_led,
    input  [1:0]     button,

    // HDMI / SDRAM / other signals forwarded
    output           tmds_clk_p,
    output           tmds_clk_n,
    output [2:0]     tmds_d_p,
    output [2:0]     tmds_d_n,

    output           O_sdram_clk,
    output           O_sdram_cke,
    output           O_sdram_cs_n,
    output           O_sdram_ras_n,
    output           O_sdram_cas_n,
    output           O_sdram_wen_n,
    inout  [31:0]    IO_sdram_dq,
    output [10:0]    O_sdram_addr,
    output [1:0]     O_sdram_ba,
    output [3:0]     O_sdram_dqm,

    // CPU-side bridge ports — driven by C++ wrapper
    input  [7:0]     cpu_ff_slot_data,
    input            cpu_drive_en,

    // Debug VRAM bus exported to C++ wrapper
    output [17:0] dbg_vram_address,
    output [31:0] dbg_vram_wdata,
    input  [31:0] dbg_vram_rdata,
    output        dbg_vram_valid,
    output        dbg_vram_write,
    output        dbg_vram_rdata_en
);

    // Instantiate original DUT (connect slot_d to the same inout)
    // NOTE: Do NOT modify internal DUT ports here unless you are sure they exist.
    // If the DUT has a slot_clk input and you want to pass this top-level slot_clk
    // into it, add: .slot_clk (slot_clk),
    // in the port list below. That would connect the driver-provided slot_clk into DUT.
    tangnano20k_vdp_cartridge u_dut (
        .clk                (clk),
        .clk14m             (clk14m),
        .slot_reset_n       (slot_reset_n),
        .slot_iorq_n        (slot_iorq_n),
        .slot_rd_n          (slot_rd_n),
        .slot_wr_n          (slot_wr_n),
        .slot_wait          (slot_wait),
        .slot_intr          (slot_intr),
        .slot_data_dir      (slot_data_dir),
        .slot_a             (slot_a),
        .slot_d             (slot_d),
        .oe_n               (oe_n),
        .dipsw              (dipsw),
        .ws2812_led         (ws2812_led),
        .button             (button),

        .tmds_clk_p         (tmds_clk_p),
        .tmds_clk_n         (tmds_clk_n),
        .tmds_d_p           (tmds_d_p),
        .tmds_d_n           (tmds_d_n),

        .O_sdram_clk        (O_sdram_clk),
        .O_sdram_cke        (O_sdram_cke),
        .O_sdram_cs_n       (O_sdram_cs_n),
        .O_sdram_ras_n      (O_sdram_ras_n),
        .O_sdram_cas_n      (O_sdram_cas_n),
        .O_sdram_wen_n      (O_sdram_wen_n),
        .IO_sdram_dq        (IO_sdram_dq),
        .O_sdram_addr       (O_sdram_addr),
        .O_sdram_ba         (O_sdram_ba),
        .O_sdram_dqm        (O_sdram_dqm),
        // new debug VRAM ports
        .dbg_vram_address   (dbg_vram_address),
        .dbg_vram_wdata     (dbg_vram_wdata),
        .dbg_vram_rdata     (dbg_vram_rdata),
        .dbg_vram_valid     (dbg_vram_valid),
        .dbg_vram_write     (dbg_vram_write),
        .dbg_vram_rdata_en  (dbg_vram_rdata_en)
    );

    // Instantiate the bridge that drives slot_d when cpu_drive_en is asserted.
    slot_bridge u_bridge (
        .slot_d             (slot_d),
        .cpu_ff_slot_data   (cpu_ff_slot_data),
        .cpu_drive_en       (cpu_drive_en)
    );

    // Note:
    // - slot_clk is intentionally not connected into this wrapper unless the DUT
    //   requires it. It is exposed as a top-level input so the Verilated model
    //   has the signal and it will appear in the single VCD file when tracing.
    // - If you want to connect it into the DUT and the DUT has a port named
    //   slot_clk, modify the instantiation above to include:
    //     .slot_clk(slot_clk)
    //   Be sure to re-run Verilator so the generated model matches the RTL.

endmodule