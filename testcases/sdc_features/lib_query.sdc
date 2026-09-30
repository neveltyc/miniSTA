# get_libs / get_lib_cells / get_lib_pins：库对象查询。
# 查询结果就是名字，可以直接喂给 set_driving_cell -lib_cell/-pin/-from_pin 这类命令；
# lib_query_literal.sdc 是同一份约束的"直接写名字"版本，两份结果应当完全一样。
create_clock -name core -period 50.0 [get_ports tau2015_clk]
set_input_transition 0.05 [get_ports {inp1 inp2 tau2015_clk}]
set_input_delay -clock core -max 5.0 [get_ports inp1]
set_input_delay -clock core -min 0.0 [get_ports inp1]
set_input_delay -clock core -max 1.0 [get_ports inp2]
set_input_delay -clock core -min 0.0 [get_ports inp2]
set_load 0.05 [get_ports out]
set_output_delay -clock core -max 30.0 [get_ports out]
set_output_delay -clock core -min -10.0 [get_ports out]

# 库对象查询：只取名字看结果（集合本身是给命令用的，打印要用 msta::collectionNames）。
puts "LIBS : [msta::collectionNames [get_libs *]]"
puts "CELL : [msta::collectionNames [get_lib_cells NAND2X1]]"
puts "QUAL : [msta::collectionNames [get_lib_cells osu018_stdcells/NAND2X1]]"
puts "PINS : [msta::collectionNames [get_lib_pins -of_objects [get_lib_cells NAND2X1] {A B Y}]]"

set_driving_cell -lib_cell [get_lib_cells NAND2X1] \
    -pin [get_lib_pins -of_objects [get_lib_cells NAND2X1] Y] \
    -from_pin [get_lib_pins -of_objects [get_lib_cells NAND2X1] A] \
    [get_ports inp1]
