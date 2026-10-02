#!/usr/bin/env tclsh
#
# SDC 前端：Tcl 负责引号、变量、source、续行和集合展开，时序引擎只接收一段
# 带类型的命令流（JSON）。
#
# 容错约定：一条命令出错只丢这条命令；未建模的命令、集合选项给警告后跳过。
# 已经解析出来的约束照常交给引擎，不会因为某一行而整份作废。

namespace eval msta {
    variable commands {}
    variable nSkipped 0
    variable dirStack {}
    variable aWarnedDialect {}
    # 库对象索引：loadLibIndex 从 C 侧写的文件里填。
    variable vLibNames {}
    variable aCellLibs
    variable aCellPins
    variable aQualCells {}
    variable aQualPins
    array set aCellLibs {}
    array set aCellPins {}
    array set aQualPins {}

    # SDC 1.8（Synopsys 手册 Appendix A）给 get_* 的选项只有
    #   [-hierarchical] [-hsc separator] [-regexp] [-nocase] -of_objects objects
    # msta 的展平名字本身就是层次名，-hierarchical 等价于默认行为。
    # 会改变对象集合、而 msta 又没建模的选项，报告后让该条命令作废。
    variable aValueOptions {-of_objects -filter -expression -level}
    variable aFlagOptions {-regexp -nocase -exact -leaf -segments -all -flat -substitute}
    # 别家工具常见的写法：认，但要告警说明它不是 SDC 1.8 语法。
    variable aDialectFlag {-quiet}

    proc warn {msg} {
        puts stderr "** 警告：sdc：$msg"
        incr ::msta::nSkipped
    }

    # 只提示、不算"跳过"（例如方言兼容说明）。
    proc note {msg} {
        puts stderr "** 警告：sdc：$msg"
    }

    # ---------------- get_* -filter：借 Tcl 的 expr 求值 ----------------
    # 属性由 C 侧在起 tclsh 之前导出（loadDesignIndex），所以过滤在 Tcl 侧做，
    # 运算符与优先级直接用 Tcl 的，不用自己写表达式解析器。
    # 支持的属性：
    #   cells: name / full_name / ref_name
    #   pins : name / full_name / ref_name / direction / is_clock
    #   nets : name / full_name / fanout
    #   ports: name / full_name / direction
    variable aDesign
    variable fDesignIndex 0
    array set aDesign {}

    proc loadDesignIndex {path} {
        variable aDesign
        variable fDesignIndex
        set file [open $path r]
        while {[gets $file line] >= 0} {
            set f [split $line "\t"]
            switch -- [lindex $f 0] {
                "cell" { lappend aDesign(cells) [list [lindex $f 1] [lindex $f 2]] }
                "pin"  { lappend aDesign(pins)  [list [lindex $f 1] [lindex $f 2] [lindex $f 3]] }
                "net"  { lappend aDesign(nets)  [list [lindex $f 1] [lindex $f 2]] }
                "port" { lappend aDesign(ports) [list [lindex $f 1] [lindex $f 2]] }
            }
        }
        close $file
        set fDesignIndex 1
    }

    # =~ / !~ / ~= 用通配匹配，其余运算符交给 Tcl expr。
    proc filterMatch {value pattern} { return [string match $pattern $value] }

