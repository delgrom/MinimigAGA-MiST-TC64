/* indicators.v */

module indicators (
  // system
  input  wire           clk,    // clock
  input  wire           rst,    // reset
  // inputs
  input wire  [  7-1:0] track,  // floppy track number
  input wire            f_wr,   // floppy fifo write
  input wire            f_rd,   // floppy fifo read
  input wire            h_wr,   // harddisk fifo write
  input wire            h_rd,   // harddisk fifo read
  input wire  [  3-1:0] status, // control block slave status
  // outputs
  output wire [  7-1:0] hex_0,  // seven segment display 0
  output wire [  7-1:0] hex_1,  // seven segment display 1
  output wire [  7-1:0] hex_2,  // seven segment display 2
  output wire [  7-1:0] hex_3,  // seven segment display 3
  output wire [  8-1:0] led_g,  // green leds
  output wire [ 10-1:0] led_r   // red leds
);


// 7-segment display

sseg_decode #(
  .REG  (1),
  .INV  (1)
) sseg_HEX0 (
  .clk  (clk),
  .rst  (rst),
  .num  (track[3:0]),
  .sseg (hex_0)
);

sseg_decode #(
  .REG  (1),
  .INV  (1)
) sseg_HEX1 (
  .clk  (clk),
  .rst  (rst),
  .num  ({1'b0, track[6:4]}),
  .sseg (hex_1)
);

assign hex_2        = 7'h7f;  // off
assign hex_3        = ~7'h71; // f


// LEDs

reg [1:0] r0, r1, g0, g1;

always @ (posedge clk or posedge rst) begin
  if (rst) begin
    r0 <= #1 2'b00;
    r1 <= #1 2'b00;
    g0 <= #1 2'b00;
    g1 <= #1 2'b00;
  end else begin
    r0 <= #1 {r0[0], f_wr};
    r1 <= #1 {r1[0], h_wr};
    g0 <= #1 {g0[0], f_rd};
    g1 <= #1 {g1[0], h_rd};
  end
end

wire r0_out, g0_out, r1_out, g1_out;

assign r0_out = |r0;
assign r1_out = |r1;
assign g0_out = |g0;
assign g1_out = |g1;

assign led_g = {        6'b000000, g1_out, g0_out};
assign led_r = {status, 5'b00000,  r1_out, r0_out};


endmodule

