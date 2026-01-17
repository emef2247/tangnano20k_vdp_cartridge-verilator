// ip_sdram_simple.v — debug output driven by reg
// - bus_rdata_en is an output reg driven in the clocked always block
// - detailed $display messages print when rdata_en changes
// - RDATA_PULSE controls pulse width (set to 8 for visibility)

module ip_sdram #(
    parameter        FREQ = 85_909_080,
    parameter integer RDATA_PULSE = 8
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
    output reg           bus_rdata_en,    // changed to reg

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

    localparam ADDR_WIDTH = 21;
    localparam DEPTH = (1 << ADDR_WIDTH);

    reg [31:0] mem [0:DEPTH-1];

    reg [31:0] ff_rdata;
    integer rdata_count;

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

    // Initialize
    initial begin
        rdata_count = 0;
        ff_rdata = 32'd0;
        bus_rdata_en = 1'b0;
    end

    always @(posedge clk) begin
        if (!reset_n) begin
            ff_rdata <= 32'd0;
            rdata_count <= 0;
            bus_rdata_en <= 1'b0;
`ifdef SDRAM_DEBUG
            $display("[IP_SDRAM] reset asserted (debug)");
`endif
        end else begin
            // Decrement counter if active (do it first so newly queued read sets full count)
            if (rdata_count > 0) rdata_count <= rdata_count - 1;

            // Handle commands
            if (bus_valid && bus_refresh) begin
                // ignore
`ifdef SDRAM_DEBUG
                $display("[IP_SDRAM-REF] t=%0t addr=%06x (refresh ignored)", $time, {bus_address,2'b00});
`endif
            end
            else if (bus_valid && bus_write) begin
                cur = mem[bus_address];
                for (i = 0; i < 4; i = i + 1) begin
                    if (bus_wdata_mask[i] == 1'b0) cur[(8*i) +: 8] = bus_wdata[(8*i) +: 8];
                end
                mem[bus_address] <= cur;
`ifdef SDRAM_DEBUG
                $display("[IP_SDRAM-WR ] t=%0t addr=%06x mask=%b data=%08x -> wrote %08x",
                         $time, {bus_address,2'b00}, bus_wdata_mask, bus_wdata, cur);
`endif
            end
            else if (bus_valid && !bus_write) begin
                // queue read response: latch rdata and set counter to pulse width
                ff_rdata <= mem[bus_address];
                rdata_count <= RDATA_PULSE;
`ifdef SDRAM_DEBUG
                $display("[IP_SDRAM-RQ ] t=%0t addr=%06x -> queued rdata=%08x en=%0d",
                         $time, {bus_address,2'b00}, mem[bus_address], RDATA_PULSE);
`endif
            end

            // drive bus_rdata_en from rdata_count
            if (rdata_count > 0) begin
                if (!bus_rdata_en) begin
                    bus_rdata_en <= 1'b1;
`ifdef SDRAM_DEBUG
                    $display("[IP_SDRAM-RSP-OUT] t=%0t ASSERT bus_rdata_en=1 rdata=%08x count=%0d",
                             $time, ff_rdata, rdata_count);
`endif
                end else begin
`ifdef SDRAM_DEBUG
                    $display("[IP_SDRAM-RSP-OUT] t=%0t HOLD   bus_rdata_en=1 count=%0d",
                             $time, rdata_count);
`endif
                end
            end else begin
                if (bus_rdata_en) begin
                    bus_rdata_en <= 1'b0;
`ifdef SDRAM_DEBUG
                    $display("[IP_SDRAM-RSP-OUT] t=%0t DEASSERT bus_rdata_en=0", $time);
`endif
                end
            end
        end
    end

endmodule