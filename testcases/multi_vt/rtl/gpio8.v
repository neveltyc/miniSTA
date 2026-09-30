// GPIO 输出寄存器：综合时映射到标准阈值（RVT）库。
module gpio8 (
    input         clk,
    input         resetn,
    input         wr,
    input  [7:0]  wdata,
    input  [7:0]  din,
    output [7:0]  dout
);
    reg [7:0] reg_q;

    always @(posedge clk)
        if (!resetn)      reg_q <= 8'd0;
        else if (wr)      reg_q <= wdata;

    assign dout = reg_q ^ din;
endmodule
