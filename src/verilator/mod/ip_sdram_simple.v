// ip_sdram_simple.v — SDRAM simple model (v11 base) with optional byte-swap for endian test
// - Single outstanding read model (CAS pipeline + stg-based response delay)
// - Added parameter SWAP_BYTES: when 1, latched 32-bit words are byte-swapped
//   (i.e. endianness reversed within the 32-bit word) before being presented on bus_rdata.
// Usage:
//  - By default SWAP_BYTES = 1 in this test build so simply replacing the file is enough.
//  - To disable swapping, set SWAP_BYTES = 0 when instantiating or edit parameter.

module ip_sdram #(
    parameter        FREQ = 85_909_080,
    parameter integer RDATA_PULSE = 8,        // data-hold cycles
    parameter integer CAS_LAT = 2,            // CAS latency in cycles, >=1
    parameter integer RESPONSE_DELAY = 5,     // cycles to delay asserting bus_rdata_en after latching
    parameter integer SHIFT_STAGES = 16,      // must be >= RESPONSE_DELAY + RDATA_PULSE
    parameter integer SWAP_BYTES = 0          // 1 = byte-swap latched data (test endian reverse), 0 = normal
) (
    input                reset_n,
    input                clk,
    input                clk_sdram,
    output               sdram_init_busy,

    input    [22:2]      bus_address,
    input                bus_valid,
    input                bus_write,
    input                bus_refresh,
    input    [31:0]      bus_wdata,
    input    [3:0]       bus_wdata_mask,
    output  [31:0]       bus_rdata,
    output reg           bus_rdata_en,   // driven after RESPONSE_DELAY for RDATA_PULSE cycles

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

    // sanity
    initial begin
        if (CAS_LAT < 1) begin
            $display("ERROR: CAS_LAT must be >= 1");
            $finish;
        end
        if (RESPONSE_DELAY + RDATA_PULSE > SHIFT_STAGES) begin
            $display("ERROR: SHIFT_STAGES must be >= RESPONSE_DELAY + RDATA_PULSE");
            $finish;
        end
    end

    localparam ADDR_WIDTH = 21;
    localparam DEPTH = (1 << ADDR_WIDTH);

    reg [31:0] mem [0:DEPTH-1];

    // response registers
    reg [31:0] ff_rdata;
    integer    rdata_hold;        // remaining cycles to keep ff_rdata valid (>=0)
    reg        rdata_strobe;      // set when a read is latched this cycle

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

    integer i;
    reg [31:0] cur;
    reg [31:0] latched_tmp;

    // pipeline: stage 0 .. CAS_LAT-1
    reg pipeline_valid [0:63];
    reg [22:2] pipeline_addr [0:63];
    reg next_pipeline_valid [0:63];
    reg [22:2] next_pipeline_addr [0:63];

    // shift register for delayed en generation
    reg [SHIFT_STAGES-1:0] stg; // stg[0] is insertion point; bit will shift toward MSB
    // next-state temporaries
    reg [31:0] next_ff_rdata;
    integer    next_rdata_hold;
    reg        next_rdata_strobe;
    reg [SHIFT_STAGES-1:0] next_stg;
    reg        next_bus_rdata_en;

    integer st;

    // initial
    initial begin
        ff_rdata = 32'd0;
        rdata_hold = 0;
        rdata_strobe = 1'b0;
        stg = {SHIFT_STAGES{1'b0}};
        bus_rdata_en = 1'b0;
        for (st = 0; st < 64; st = st + 1) begin
            pipeline_valid[st] = 1'b0;
            pipeline_addr[st]  = {21{1'b0}};
        end
    end

    // helper: byte-swap 32-bit
    function [31:0] byteswap32(input [31:0] v);
        begin
            byteswap32 = {v[7:0], v[15:8], v[23:16], v[31:24]};
        end
    endfunction

    always @(posedge clk) begin
        if (!reset_n) begin
            // synchronous reset
            ff_rdata <= 32'd0;
            rdata_hold <= 0;
            rdata_strobe <= 1'b0;
            stg <= {SHIFT_STAGES{1'b0}};
            bus_rdata_en <= 1'b0;
            for (st = 0; st < 64; st = st + 1) begin
                pipeline_valid[st] <= 1'b0;
                pipeline_addr[st]  <= {21{1'b0}};
            end
        end else begin
            // next-state base copy
            for (st = 0; st < 64; st = st + 1) begin
                next_pipeline_valid[st] = pipeline_valid[st];
                next_pipeline_addr[st]  = pipeline_addr[st];
            end
            next_ff_rdata = ff_rdata;
            next_rdata_hold = rdata_hold;
            next_rdata_strobe = 1'b0;
            next_stg = stg;
            next_bus_rdata_en = bus_rdata_en;

            // 1) handle writes / refresh first (so mem is updated before any read service)
            if (bus_valid && bus_refresh) begin
                // ignore in this simple model
            end
            else if (bus_valid && bus_write) begin
                // masked write: mask==0 => write enabled (as used in original mapping)
                cur = mem[bus_address];
                for (i = 0; i < 4; i = i + 1) begin
                    if (bus_wdata_mask[i] == 1'b0) begin
                        cur[(8*i) +: 8] = bus_wdata[(8*i) +: 8];
                    end
                end
                // blocking write so same-cycle reads see update
                mem[bus_address] = cur;
`ifdef SDRAM_DEBUG
                $display("[IP_SDRAM-WR ] t=%0t addr=%06x mask=%b wdata=%08x -> mem[%06x]=%08x",
                         $time, {bus_address,2'b00}, bus_wdata_mask, bus_wdata, {bus_address,2'b00}, mem[bus_address]);
`endif
            end

            // 2) shift pipeline (into next)
            for (st = 0; st < CAS_LAT-1; st = st + 1) begin
                next_pipeline_valid[st] = pipeline_valid[st+1];
                next_pipeline_addr[st]  = pipeline_addr[st+1];
            end
            next_pipeline_valid[CAS_LAT-1] = 1'b0;
            next_pipeline_addr[CAS_LAT-1]  = {21{1'b0}};

            // 3) inject read into last stage if requested
            if (bus_valid && !bus_write && !bus_refresh) begin
                next_pipeline_valid[CAS_LAT-1] = 1'b1;
                next_pipeline_addr[CAS_LAT-1]  = bus_address;
`ifdef SDRAM_DEBUG
                $display("[IP_SDRAM-INJ] t=%0t inject read addr=%06x into stage=%0d (CAS=%0d)",
                         $time, {bus_address,2'b00}, CAS_LAT-1, CAS_LAT);
`endif
            end

            // 4) service stage 0: latch mem and start shift token if request present
            if (pipeline_valid[0]) begin
                // latch the memory content
                latched_tmp = mem[pipeline_addr[0]];
                // optional byte-swap for endianness test
                if (SWAP_BYTES != 0)
                    next_ff_rdata = byteswap32(latched_tmp);
                else
                    next_ff_rdata = latched_tmp;
                // set hold long enough to cover delay + pulse
                next_rdata_hold = RESPONSE_DELAY + RDATA_PULSE;
                // set rdata_strobe to insert a '1' into shift pipeline
                next_rdata_strobe = 1'b1;
                // insert token at stg[0]
                next_stg = stg;
                next_stg[0] = 1'b1;
                // consume pipeline stage
                next_pipeline_valid[0] = 1'b0;
`ifdef SDRAM_DEBUG
                $display("[IP_SDRAM-RSP-START] t=%0t addr=%06x mem=%08x -> ff_rdata=%08x hold=%0d (delay=%0d) swap=%0d",
                         $time, {pipeline_addr[0],2'b00}, mem[pipeline_addr[0]], next_ff_rdata, next_rdata_hold, RESPONSE_DELAY, SWAP_BYTES);
`endif
            end else begin
                // no new request starting - decrement hold if active
                if (rdata_hold > 0) begin
                    next_rdata_hold = rdata_hold - 1;
                    // do not clear ff_rdata when hold ends - keep stable
                    if (next_rdata_hold == 0) begin
`ifdef SDRAM_DEBUG
                        $display("[IP_SDRAM-RSP-HOLD-END] t=%0t hold finished, ff_rdata remains=%08x", $time, ff_rdata);
`endif
                    end
                end
                // shift pipeline token if present
                if (|stg) begin
                    // shift left: bit moves from lower index to higher index each cycle
                    next_stg = {stg[SHIFT_STAGES-2:0], 1'b0};
                end
            end

            // Compute bus_rdata_en = OR of window [RESPONSE_DELAY .. RESPONSE_DELAY+RDATA_PULSE-1] of next_stg
            if (|next_stg[RESPONSE_DELAY +: RDATA_PULSE])
                next_bus_rdata_en = 1'b1;
            else
                next_bus_rdata_en = 1'b0;

            // commit next-state (non-blocking)
            for (st = 0; st < 64; st = st + 1) begin
                pipeline_valid[st] <= next_pipeline_valid[st];
                pipeline_addr[st]  <= next_pipeline_addr[st];
            end
            ff_rdata <= next_ff_rdata;
            rdata_hold <= next_rdata_hold;
            rdata_strobe <= next_rdata_strobe;
            stg <= next_stg;
            bus_rdata_en <= next_bus_rdata_en;
        end
    end

endmodule