    # 把 "a =~ b" 翻成 "[msta::filterMatch a b]"；表达式按词切开后逐个替换，
    # 这样 && / || 的优先级仍然由 Tcl 决定。
    proc translateFilter {expr attrs} {
        set tokens {}
        set n [string length $expr]
        set i 0
        while {$i < $n} {
            set c [string index $expr $i]
            if {$c eq " " || $c eq "\t"} { incr i; continue }
            if {$c eq "\""} {
                set tok "\""
                incr i
                while {$i < $n && [string index $expr $i] ne "\""} {
                    append tok [string index $expr $i]
                    incr i
                }
                append tok "\""
                incr i
                lappend tokens $tok
                continue
            }
            set two [string range $expr $i [expr {$i + 1}]]
            if {$two in {"=~" "!~" "~=" "==" "!=" "<=" ">=" "&&" "||"}} {
                lappend tokens $two
                incr i 2
                continue
            }
            if {$c in {"<" ">" "!" "(" ")"}} {
                lappend tokens $c
                incr i
                continue
            }
            set tok ""
            while {$i < $n} {
                set c [string index $expr $i]
                set two [string range $expr $i [expr {$i + 1}]]
                if {$c eq " " || $c eq "\t" || $c in {"(" ")" "<" ">" "!"} ||
                    $two in {"=~" "!~" "~=" "==" "!=" "<=" ">=" "&&" "||"}} { break }
                append tok $c
                incr i
            }
            lappend tokens $tok
        }
        # SDC 过滤表达式里的裸词按字符串处理（ref_name == sky130_fd_sc_hd__dfxtp_1），而 Tcl 的 expr 要求
        # 变量写成 $name、字符串写成 "..."：属性名前面补 $，其余裸词加引号。
        set quoted {}
        foreach tok $tokens {
            if {[string match "\"*" $tok]} {
                lappend quoted $tok
            } elseif {[lsearch -exact $attrs $tok] >= 0} {
                lappend quoted "\$$tok"
            } elseif {[string is double -strict $tok] ||
                $tok in {"=~" "!~" "~=" "==" "!=" "<=" ">=" "&&" "||" "<" ">" "!" "(" ")"}} {
                lappend quoted $tok
            } else {
                lappend quoted "\"$tok\""
            }
        }
        set tokens $quoted
        set out {}
        for {set k 0} {$k < [llength $tokens]} {incr k} {
            set tok [lindex $tokens $k]
            if {$tok in {"=~" "!~" "~="}} {
                set left [lindex $out end]
                set right [lindex $tokens [expr {$k + 1}]]
                if {[llength $out] == 0 || $right eq ""} {
                    return -code error "匹配运算符的位置不对"
                }
                set out [lrange $out 0 end-1]
                if {$tok eq "!~"} {
                    lappend out "!\[msta::filterMatch $left $right\]"
                } else {
                    lappend out "\[msta::filterMatch $left $right\]"
                }
                incr k
                continue
            }
            lappend out $tok
        }
        return [join $out " "]
    }

    # 一个对象过不过滤：把属性绑成局部变量再 expr。
    # 求值出错（属性名不认识等）返回 -1，由调用方告警。
    proc filterPass {attrs expr} {
        foreach {key value} $attrs { set $key $value }
        if {[catch {expr $expr} result]} { return -1 }
        return [expr {$result ? 1 : 0}]
    }

    # 按 -filter 筛出集合：kind 是 C 侧的类型字符，候选来自设计索引。
    # 返回名字列表；-1 表示不支持（调用方告警并丢掉这条命令）。
    proc filterQuery {kind names expr} {
        variable aDesign
        variable fDesignIndex
        set table [dict get {P ports G pins N nets I cells} $kind]
        switch -- $table {
            "cells" { set attrNames {name full_name ref_name} }
            "pins"  { set attrNames {name full_name ref_name direction is_clock} }
            "nets"  { set attrNames {name full_name fanout} }
            "ports" { set attrNames {name full_name direction} }
            default { return -1 }
        }
        if {[catch {set re [translateFilter $expr $attrNames]} msg]} {
            note "-filter 表达式未建模：$msg"
            return -1
        }
        if {![info exists aDesign($table)]} {
            note "-filter 需要设计索引，而设计索引只在 SDC 用到 -filter 时才生成；跳过这条命令"
            return -1
        }
        set out {}
        foreach obj $aDesign($table) {
            set name [lindex $obj 0]
            set matched 0
            foreach pattern $names {
                if {[string match $pattern $name]} { set matched 1; break }
            }
            if {!$matched} { continue }
            switch -- $table {
                "cells" { set attrs [list name $name full_name $name ref_name [lindex $obj 1]] }
                "pins"  { set attrs [list name $name full_name $name \
                                    ref_name [string range $name [expr {[string last / $name] + 1}] end] \
                                    direction [lindex $obj 1] is_clock [lindex $obj 2]] }
                "nets"  { set attrs [list name $name full_name $name fanout [lindex $obj 1]] }
                "ports" { set attrs [list name $name full_name $name direction [lindex $obj 1]] }
            }
            set rc [filterPass $attrs $re]
            if {$rc < 0} {
                note "-filter 表达式用到了未建模的属性；跳过这条命令"
                return -1
            }
            if {$rc} { lappend out $name }
        }
        return $out
    }

