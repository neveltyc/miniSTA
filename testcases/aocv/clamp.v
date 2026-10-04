module ocv(input clk, output q);
wire common, lclk, cbranch, cclk, start, data, x1, x2, x3;
BUF u_common (.A(clk), .Y(common));
BUF u_launch_clk (.A(common), .Y(lclk));
BUF u_capture_clk (.A(common), .Y(cbranch));
BUF u_capture_extra (.A(cbranch), .Y(cclk));
DFF u_launch (.CLK(lclk), .D(1'b0), .Q(start));
DATA u_data (.A(start), .Y(x1));
DATA u_data2 (.A(x1), .Y(x2));
DATA u_data3 (.A(x2), .Y(x3));
DATA u_data4 (.A(x3), .Y(data));
DFF u_capture (.CLK(cclk), .D(data), .Q(q));
endmodule
