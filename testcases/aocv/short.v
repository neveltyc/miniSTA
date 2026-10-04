module ocv(input clk, output q);
wire common, lclk, cbranch, cclk, start, data;
BUF u_common (.A(clk), .Y(common));
BUF u_launch_clk (.A(common), .Y(lclk));
BUF u_capture_clk (.A(common), .Y(cbranch));
BUF u_capture_extra (.A(cbranch), .Y(cclk));
DFF u_launch (.CLK(lclk), .D(1'b0), .Q(start));
DATA u_data (.A(start), .Y(data));
DFF u_capture (.CLK(cclk), .D(data), .Q(q));
endmodule
