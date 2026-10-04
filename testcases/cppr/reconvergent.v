module ocv(input clk, output q);
wire common, lclk, cbranch, cclk, start, data, root, short, long1, long2;
BUF u_root (.A(clk), .Y(root));
BUF u_short (.A(root), .Y(short));
BUF u_long1 (.A(root), .Y(long1));
BUF u_long2 (.A(long1), .Y(long2));
MERGE u_common (.A(short), .B(long2), .Y(common));
BUF u_launch_clk (.A(common), .Y(lclk));
BUF u_capture_clk (.A(common), .Y(cbranch));
BUF u_capture_extra (.A(cbranch), .Y(cclk));
DFF u_launch (.CLK(lclk), .D(1'b0), .Q(start));
DATA u_data (.A(start), .Y(data));
DFF u_capture (.CLK(cclk), .D(data), .Q(q));
endmodule
