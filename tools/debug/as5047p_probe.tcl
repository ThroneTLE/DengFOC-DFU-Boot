# Read-only probe of the on-board AS5047P over SPI3 (PC10/11/12) with CS = PB3,
# while the board runs the idle loop app. Pins are restored by a reset at the end.
init
proc rd {a} { return [lindex [read_memory $a 32 1] 0] }
proc wr {a v} { write_memory $a 32 [list $v] }
proc setbits {a m} { wr $a [expr {[rd $a] | $m}] }

echo [format "### baseline IDR  A=%04X B=%04X C=%04X D=%04X" [expr {[rd 0x40020010]&0xFFFF}] [expr {[rd 0x40020410]&0xFFFF}] [expr {[rd 0x40020810]&0xFFFF}] [expr {[rd 0x40020C10]&0xFFFF}]]

setbits 0x40023830 0x0F          ;# GPIOA..D clocks
setbits 0x40023840 0x8000        ;# SPI3 clock
# CS lines high first: PB3 (AS5047P), PC13 (DRV8303 nCS)
wr 0x40020418 [expr {1<<3}]
wr 0x40020818 [expr {1<<13}]
# PB3 -> output
set m [rd 0x40020400]; wr 0x40020400 [expr {($m & ~(3<<6)) | (1<<6)}]
# PC13 -> output, PC10..12 -> AF6
set m [rd 0x40020800]
set m [expr {($m & ~((3<<26)|(3<<20)|(3<<22)|(3<<24))) | (1<<26) | (2<<20) | (2<<22) | (2<<24)}]
wr 0x40020800 $m
set af [rd 0x40020824]; wr 0x40020824 [expr {($af & ~0xFFF00) | (6<<8) | (6<<12) | (6<<16)}]
# SPI3: master, mode 1 (CPOL0 CPHA1), 16-bit, /16, SSM/SSI
wr 0x40003C00 0x0B1D
wr 0x40003C00 0x0B5D

proc xfer {cmd} {
    wr 0x40020418 [expr {1<<(3+16)}]       ;# CS low
    wr 0x40003C0C $cmd
    for {set i 0} {$i < 50} {incr i} { if {[rd 0x40003C08] & 1} break }
    set r [expr {[rd 0x40003C0C] & 0xFFFF}]
    for {set i 0} {$i < 50} {incr i} { if {!([rd 0x40003C08] & 0x80)} break }
    wr 0x40020418 [expr {1<<3}]            ;# CS high
    return $r
}
# AS5047P: response arrives in the following frame
xfer 0x4001                ;# ERRFL
set errfl  [xfer 0xFFFC]   ;# -> ERRFL, request DIAAGC
set diag   [xfer 0x7FFE]   ;# -> DIAAGC, request ANGLEUNC
set ang1   [xfer 0x7FFE]
set ang2   [xfer 0xC000]   ;# NOP
echo [format "### ERRFL=0x%04X DIAAGC=0x%04X ANGLEUNC=0x%04X 0x%04X" $errfl $diag $ang1 $ang2]
echo [format "### DIAAGC decode: MAGH=%d MAGL=%d COF=%d LF=%d AGC=%d" [expr {($diag>>10)&1}] [expr {($diag>>11)&1}] [expr {($diag>>9)&1}] [expr {($diag>>8)&1}] [expr {$diag&0xFF}]]
echo [format "### angle(raw14)=%d -> %.1f deg" [expr {$ang2&0x3FFF}] [expr {($ang2&0x3FFF)*360.0/16384}]]

wr 0x40003C00 0x0000
reset run
shutdown