    proc collection {kind args} {
        variable aValueOptions
        variable aFlagOptions
        variable aDialectFlag
        set names {}
        set parts {}
        set fUnsupported 0
        set quiet 0
        set filterExpr ""
        for {set i 0} {$i < [llength $args]} {incr i} {
            set arg [lindex $args $i]
            if {![string match -* $arg] || [string match "\u001e*" $arg]} {
                foreach item $arg {
                    if {$item ne ""} { lappend names $item }
                }
                continue
            }
            if {$arg eq "-hierarchical"} { continue }
            if {[lsearch -exact $aDialectFlag $arg] >= 0} {
                dialect "$arg"
                if {$arg eq "-quiet"} { set quiet 1 }
                continue
            }
            if {$arg eq "-hsc"} {
                incr i
                if {[lindex $args $i] ne "/"} {
                    warn "层次分隔符 \"[lindex $args $i]\" 未建模；跳过这条命令"
                    set fUnsupported 1
                }
                continue
            }
            if {[lsearch -exact $aValueOptions $arg] >= 0} {
                # -of_objects 由 C 侧按对象关系展开（这里只把集合原样带过去）；
                # -filter 在 Tcl 侧按设计索引筛（见 filterQuery），
                # -expression/-level 等选项不受支持。
                if {$arg eq "-of_objects"} {
                    incr i
                    lappend parts "of=[escapeOption [lindex $args $i]]"
                    continue
                }
                if {$arg eq "-filter"} {
                    incr i
                    set filterExpr [lindex $args $i]
                    continue
                }
                warn "集合选项 $arg 未建模；跳过这条命令"
                set fUnsupported 1
                incr i
                continue
            }
            if {[lsearch -exact $aFlagOptions $arg] >= 0} {
                warn "集合选项 $arg 未建模；跳过这条命令"
                set fUnsupported 1
                continue
            }
            warn "集合选项 $arg 不是 SDC 1.8 语法；跳过这条命令"
            set fUnsupported 1
        }
        if {$fUnsupported} { return "\u001eZ0" }
        # SDC 规定不写对象时等于写 *。
        if {[llength $names] == 0} { set names [list *] }
        if {$filterExpr ne ""} {
            if {[llength $parts] > 0} {
                warn "-filter 与 -of_objects 同时使用未建模；跳过这条命令"
                return "\u001eZ0"
            }
            set filtered [filterQuery $kind $names $filterExpr]
            if {$filtered < 0} { return "\u001eZ0" }
            set names $filtered
            if {[llength $names] == 0 && !$quiet} {
                note "get_* -filter \"$filterExpr\" 没有匹配到任何对象"
            }
        }
        # -quiet 只是压掉"没匹配到对象"的提示，用小写类型标记传给 C。
        if {$quiet} { set kind [string tolower $kind] }
        # 有集合选项（-of_objects）时把选项区挂在 \x1d 之后，交 C 侧按关系展开。
        if {[llength $parts] > 0} {
            return "\u001e${kind}[join $names \u001f]\u001d[join $parts \u001f]"
        }
        return "\u001e${kind}[join $names \u001f]"
    }

    # 非 SDC 1.8 语法的兼容写法：认，但每次告警一次说明原因。
    proc dialect {what} {
        variable aWarnedDialect
        if {[lsearch -exact $aWarnedDialect $what] >= 0} { return }
        lappend aWarnedDialect $what
        note "$what 不是 SDC 1.8 语法，按兼容写法处理"
    }

