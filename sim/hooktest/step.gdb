set pagination off
set confirm off
target sim --model bf524 --environment virtual --memory-size 64m
load
starti
set $i = 0
while $i < 30
  printf "%#010x  ", $pc
  x/i $pc
  stepi
  set $i = $i + 1
end
printf "R0=%#x P0=%#x RETS=%#x SP=%#x counter=%d\n", $r0, $p0, $rets, $sp, *(int*)0x01e0001c
