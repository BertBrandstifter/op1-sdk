set pagination off
set confirm off
target sim --model bf524 --environment virtual --memory-size 64m
load
break *0x01a42344
run
printf "stopped at   pc=%#010x\n", $pc
printf "R0           %#010x (expect 0xcafe1234)\n", $r0
printf "P0           %#010x (expect 0xbeef5678)\n", $p0
printf "RETS         %#010x (expect 0x01f00024 = _back)\n", $rets
printf "SP           %#010x (expect 0x01f7fffc)\n", $sp
printf "counter      %d (expect 1)\n", *(int *)0x01e0001c
quit
