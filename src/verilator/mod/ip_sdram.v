// ip_ram.v - small RAM model, Verilator-friendly
module ip_sdram (
    input           reset_n,
    input           clk,
    input   [13:0]  bus_address,
    input           bus_valid,
    output          bus_ready,
    input           bus_write,
    input   [7:0]   bus_wdata,
    output  [7:0]   bus_rdata,
    output          bus_rdata_en
);
    reg [7:0] ff_ram [0:16383];
    reg [7:0] ff_rdata;
    reg       ff_rdata_en;
    reg [7:0] ff_rdata2;
    reg       ff_rdata2_en;
    reg [7:0] ff_rdata3;
    reg       ff_rdata3_en;

    assign bus_ready = 1'b1;

    integer i;
    initial begin
        ff_rdata = 8'd0;
        ff_rdata_en = 1'b0;
        ff_rdata2 = 8'd0;
        ff_rdata2_en = 1'b0;
        ff_rdata3 = 8'd0;
        ff_rdata3_en = 1'b0;
        // Optionally clear RAM for deterministic sims:
        // for (i = 0; i < 16384; i = i + 1) ff_ram[i] = 8'h00;
    end

    // read: sample in one clock (registered), then push through pipeline
    always @(posedge clk) begin
        if (!reset_n) begin
            ff_rdata <= 8'd0;
            ff_rdata_en <= 1'b0;
        end else begin
            if (bus_valid && !bus_write) begin
                ff_rdata <= ff_ram[ bus_address ];
                ff_rdata_en <= 1'b1;
            end else begin
                ff_rdata <= 8'd0;
                ff_rdata_en <= 1'b0;
            end
        end
    end

    // write: synchronous
    always @(posedge clk) begin
        if (bus_valid && bus_write) begin
            ff_ram[ bus_address ] <= bus_wdata;
        end
    end

    // pipeline stage to mimic latency (2 cycles)
    always @(posedge clk) begin
        if (!reset_n) begin
            ff_rdata2 <= 8'd0;
            ff_rdata2_en <= 1'b0;
            ff_rdata3 <= 8'd0;
            ff_rdata3_en <= 1'b0;
        end else begin
            ff_rdata2 <= ff_rdata;
            ff_rdata2_en <= ff_rdata_en;
            ff_rdata3 <= ff_rdata2;
            ff_rdata3_en <= ff_rdata2_en;
        end
    end

    assign bus_rdata = ff_rdata3;
    assign bus_rdata_en = ff_rdata3_en;
endmodule