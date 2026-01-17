//
// ip_sdram_simple.v
//
//  Simple SDRAM behavioral model without delay elements / init timers.
//  Copyright (C) 2025 Takayuki Hara (original header retained)
//
//  This simplified model is intended for simulation where precise SDRAM
//  timing/initialization is not required. It implements an immediate
//  internal memory array and supports masked writes and reads.
//
//  Notes:
//  - No init / refresh / command timing. sdram_init_busy is de-asserted (0).
//  - bus_valid + bus_write performs a masked write in the same clock edge.
//    bus_wdata_mask bit == 0 => that byte is written (DQM polarity: 1 = mask).
//  - bus_valid + read returns data next clk with bus_rdata_en asserted for
//    one clock.
//  - External SDRAM pins are simplified/tied; IO_sdram_dq is high-Z from model.
//
//  If you want the same pin-level command mapping as the original model,
//  or different mask polarity, or connect IO_sdram_dq for interactive
//  transaction, I can adjust the implementation.
//
//-----------------------------------------------------------------------------

module ip_sdram #(
    parameter        FREQ = 85_909_080    // Hz (kept for compatibility)
) (
    input                reset_n,
    input                clk,                // CPU/domain clock
    input                clk_sdram,
    output               sdram_init_busy,

    input    [22:2]      bus_address,
    input                bus_valid,
    input                bus_write,
    input                bus_refresh,
    input    [31:0]      bus_wdata,
    input    [3:0]       bus_wdata_mask,
    output  [31:0]       bus_rdata,
    output               bus_rdata_en,

    // SDRAM ports (kept for compatibility; driven to simple values)
    output               O_sdram_clk,
    output               O_sdram_cke,
    output               O_sdram_cs_n,
    output               O_sdram_ras_n,
    output               O_sdram_cas_n,
    output               O_sdram_wen_n,
    inout   [31:0]       IO_sdram_dq,
    output  [10:0]       O_sdram_addr,
    output  [ 1:0]       O_sdram_ba,
    output  [ 3:0]       O_sdram_dqm
);

    // ADDRESS width: bus_address[22:2] => 21 bits -> depth 2^21 words
    localparam ADDR_WIDTH = 21;
    localparam DEPTH = (1 << ADDR_WIDTH);

    // Simple internal memory (behavioral). For simulation use only.
    reg [31:0] mem [0:DEPTH-1];

    // Read output registers
    reg [31:0] ff_rdata;
    reg        ff_rdata_en;

    // sdram init busy - always ready in this simple model
    assign sdram_init_busy = 1'b0;

    // SDRAM pins simplified/tied to static values
    assign O_sdram_clk  = clk_sdram;
    assign O_sdram_cke  = 1'b1;         // CKE asserted
    // Drive command pins to inactive (all high = deselect / no-op)
    assign O_sdram_cs_n = 1'b1;
    assign O_sdram_ras_n = 1'b1;
    assign O_sdram_cas_n = 1'b1;
    assign O_sdram_wen_n = 1'b1;

    assign O_sdram_dqm  = 4'b1111;     // no DQ driving/masking externally
    assign O_sdram_ba   = 2'd0;
    assign O_sdram_addr = 11'd0;

    // Do not drive external data bus in this model (tri-state)
    assign IO_sdram_dq = 32'bz;

    // Bus outputs
    assign bus_rdata    = ff_rdata;
    assign bus_rdata_en = ff_rdata_en;

    integer i;
    reg [31:0] cur;
    // Simplified memory operation:
    // - On posedge clk:
    //    * if reset: clear outputs
    //    * else if bus_valid & bus_write: perform masked write
    //    * else if bus_valid & !bus_write: latch read data and assert rdata_en next cycle
    //
    // Mask polarity: bus_wdata_mask bit == 0 -> write enabled for that byte.
    // (This corresponds to DQM=0 to allow write). Change if you prefer opposite polarity.

    always @(posedge clk) begin
        if (!reset_n) begin
            ff_rdata <= 32'd0;
            ff_rdata_en <= 1'b0;
        end
        else begin
            // Default: deassert read enable
            ff_rdata_en <= 1'b0;

            if (bus_valid && bus_refresh) begin
                // In this simple model, refresh request is ignored (no op).
                // Could implement refresh-related behavior here if needed.
                ff_rdata_en <= 1'b0;
            end
            else if (bus_valid && bus_write) begin
                // Masked write: bus_wdata_mask bit == 0 => write that byte
                // Read current word
                cur = mem[bus_address];
                // byte 0 = lowest 8 bits
                for (i = 0; i < 4; i = i + 1) begin
                    if (bus_wdata_mask[i] == 1'b0) begin
                        cur[(8*i) +: 8] = bus_wdata[(8*i) +: 8];
                    end
                end
                mem[bus_address] <= cur;
                // write does not produce bus_rdata_en in this model
                ff_rdata_en <= 1'b0;
            end
            else if (bus_valid && !bus_write) begin
                // Read: latch data and assert rdata_en for one cycle
                ff_rdata <= mem[bus_address];
                ff_rdata_en <= 1'b1;
            end
            else begin
                // no transaction
                ff_rdata_en <= 1'b0;
            end
        end
    end

endmodule