    # 集合标记拆成 {头部 选项}：get_* 是 \x1e<kind><名字...>，
    # all_* 是 \x1eA<form>\x1d<选项...>，选项一律挂在 \x1d 之后。
    proc splitMarker {value} {
        set sep [string first "\u001d" $value]
        if {$sep >= 0} {
            return [list [string range $value 0 $sep] [string range $value $sep+1 end]]
        }
        return [list "${value}\u001d" ""]
    }

    # get_* 集合里记着的名字（用于集合减法；通配模式原样交给 C 侧匹配）。
    proc markerNames {value} {
        set parts [splitMarker $value]
        set body [string range [lindex $parts 0] 2 end-1]
        set names {}
        foreach n [split $body "\u001f"] { if {$n ne ""} { lappend names $n } }
        return $names
    }

    # 集合的"类型字符"：P/N/C/I 是 get_*，A 是 all_* 那种整体标记。
    proc markerKind {value} { return [string index $value 1] }

    # 选项区里要带一整个集合标记（-of_objects）时，把标记里的分隔符换成 @e/@d/@f，
    # 免得和选项之间的 \x1f 打架；空白换成 @s，这样标记里不含空白，放进 concat
    # 拼成的列表也不会被拆开。C 侧读出来再换回去（空白一律还原成空格）。
    proc escapeOption {value} {
        return [string map [list @ @@ \u001e @e \u001d @d \u001f @f \
                                 " " @s \t @s \n @s \r @s] $value]
    }

    # 从集合里取名字：带类型标记的取标记里的名字（\x1f 分隔），裸字符串原样返回。
    proc collectionNames {value} {
        if {![string match "\u001e*" $value]} { return [list $value] }
        set body [string range $value 2 end]
        set sep [string first "\u001d" $body]
        if {$sep >= 0} { set body [string range $body 0 $sep-1] }
        set names {}
        foreach n [split $body "\u001f"] { if {$n ne ""} { lappend names $n } }
        return $names
    }

    # 读 C 侧写的库索引（见 msta_sdc.c 的 Msta_SdcWriteLibIndex）。
    proc loadLibIndex {path} {
        variable vLibNames
        variable aCellLibs
        variable aCellPins
        variable aQualCells
        variable aQualPins
        set file [open $path r]
        while {[gets $file line] >= 0} {
            if {$line eq ""} { continue }
            set f [split $line "\t"]
            set lib  [lindex $f 1]
            set cell [lindex $f 2]
            set pin  [lindex $f 3]
            set qual "$lib/$cell"
            switch -- [lindex $f 0] {
                "lib" {
                    if {[lsearch -exact $vLibNames $lib] < 0} { lappend vLibNames $lib }
                }
                "cell" {
                    if {![info exists aCellLibs($cell)]} { set aCellLibs($cell) {} }
                    if {[lsearch -exact $aCellLibs($cell) $lib] < 0} {
                        lappend aCellLibs($cell) $lib
                    }
                    if {[lsearch -exact $aQualCells $qual] < 0} {
                        lappend aQualCells $qual
                    }
                }
                "pin" {
                    if {![info exists aQualPins($qual)]} { set aQualPins($qual) {} }
                    if {[lsearch -exact $aQualPins($qual) $pin] < 0} {
                        lappend aQualPins($qual) $pin
                    }
                    if {![info exists aCellPins($cell)]} { set aCellPins($cell) {} }
                    if {[lsearch -exact $aCellPins($cell) $pin] < 0} {
                        lappend aCellPins($cell) $pin
                    }
                }
            }
        }
        close $file
    }

    # SDC 的通配（* 和 ?）转成正则；方括号当普通字符，总线名字要用。
    proc globToRegexp {pattern} {
        set re [regsub -all {[.^$+()|\\\[\]{}]} $pattern {\\&}]
        return "^[string map {* ".*" ? "."} $re]\$"
    }

