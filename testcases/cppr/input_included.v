module ocv(input clk, input a, input b, output q);
wire common, cbranch, cclk, ax, bx, data;
BUF u_common (.A(clk),.Y(common));
BUF u_capture_clk (.A(common),.Y(cbranch));
BUF u_capture_extra (.A(cbranch),.Y(cclk));
DATA u_a (.A(a),.Y(ax)); DATA u_b (.A(b),.Y(bx));
MERGE u_merge (.A(ax),.B(bx),.Y(data));
DFF u_capture (.CLK(cclk),.D(data),.Q(q));
endmodule
