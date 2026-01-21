// ip_sdram_simple.v — Tiny SDRAM model for V9968 RTL
// Copyright (c) 2026 emef2247
// SPDX-License-Identifier: MIT
//
// Minimal, Verilator-friendly SDRAM model used for V9968 RTL testing.
// See LICENSE in the repository root for full text.
// Contact: emef2247 (project repository)

// ip_sdram_simple_tiny.v — Tiny, clk-only SDRAM-like model (minimal for Verilator)
// Backwards-compatible: accepts FREQ parameter and bus_wdata_mask port (mask is honored here)
// - Very small/fast model: minimal features, but supports byte-mask writes and small DEPTH.
// - All logic on posedge clk. clk_sdram port retained for compatibility/tracing only.

module ip_sdram #(
    parameter integer FREQ = 85_909_080,   // kept for compatibility (ignored)
    parameter integer ADDR_WIDTH    = 16,  // internal mem index width (default 16 -> 64K words)
    parameter integer RDATA_PULSE   = 1,
    parameter integer CAS_LAT       = 2,
    parameter integer RESPONSE_DELAY= 5,
    parameter integer EN_DELAY      = 0
) (
    input                reset_n,
    input                clk,
    input                clk_sdram,      // retained (not used internally)
    output               sdram_init_busy,

    input    [22:2]      bus_address,
    input                bus_valid,
    input                bus_write,
    input                bus_refresh,
    input    [31:0]      bus_wdata,
    input    [3:0]       bus_wdata_mask, // now honored: 1 = masked (no write), 0 = write
    output  [31:0]       bus_rdata,
    output reg           bus_rdata_en,

    // static SDRAM pins (driven to constants)
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

    // simple checks
    initial begin
        if (CAS_LAT < 1) begin
            $display("ERROR: CAS_LAT must be >= 1");
            $finish;
        end
    end

    localparam DEPTH = (1 << ADDR_WIDTH);

    // small memory; index taken from bus_address[ADDR_WIDTH+1:2]
    reg [31:0] mem [0:DEPTH-1];

    // pipeline sized by CAS_LAT
    reg pipeline_valid [0:CAS_LAT-1];
    reg [22:2] pipeline_addr [0:CAS_LAT-1];
    reg next_pipeline_valid [0:CAS_LAT-1];
    reg [22:2] next_pipeline_addr [0:CAS_LAT-1];

    // outputs / pending
    reg [31:0] ff_rdata;
    reg pending;
    reg [31:0] pending_data;
    integer counter;

    // static pin assignments
    assign sdram_init_busy = 1'b0;
    assign O_sdram_clk  = clk;   // traceable
    assign O_sdram_cke  = 1'b1;
    assign O_sdram_cs_n = 1'b1;
    assign O_sdram_ras_n = 1'b1;
    assign O_sdram_cas_n = 1'b1;
    assign O_sdram_wen_n = 1'b1;
    assign O_sdram_dqm  = 4'b1111;
    assign O_sdram_ba   = 2'd0;
    assign O_sdram_addr = 11'd0;
    assign IO_sdram_dq = 32'bz;
    assign bus_rdata = ff_rdata;

    integer i, st;

    // initial (note: mem not explicitly zeroed to avoid long init; tests typically write before read)
    initial begin
        ff_rdata = 32'd0;
        pending = 1'b0;
        pending_data = 32'd0;
        counter = 0;
        bus_rdata_en = 1'b0;
        for (st = 0; st < CAS_LAT; st = st + 1) begin
            pipeline_valid[st] = 1'b0;
            pipeline_addr[st]  = {21{1'b0}};
        end
    end

    // main pipeline & counter on posedge clk
    always @(posedge clk) begin
        if (!reset_n) begin
            for (st = 0; st < CAS_LAT; st = st + 1) begin
                pipeline_valid[st] <= 1'b0;
                pipeline_addr[st]  <= {21{1'b0}};
            end
            pending <= 1'b0;
            pending_data <= 32'd0;
            counter <= 0;
            ff_rdata <= 32'd0;
            bus_rdata_en <= 1'b0;
        end else begin
            // default: clear en
            bus_rdata_en <= 1'b0;

            // 1) WRITE with byte-mask support (mask bit == 0 -> write that byte)
            if (bus_valid && bus_write) begin
                // compute internal index using lower ADDR_WIDTH bits of bus_address
                reg [ADDR_WIDTH-1:0] idx;
                reg [31:0] cur;
                idx = bus_address[ADDR_WIDTH+1:2];
                cur = mem[idx];
                if (bus_wdata_mask[0] == 1'b0) cur[ 7: 0] = bus_wdata[ 7: 0];
                if (bus_wdata_mask[1] == 1'b0) cur[15: 8] = bus_wdata[15: 8];
                if (bus_wdata_mask[2] == 1'b0) cur[23:16] = bus_wdata[23:16];
                if (bus_wdata_mask[3] == 1'b0) cur[31:24] = bus_wdata[31:24];
                mem[idx] <= cur;
`ifdef SDRAM_DEBUG
                $display("[IP_SDRAM-TINY-WR] t=%0t addr=%06x mem_idx=%0d mask=%b wdata=%08x -> mem=%08x",
                         $time, {bus_address,2'b00}, idx, bus_wdata_mask, bus_wdata, cur);
`endif
            end

            // 2) shift CAS pipeline (left shift)
            for (st = 0; st < CAS_LAT-1; st = st + 1) begin
                next_pipeline_valid[st] = pipeline_valid[st+1];
                next_pipeline_addr[st]  = pipeline_addr[st+1];
            end
            next_pipeline_valid[CAS_LAT-1] = 1'b0;
            next_pipeline_addr[CAS_LAT-1]  = {21{1'b0}};

            // inject read into last stage if requested
            if (bus_valid && !bus_write && !bus_refresh) begin
                next_pipeline_valid[CAS_LAT-1] = 1'b1;
                next_pipeline_addr[CAS_LAT-1]  = bus_address;
`ifdef SDRAM_DEBUG
                $display("[IP_SDRAM-TINY-INJ] t=%0t inject read addr=%06x into stage=%0d (CAS=%0d)",
                         $time, {bus_address,2'b00}, CAS_LAT-1, CAS_LAT);
`endif
            end

            // commit CAS pipeline
            for (st = 0; st < CAS_LAT-1; st = st + 1) begin
                pipeline_valid[st] <= next_pipeline_valid[st];
                pipeline_addr[st]  <= next_pipeline_addr[st];
            end
            pipeline_valid[CAS_LAT-1] <= next_pipeline_valid[CAS_LAT-1];
            pipeline_addr[CAS_LAT-1]  <= next_pipeline_addr[CAS_LAT-1];

            // 3) service stage 0: read mem into pending when valid
            if (pipeline_valid[0]) begin
                reg [ADDR_WIDTH-1:0] midx;
                midx = pipeline_addr[0][ADDR_WIDTH+1:2];
                pending <= 1'b1;
                pending_data <= mem[midx];
                counter <= 0;
                pipeline_valid[0] <= 1'b0;
`ifdef SDRAM_DEBUG
                $display("[IP_SDRAM-TINY-ENQ] t=%0t addr=%06x mem_idx=%0d data=%08x",
                         $time, {pipeline_addr[0],2'b00}, midx, mem[midx]);
`endif
            end else begin
                if (pending) begin
                    if (counter < RESPONSE_DELAY - 1 + EN_DELAY) begin
                        counter <= counter + 1;
                    end else begin
                        // load rdata and assert en (clk-only)
                        ff_rdata <= pending_data;
                        bus_rdata_en <= 1'b1;
                        pending <= 1'b0;
`ifdef SDRAM_DEBUG
                        $display("[IP_SDRAM-TINY-EN] t=%0t -> rdata=%08x", $time, pending_data);
`endif
                    end
                end
            end
        end
    end

endmodule