    # 名字是否命中模式；-regexp 时模式本身就是正则。
    proc nameMatches {pattern name fRegexp fNocase} {
        set opts {}
        if {$fNocase} { set opts [list -nocase] }
        if {$fRegexp} { return [regexp {*}$opts -- $pattern $name] }
        return [regexp {*}$opts -- [globToRegexp $pattern] $name]
    }

    # get_libs / get_lib_cells / get_lib_pins 的实现。
    # 结果打包成 L/B/Y 标记交给 C 侧展开，名字可以直接用在约束命令里。
    proc libQuery {cmd kind args} {
        variable vLibNames
        variable aCellLibs
        variable aCellPins
        variable aQualCells
        variable aQualPins
        set what [expr {$kind eq "libs" ? "库"
                      : $kind eq "cells" ? "库单元" : "库引脚"}]
        set fRegexp 0
        set fNocase 0
        set patterns {}
        set ofObjects {}
        set n [llength $args]
        for {set i 0} {$i < $n} {incr i} {
            set arg [lindex $args $i]
            switch -- $arg {
                "-regexp" { set fRegexp 1; continue }
                "-nocase" { set fNocase 1; continue }
                "-hierarchical" { continue }
                "-hsc" {
                    incr i
                    if {[lindex $args $i] ne "/"} {
                        warn "层次分隔符 \"[lindex $args $i]\" 未建模；msta 只用 \"/\""
                    }
                    continue
                }
                "-of_objects" {
                    if {$kind ne "pins"} {
                        warn "SDC 1.8 里 $cmd 不带 -of_objects；跳过这条命令"
                        return "\u001eZ0"
                    }
                    incr i
                    set ofObjects [collectionNames [lindex $args $i]]
                    continue
                }
            }
            if {[string match -* $arg] && ![string match "\u001e*" $arg]} {
                warn "$cmd 的选项 $arg 未建模（或不是 SDC 1.8 语法）；跳过这条命令"
                return "\u001eZ0"
            }
            foreach item $arg { if {$item ne ""} { lappend patterns $item } }
        }
        # 不写对象时按 SDC 的约定等于写 *。
        if {[llength $patterns] == 0} { set patterns [list *] }

        set result {}
        set cells {}
        if {$kind eq "libs"} {
            set cells $vLibNames
        } elseif {$kind eq "cells"} {
            set cells [array names aCellLibs]
        }
        foreach pattern $patterns {
            if {$kind eq "pins"} {
                set pins {}
                if {[llength $ofObjects] > 0} {
                    foreach cell $ofObjects {
                        if {[info exists aQualPins($cell)]} {
                            foreach pin $aQualPins($cell) { lappend pins $pin }
                        } elseif {[info exists aCellPins($cell)]} {
                            foreach pin $aCellPins($cell) { lappend pins $pin }
                        } else {
                            note "$cmd：库单元 \"$cell\" 不在库里"
                        }
                    }
                } else {
                    foreach cell [array names aCellPins] {
                        foreach pin $aCellPins($cell) { lappend pins $pin }
                    }
                }
                foreach name $pins {
                    if {[nameMatches $pattern $name $fRegexp $fNocase] &&
                        [lsearch -exact $result $name] < 0} { lappend result $name }
                }
            } else {
                # 带库名前缀的模式（库名/单元名）匹配限定名，返回的也是限定名。
                set names $cells
                if {$kind eq "cells" && [string first "/" $pattern] >= 0} { set names $aQualCells }
                foreach name $names {
                    if {[nameMatches $pattern $name $fRegexp $fNocase] &&
                        [lsearch -exact $result $name] < 0} { lappend result $name }
                }
            }
        }
        if {[llength $result] == 0} {
            note "$cmd \"[join $patterns { }]\" 没有匹配到任何$what"
        }
        set kindChar [expr {$kind eq "libs" ? "L" : $kind eq "cells" ? "B" : "Y"}]
        return "\u001e${kindChar}[join $result \u001f]"
    }

