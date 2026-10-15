set pagination off
set confirm off
target sim --model bf524 --environment virtual --memory-size 64m
load
starti
x/3i 0x01a42344
x/2xw 0x01a42344
break *0x01e00016
continue
printf "at %#x counter=%d R0=%#x P0=%#x RETS=%#x SP=%#x\n", $pc, *(int*)0x01e0001c, $r0, $p0, $rets, $sp
stepi
printf "after stepi pc=%#x seqstat=%#x ipend=%#x\n", $pc, $seqstat, $ipend
info registers seqstat retx ipend imask
