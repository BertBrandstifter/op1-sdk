# Audio CPU load panel (debug)

Answers: how much of the OP-1's DSP does the audio take, measured on the device? What core
clock does TE run? How much of that load is our SDK engines? The simulator's cycle counts leave
out SDRAM and cache latency, so they are a lower bound. This panel shows the real number.

## Open it on the OP-1

Not part of `op1plus`. Build the debug image (op1plus + cpuinfo, same boot text) and flash it:

```sh
python3 tools/compose.py recipes/op1cpu.json ~/path/to/op1_246.op1
python3 tools/flash.py build/apps/op1cpu/build/op1cpu.op1 --eject     # te-boot upload mode
```

Flash `op1plus` again when done.

## Controls

- shift + speech: panel on / off. The **first** opening starts the measurement. It keeps running
  when the panel is closed (until power-off). Nothing runs before the first opening.
- Conflicts with `meminfo` and `diskinfo` (same key).

## How it measures

- TE's audio block callback (`TE_AUDIO_BLOCK_CB`, `0xff80036c`) runs once per 64-frame block from
  the IVG11 handler. The first opening wraps it. The wrapper reads `CYCLES` before and after TE's
  callback. Work = cycles inside. Period = cycles since the last block. Load = work / period.
- The main loop re-wraps the callback if TE sets it again (`REWRAP` counts how often).
- If the cycle counter was off (`SYSCFG.CCEN`), the wrapper turns it on. That is safe because it
  runs in supervisor mode inside the interrupt. `CCEN WAS ON` / `SET BY US` shows which case
  happened.
- SDK engines: `enginehost` adds up the `CYCLES` spent in our engines' renders (`eh_cycles`,
  only after the panel opened). If they run inside TE's callback, this number is part of the
  load. `OUTSIDE CB?` means the engines took more cycles than the callback, so the voices are
  rendered somewhere else.

## The panel

- `AUDIO LOAD avg PEAK peak`: percent of the block period spent in TE's audio callback, average
  and worst block of the last window (172 blocks, ~0.25 s). Green < 70, amber < 90, red ≥ 90.
- `BLOCK AVG / MAX / WORST / OF`: the same in cycles (K = 1000). WORST = the worst block since
  the panel opened. OF = the period.
- `SDK ENGINES`: our engines' share of the period, in percent and in cycles.
- `CCLK`: core clock = period × 44100 / 64. This assumes the codec runs at 44.1 kHz.
  `CLKIN` = the input clock this implies, from the PLL settings. A round crystal value
  (12, 24, 25, 27 MHz…) confirms the 44.1 kHz assumption.
- `PLL CTL / MSEL / DF`, `PLL DIV / CSEL / SSEL`, `VR CTL`: the clock and voltage registers as
  read. `CB`: TE's callback address.
- Graph: ~37 s history. Green = average, amber = peak per window. The line is 70 %.

## What it does not show

The load of everything outside the audio callback: GUI, key handling, file I/O, and voices or FX
if TE renders them in a thread. The CPU idle time is not measured.

## Test

`sim/run_citest.py` (composes `recipes/op1cpu.json`): nothing runs before the panel opens; the
wrap keeps TE's callback running every block; the counter is enabled; work < period, engines <
work; a re-set callback is wrapped again. Panel screenshot: `build/demo/cpuinfo_screen.png`.
