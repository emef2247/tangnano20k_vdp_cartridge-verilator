// src/verilator/RTL/wrapper_top.sv
// slot_bridge + wrapper_top in one file to ensure wrapper_top exists for verilator

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
    input            cpu_drive_en
);

    // Instantiate original DUT (connect slot_d to the same inout)
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
        .O_sdram_dqm        (O_sdram_dqm)
    );

    // Instantiate the bridge that drives slot_d when cpu_drive_en is asserted.
    slot_bridge u_bridge (
        .slot_d             (slot_d),
        .cpu_ff_slot_data   (cpu_ff_slot_data),
        .cpu_drive_en       (cpu_drive_en)
    );

endmodule