    # remove_from_collection A B：从 A 里减掉 B 的名字。
    # 减掉的名字记进标记的 minus 选项，等 C 侧展开 A 时再过滤。
    proc remove_from_collection {a b} {
        foreach v [list $a $b] {
            if {![string match "\u001e*" $v]} {
                error "remove_from_collection 的两个参数都必须是集合"
            }
        }
        if {[markerKind $b] eq "A"} {
            warn "remove_from_collection：右边是 all_* 集合，无法逐个列出；跳过这条命令"
            return "\u001eZ0"
        }
        if {[lindex [splitMarker $b] 1] ne ""} {
            warn "remove_from_collection：右边的集合已经带有集合选项；跳过这条命令"
            return "\u001eZ0"
        }
        set names [markerNames $b]
        if {[llength $names] == 0} { return $a }
        set parts [splitMarker $a]
        set options [lindex $parts 1]
        foreach n $names { append options "minus=$n\u001f" }
        return "[lindex $parts 0]${options}"
    }

    # all_inputs / all_outputs / all_registers：选项打包进集合标记，由 C 侧展开。
    # 标记形状：\x1eA<form>\x1d<opt>\x1f<opt>...
    proc allForm {form args} {
        set parts {}
        set n [llength $args]
        for {set i 0} {$i < $n} {incr i} {
            set arg [lindex $args $i]
            if {$arg in {-clock -rise_clock -fall_clock}} {
                incr i
                if {$i < $n} {
                    lappend parts "[string range $arg 1 end]=[lindex $args $i]"
                }
            } elseif {$form eq "registers" && $arg in {-cells -data_pins -clock_pins -async_pins
                                                      -output_pins -level_sensitive -edge_triggered
                                                      -no_hierarchy -master_slave -slave_clock_pins}} {
                lappend parts [string range $arg 1 end]
            } elseif {$form ne "registers" && $arg in {-level_sensitive -edge_triggered}} {
                lappend parts [string range $arg 1 end]
            } elseif {$form eq "inputs" && $arg eq "-no_clocks"} {
                dialect "$arg"
                lappend parts no_clocks
            } else {
                warn "all_$form 的选项 $arg 未建模（或不是 SDC 1.8 语法）；跳过这条命令"
                return "\u001eZ0"
            }
        }
        return "\u001eA${form}\u001d[join $parts \u001f]"
    }

    # 本次运行里 get_* / all_* 等返回过的集合标记。emit 靠它准确认出"这个参数就是
    # 一个集合"，而不是"一个列表，里面有集合"：标记里可能有空白（例如
    # -of_objects {u1 u2}），单看文本分不清。
    variable aMarkers
    array set aMarkers {}

    # 记下一个集合标记并原样返回；文件末尾的 get_* / all_* 等全局命令都经过这里。
    proc marker {value} {
        variable aMarkers
        set aMarkers($value) 1
        return $value
    }

    proc isMarker {value} {
        variable aMarkers
        return [info exists aMarkers($value)]
    }

    # 把一个含集合的 Tcl 列表（[list d [get_pins x]]、[concat ...]，可以多层嵌套）
    # 展平成一组词：每个词是一个集合标记或一个名字。不含集合的部分按空白拆开，
    # 与 C 侧拆列表文本 {a b} 的规则相同，所以写在这里的名字和直接写在 {a b}
    # 里的名字解释成同样的对象。空元素 {} 不产生词。
    proc flattenList {value} {
        if {[isMarker $value]} { return [list $value] }
        if {[string first "\u001e" $value] < 0} {
            return [regexp -all -inline {[^ \t\r\n]+} $value]
        }
        if {![string is list $value]} {
            error "含集合的参数不是合法的 Tcl 列表"
        }
        # 只有一个元素、拆不开（如 "x[get_ports a]"）：集合和别的文字拼成了一个词。
        if {[llength $value] == 1 && [lindex $value 0] eq $value} {
            error "集合不能和别的文字拼成一个名字"
        }
        set words {}
        foreach item $value { lappend words {*}[flattenList $item] }
        return $words
    }

