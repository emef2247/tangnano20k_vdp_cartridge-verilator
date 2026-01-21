// ip_sdram_simple.v — SDRAM simple model (v23-based, EN_DELAY added)
// - Single outstanding read (sufficient for this design)
// - Timing:
//    * ff_rdata updated on posedge clk_sdram when counter == RESPONSE_DELAY-1
//    * bus_rdata_en asserted for 1 clk cycle on posedge clk when counter == RESPONSE_DELAY-1 + EN_DELAY
//      (ff_rdata is loaded earlier on clk_sdram giving the ~0.5 cycle lead when EN_DELAY=0)
// - EN_DELAY introduced: internal shift offset (in clock cycles) between rdata load and en assertion
// - Defaults tuned to match ModelSim / v23 behavior: RESPONSE_DELAY=7, RDATA_PULSE=1
module ip_sdram #(
    parameter        FREQ = 85_909_080,
    parameter integer RDATA_PULSE    = 1,        // should be 1 to match ModelSim
    parameter integer CAS_LAT        = 2,        // CAS latency (cycles) >=1
    parameter integer RESPONSE_DELAY = 5,        // cycles to base rdata timing
    parameter integer EN_DELAY       = 0,        // additional delay (your EN_DELAY_AFTER_RDATA_CLK)
    parameter integer SHIFT_STAGES   = 16,       // >= RESPONSE_DELAY + EN_DELAY + RDATA_PULSE
    parameter integer SWAP_BYTES     = 0         // optional byte-swap
) (
    input                reset_n,
    input                clk,            // main VDP clock (posedge used for pipeline & en generation)
    input                clk_sdram,      // phase-shifted clock (posedge used to update ff_rdata half-cycle earlier)
    output               sdram_init_busy,

    input    [22:2]      bus_address,
    input                bus_valid,
    input                bus_write,
    input                bus_refresh,
    input    [31:0]      bus_wdata,
    input    [3:0]       bus_wdata_mask,
    output  [31:0]       bus_rdata,
    output reg           bus_rdata_en,   // asserted for 1 cycle on clk when ready (pos-edge)

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

    // sanity checks
    initial begin
        if (CAS_LAT < 1) begin
            $display("ERROR: CAS_LAT must be >= 1");
            $finish;
        end
        if (RESPONSE_DELAY + EN_DELAY + RDATA_PULSE > SHIFT_STAGES) begin
            $display("ERROR: SHIFT_STAGES must be >= RESPONSE_DELAY + EN_DELAY + RDATA_PULSE");
            $finish;
        end
    end

    localparam ADDR_WIDTH = 21;
    localparam DEPTH = (1 << ADDR_WIDTH);

    reg [31:0] mem [0:DEPTH-1];

    // pipeline registers for CAS latency (0..CAS_LAT-1)
    reg pipeline_valid [0:63];
    reg [22:2] pipeline_addr [0:63];
    reg next_pipeline_valid [0:63];
    reg [22:2] next_pipeline_addr [0:63];

    // bus output register
    reg [31:0] ff_rdata;

    // single outstanding pending data state
    reg pending;
    reg [31:0] pending_data;
    integer counter; // counts clk cycles since enqueue; compared against RESPONSE_DELAY-1 and RESPONSE_DELAY-1+EN_DELAY

    // assignments for static pins
    assign sdram_init_busy = 1'b0;
    assign O_sdram_clk  = clk_sdram;
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

    // byte-swap helper
    function [31:0] byteswap32(input [31:0] v);
        begin
            byteswap32 = {v[7:0], v[15:8], v[23:16], v[31:24]};
        end
    endfunction

    integer i, st;

    // initial
    initial begin
        ff_rdata = 32'd0;
        pending = 1'b0;
        pending_data = 32'd0;
        counter = 0;
        bus_rdata_en = 1'b0;
        for (st = 0; st < 64; st = st + 1) begin
            pipeline_valid[st] = 1'b0;
            pipeline_addr[st]  = {21{1'b0}};
        end
    end

    // pipeline and pending counter — run on posedge clk
    always @(posedge clk) begin
        if (!reset_n) begin
            for (st = 0; st < 64; st = st + 1) begin
                pipeline_valid[st] <= 1'b0;
                pipeline_addr[st]  <= {21{1'b0}};
            end
            pending <= 1'b0;
            pending_data <= 32'd0;
            counter <= 0;
            ff_rdata <= 32'd0;
            bus_rdata_en <= 1'b0;
        end else begin
            // default clear en (we emit 1-cycle pulses explicitly)
            bus_rdata_en <= 1'b0;

            // 1) handle writes first to update mem if needed
            if (bus_valid && bus_write) begin
                reg [31:0] cur;
                cur = mem[bus_address];
                for (i = 0; i < 4; i = i + 1) begin
                    if (bus_wdata_mask[i] == 1'b0) begin
                        cur[(8*i) +: 8] = bus_wdata[(8*i) +: 8];
                    end
                end
                mem[bus_address] <= cur;
`ifdef SDRAM_DEBUG
                $display("[IP_SDRAM-WR ] t=%0t addr=%06x mask=%b wdata=%08x -> mem[%06x]=%08x",
                         $time, {bus_address,2'b00}, bus_wdata_mask, bus_wdata, {bus_address,2'b00}, mem[bus_address]);
`endif
            end

            // 2) shift CAS pipeline
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
                $display("[IP_SDRAM-INJ] t=%0t inject read addr=%06x into stage=%0d (CAS=%0d)",
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

            // 3) If a read has arrived at stage 0, enqueue into pending and reset counter
            if (pipeline_valid[0]) begin
                reg [31:0] tmp;
                tmp = mem[pipeline_addr[0]];
                if (SWAP_BYTES != 0) tmp = byteswap32(tmp);
                pending <= 1'b1;
                pending_data <= tmp;
                counter <= 0; // start counting from 0 on next clocks
                // consume stage 0
                pipeline_valid[0] <= 1'b0;
`ifdef SDRAM_DEBUG
                $display("[IP_SDRAM-RSP-ENQ] t=%0t addr=%06x mem=%08x pending=%08x (cnt reset)",
                         $time, {pipeline_addr[0],2'b00}, mem[pipeline_addr[0]], tmp);
`endif
            end else begin
                // If pending and not yet emitted, increment counter or assert en when target reached
                if (pending) begin
                    // target for en assertion is RESPONSE_DELAY - 1 + EN_DELAY
                    if (counter < RESPONSE_DELAY - 1 + EN_DELAY) begin
                        counter <= counter + 1;
                    end
                    else begin
                        // reached target: assert en on this posedge and clear pending
                        bus_rdata_en <= 1'b1;
                        pending <= 1'b0;
`ifdef SDRAM_DEBUG
                        $display("[IP_SDRAM-RSP-EN ] t=%0t asserting en (counter==%0d) pending_data=%08x (EN_DELAY=%0d)",
                                 $time, counter, pending_data, EN_DELAY);
`endif
                    end
                end
            end
        end
    end

    // 4) Load ff_rdata on posedge clk_sdram when counter == RESPONSE_DELAY-1 and pending set
    //    This gives the half-cycle lead: ff_rdata becomes valid on clk_sdram, then bus_rdata_en on clk (depending on EN_DELAY).
    always @(posedge clk_sdram) begin
        if (!reset_n) begin
            ff_rdata <= 32'd0;
        end else begin
            if (pending && counter == RESPONSE_DELAY-1) begin
                ff_rdata <= pending_data;
`ifdef SDRAM_DEBUG
                $display("[IP_SDRAM-RSP-LOAD] t=%0t clk_sdram loading ff_rdata=%08x (counter=%0d)", $time, pending_data, counter);
`endif
            end
        end
    end

endmodule