// Simple bridge that allows CPU-side (C++) to drive the slot data bus.
// When cpu_drive_en==1 the bridge drives slot_d with cpu_ff_slot_data,
// otherwise it tri-states so DUT may drive slot_d.
module slot_bridge (
    inout  [7:0] slot_d,           // connect to DUT's slot_d (inout)
    input  [7:0] cpu_ff_slot_data, // data from C++ (driver)
    input        cpu_drive_en      // when 1 -> bridge drives slot_d with cpu_ff_slot_data
);

    // Drive or tri-state depending on cpu_drive_en.
    assign slot_d = cpu_drive_en ? cpu_ff_slot_data : 8'bz;

endmodule

