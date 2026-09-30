module repro(input clk, input a, input b, input d, output q); wire rn; OR2 u_or (.A(a),.B(b),.Y(rn)); DFF u_ff (.CLK(clk),.D(d),.RN(rn),.Q(q)); endmodule
