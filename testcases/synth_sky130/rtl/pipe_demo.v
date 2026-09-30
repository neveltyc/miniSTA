// Three-stage pipeline used to produce a synthesizable sky130 netlist.
// Stage 1 registers three combinational results, stage 2 selects between
// them, and stage 3 inverts the result onto the output port.

module pipe_demo (
    input  wire       clk,
    input  wire       rst_n,
    input  wire [7:0] a,
    input  wire [7:0] b,
    input  wire       sel,
    output wire [7:0] y
);

    reg [7:0] s1_sum;
    reg [7:0] s1_and;
    reg [7:0] s1_xor;
    reg [7:0] s2_mix;
    reg [7:0] s3_out;

    always @(posedge clk or negedge rst_n) begin
        if (!rst_n) begin
            s1_sum <= 8'd0;
            s1_and <= 8'd0;
            s1_xor <= 8'd0;
        end
        else begin
            s1_sum <= a + b;
            s1_and <= a & b;
            s1_xor <= a ^ b;
        end
    end

    always @(posedge clk or negedge rst_n) begin
        if (!rst_n)
            s2_mix <= 8'd0;
        else
            s2_mix <= sel ? (s1_sum ^ s1_and) : (s1_xor | s1_sum);
    end

    always @(posedge clk or negedge rst_n) begin
        if (!rst_n)
            s3_out <= 8'd0;
        else
            s3_out <= ~s2_mix;
    end

    assign y = s3_out;

endmodule
