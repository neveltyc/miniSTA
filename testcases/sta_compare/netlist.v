module simple (
inp1,
inp2,
tau2015_clk,
out
);

// Start PIs
input inp1;
input inp2;
input tau2015_clk;

// Start POs
output out;

// Start wires
wire n1;
wire n3;
wire n4;
wire inp1;
wire inp2;
wire tau2015_clk;
wire out;

// Start cells
NAND2X1 u1 ( .A(inp1), .B(inp2), .Y(n1) );
DFFPOSX1 f1 ( .D(n1), .CLK(tau2015_clk), .Q(n3) );
INVX1 u2 ( .A(n3), .Y(n4) );
INVX2 u3 ( .A(n4), .Y(out) );

endmodule
