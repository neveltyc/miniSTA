// 三级流水线，用来综合出 sky130 门级网表。
// 第 1 级寄存三个组合运算结果，第 2 级在它们之间选择，
// 第 3 级把结果取反后送到输出端口。

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