    proc quote {value} {
        set value [string map [list \\ \\\\ \" \\" \n \\n \r \\r \t \\t \
                                    \u001d \\u001d \u001e \\u001e \u001f \\u001f] $value]
        return "\"${value}\""
    }

    # 记录一条约束命令：参数原样交给 C 侧（集合已是标记）。选项的值是紧跟的
    # 一个参数，列表文本 {a b} 怎么解释由 C 侧的选项表决定。
    # 列表里套了集合的参数，C 侧没法从文本里把标记拆出来，这里先用 flattenList
    # 展平成一组词，写 JSON 时这个参数成为一个字符串数组。
    # 每个参数记成 {s 文本} 或 {l 词列表}。
    proc emit {name args} {
        variable commands
        set words [list [list s $name]]
        foreach arg $args {
            if {[isMarker $arg] || [string first "\u001e" $arg] < 0} {
                lappend words [list s $arg]
            } elseif {[catch {flattenList $arg} items]} {
                warn "$name：$items；跳过这条命令"
                lappend words [list s "\u001eZ0"]
            } else {
                lappend words [list l $items]
            }
        }
        lappend commands $words
    }

    proc write_json {path} {
        variable commands
        set file [open $path w]
        puts -nonewline $file "\["
        set first_command 1
        foreach command $commands {
            if {!$first_command} {puts -nonewline $file ","}
            set first_command 0
            puts -nonewline $file "\["
            set first_word 1
            foreach word $command {
                if {!$first_word} {puts -nonewline $file ","}
                set first_word 0
                lassign $word type value
                if {$type eq "l"} {
                    set items {}
                    foreach item $value { lappend items [quote $item] }
                    puts -nonewline $file "\[[join $items ,]\]"
                } else {
                    puts -nonewline $file [quote $value]
                }
            }
            puts -nonewline $file "\]"
        }
        puts $file "\]"
        close $file
    }

