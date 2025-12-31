module ip_sdram #(
    parameter       FREQ = 85_909_080
) (
    input               reset_n,
    input               clk,
    input               clk_sdram,
    output              sdram_init_busy,
    input   [22:2]      bus_address,
    input               bus_valid,
    input               bus_write,
    input               bus_refresh,
    input   [31:0]      bus_wdata,
    input   [3:0]       bus_wdata_mask,
    output  [31:0]      bus_rdata,
    output              bus_rdata_en,
    output              O_sdram_clk,
    output              O_sdram_cke,
    output              O_sdram_cs_n,
    output              O_sdram_ras_n,
    output              O_sdram_cas_n,
    output              O_sdram_wen_n,
    inout   [31:0]      IO_sdram_dq,
    output  [10:0]      O_sdram_addr,
    output  [1:0]       O_sdram_ba,
    output  [3:0]       O_sdram_dqm
);
    assign IO_sdram_dq = 32'd0;
endmodule

