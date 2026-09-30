// 多阈值综合的顶层：三个子块分别用 LVT/RVT/HVT 三种标准单元库映射。
//   picorv32 核  -> LVT（低阈值，快，时序关键）
//   timer8       -> HVT（高阈值，慢，漏电小，常开）
//   gpio8        -> RVT（标准阈值）
module soc_top (
    input         clk,
    input         resetn,

    // 外部存储器接口（直接引出，保留完整时序路径）
    output        mem_valid,
    output        mem_instr,
    input         mem_ready,
    output [31:0] mem_addr,
    output [31:0] mem_wdata,
    output [ 3:0] mem_wstrb,
    input  [31:0] mem_rdata,

    // GPIO
    input  [7:0]  gpio_in,
    output [7:0]  gpio_out,

    // 状态
    output        trap,
    output        irq_out,
    output [7:0]  timer_count
);
    wire [31:0] irq;
    wire        timer_irq;
    wire        gpio_wr;

    timer8 u_timer (
        .clk    (clk),
        .resetn (resetn),
        .enable (resetn),
        .count  (timer_count),
        .irq    (timer_irq)
    );

    // 地址低 8 位为 0 时写 GPIO
    assign gpio_wr = mem_valid & mem_ready & (mem_addr[31:8] == 24'h0);

    gpio8 u_gpio (
        .clk    (clk),
        .resetn (resetn),
        .wr     (gpio_wr),
        .wdata  (mem_wdata[7:0]),
        .din    (gpio_in),
        .dout   (gpio_out)
    );

    assign irq = {31'b0, timer_irq};

    picorv32 u_core (
        .clk            (clk),
        .resetn         (resetn),
        .trap           (trap),
        .mem_valid      (mem_valid),
        .mem_instr      (mem_instr),
        .mem_ready      (mem_ready),
        .mem_addr       (mem_addr),
        .mem_wdata      (mem_wdata),
        .mem_wstrb      (mem_wstrb),
        .mem_rdata      (mem_rdata),
        .irq            (irq),
        .eoi            ()
    );

    assign irq_out = trap | timer_irq;
endmodule