    # 执行一条已经完整的命令；出错只记一条警告。
    proc evalCommand {command path lineNo} {
        set code [catch {uplevel #0 $command} message]
        if {$code != 0} {
            warn "$path:$lineNo：$message；跳过这条命令"
        }
    }

    # 逐条命令执行，用 info complete 处理续行、多行花括号和引号。
    proc evalScript {text path} {
        set lineNo 0
        set firstLine 0
        set buffer ""
        foreach line [split $text "\n"] {
            incr lineNo
            if {$buffer eq ""} { set firstLine $lineNo }
            append buffer $line "\n"
            if {![info complete $buffer]} continue
            evalCommand $buffer $path $firstLine
            set buffer ""
        }
        if {$buffer ne ""} {
            warn "$path:$firstLine：文件末尾的 Tcl 命令不完整，已跳过"
        }
    }

    proc runFile {path} {
        if {[catch {open $path r} fileHandle]} {
            warn "无法读取 \"$path\"：$fileHandle"
            return
        }
        set text [read $fileHandle]
        close $fileHandle
        lappend ::msta::dirStack [file dirname $path]
        evalScript $text $path
        set ::msta::dirStack [lrange $::msta::dirStack 0 end-1]
    }

    proc resolvePath {path} {
        if {[file pathtype $path] ne "relative"} { return $path }
        set stack $::msta::dirStack
        if {[llength $stack] > 0} { return [file join [lindex $stack end] $path] }
        return $path
    }
}

proc get_ports {args} { return [msta::marker [msta::collection P {*}$args]] }
# 引脚用 G 标记（与网络 N 分开），-of_objects 时要按关系推对象。
proc get_pins {args} { return [msta::marker [msta::collection G {*}$args]] }
proc get_clocks {args} { return [msta::marker [msta::collection C {*}$args]] }
proc get_cells {args} { return [msta::marker [msta::collection I {*}$args]] }
proc get_nets {args} { return [msta::marker [msta::collection N {*}$args]] }
proc all_inputs {args} { return [msta::marker [msta::allForm inputs {*}$args]] }
proc all_outputs {args} { return [msta::marker [msta::allForm outputs {*}$args]] }
proc all_registers {args} { return [msta::marker [msta::allForm registers {*}$args]] }
proc all_clocks {} { return [msta::marker [msta::allForm all_clocks]] }

# Tcl 层的集合运算：实现在 msta 命名空间里，这里只做全局入口。
proc remove_from_collection {a b} { return [msta::marker [msta::remove_from_collection $a $b]] }

# current_design 既是语句（指定当前设计）也是对象：[current_design] 表示整个设计。
proc current_design {args} { return [msta::marker "\u001eAdesign\u001d"] }

# 层次相关的命令：msta 只有一张展平网表，名字始终从顶层解析。
proc current_instance {args} {
    if {[llength $args] > 0 && [lindex $args 0] ne ""} {
        msta::warn "current_instance 未建模，已忽略；对象名一律从顶层解析"
    }
    return ""
}

proc set_hierarchy_separator {args} {
    set separator [lindex $args 0]
    if {$separator ne "/"} {
        msta::warn "层次分隔符 \"$separator\" 未建模；msta 只用 \"/\""
    }
    return ""
}

# 库对象查询：索引由 C 侧在起 tclsh 之前写进文件（loadLibIndex）。
# 查询结果用 L/B/Y 三种标记打包，交给 C 侧展开成名字；名字可以直接喂给
# set_driving_cell -lib_cell / -pin 这类命令。
proc get_libs {args} { return [msta::marker [msta::libQuery get_libs libs {*}$args]] }
proc get_lib_cells {args} { return [msta::marker [msta::libQuery get_lib_cells cells {*}$args]] }
proc get_lib_pins {args} { return [msta::marker [msta::libQuery get_lib_pins pins {*}$args]] }

foreach command {
    create_clock create_generated_clock set_clock_uncertainty set_clock_latency
    set_propagated_clock set_clock_groups set_input_delay set_output_delay
    set_input_transition set_load set_driving_cell set_false_path
    set_multicycle_path set_max_delay set_min_delay set_case_analysis
    set_disable_timing set_units set_logic_zero set_logic_one set_logic_dc
    group_path
} {
    interp alias {} $command {} msta::emit $command
}

# 没注册的命令都给警告后跳过，不让 Tcl 的 "invalid command name" 把整份文件打断。
# set_* / create_* 仍然交给 C 侧：它认识一部分命令，不认识的会统一计数并告警。
rename unknown tcl_unknown
proc unknown {name args} {
    if {[string match {set_*} $name] || [string match {create_*} $name]} {
        msta::emit $name {*}$args
        return ""
    }
    msta::warn "命令 \"$name\" 未实现，已忽略"
    return ""
}

# SDC 里的 source 也走同一套容错流程，相对路径按当前文件所在目录解析。
rename source tcl_source
proc source {args} {
    msta::runFile [msta::resolvePath [lindex $args end]]
}

if {$argc < 2 || $argc > 4} {
    puts stderr "用法：sdc_bridge.tcl input.sdc output.json \[lib-index\] \[design-index\]"
    exit 2
}
set input [file normalize [lindex $argv 0]]
set output [lindex $argv 1]

if {$argc >= 3 && [file readable [lindex $argv 2]]} {
    msta::loadLibIndex [lindex $argv 2]
}
if {$argc >= 4 && [file readable [lindex $argv 3]]} {
    msta::loadDesignIndex [lindex $argv 3]
}

if {![file exists $input]} {
    puts stderr "** 错误：sdc：无法读取 \"$input\""
    exit 1
}

cd [file dirname $input]
msta::runFile $input

if {$msta::nSkipped > 0} {
    puts stderr "** 警告：sdc \"$input\"：跳过 $msta::nSkipped 条命令，其余约束照常生效"
}
msta::write_json $output
exit 0
