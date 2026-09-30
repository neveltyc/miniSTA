// 常开的小定时器：综合时映射到高阈值（HVT）库，优先省漏电。
module timer8 (
    input         clk,
    input         resetn,
    input         enable,
    output [7:0]  count,
    output        irq
);
    reg [7:0] cnt;

    always @(posedge clk)
        if (!resetn)
            cnt <= 8'd0;
        else if (enable)
            cnt <= cnt + 8'd1;

    assign count = cnt;
    assign irq   = (cnt == 8'hff) & enable;
endmodule
