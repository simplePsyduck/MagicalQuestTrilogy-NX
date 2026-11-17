# Performance notes (core speed pass)

Everything here is checked to draw the **same picture and play the same sound** as before: this pass changed
how the answers are worked out, not the answers.

## What changed
| Where | Change | Runs on |
|---|---|---|
| `core/snes.h`, `snes.cpp` | `tick()` (called for every bus access and idle cycle, ~40-60k times a frame) now does the usual "nothing happens in these cycles" case inline; the event/line-end handling moved to `tickSlow()`. | emulation thread |
| `core/ppu.cpp` `renderObj` | Sprites: most of the 128 OAM entries are rejected before any size/flag work; each 8-pixel tile row is decoded with the same spread tables the backgrounds use; the colour source (palette / recolour / override) is chosen once per sprite instead of once per pixel. | emulation thread |
| `core/snes.cpp` `cpuRead`/`cpuWrite` | Reads of plain memory (WRAM, ROM, patch area, SRAM) go straight through a per-8 KB-page pointer table, and access times come from a table (rebuilt on `$420D`, reset, state load). `busRead` is unchanged and still handles every I/O register. Checked against `busRead` for all 16 M addresses, ROM sizes 512 KB-6 MB, both fast/slow ROM. | emulation thread |
| `core/ppu.cpp` `renderBgFast` | A tile row with no pixels in it (most of a sparse layer) only marks its pixels transparent. | emulation thread |
| `core/ppu.cpp` `composeCore` | The common line (no colour clipping/maths): the top layer of a pixel is found with one compare per layer (rank and colour packed in one word), loops are specialised for the number of layers and for "no window", and the 4:3 screen and the margins are separate loops, so no per-pixel margin/dim/freeze tests in the middle. Lines with colour maths use the old path. | video thread |
| `platform/android/video_gles.cpp` | Backdrop pass (the blurred sides when the picture does not fill the screen) is scissored to the bars instead of shading the whole screen under the picture; colour settings left at default (gamma, contrast, brightness, saturation, profile) are skipped in the shader (the gamma `pow` ran on every pixel). | GPU |
| `switch/Makefile` | `OPT ?= -O2`, so `make clean && make OPT=-O3` is a one-word A/B test. | build |

## How it was checked (host PC)
* `tools/perf/ppu_equiv_test.cpp`: 9000 random scenes, hashes identical to the original `ppu.cpp`.
* `tools/perf/ppu_equiv_hooks_test.cpp` (new): the same with this port's frozen-margin / sub-fill / margin-maths
  hooks switched on at random, 4000 scenes, identical.
* `tools/perf/prof.cpp`: picture hash and audio sample count unchanged; the host frame time dropped about 7-10%
  on the benchmark ROM (a PC, with a ROM that is light on CPU work: treat it as a direction, not a Switch figure).

## Game CPU Speed (new setting)
Options > Speed > **Game CPU Speed**: 100% (stock) to 400% in steps of 25. It speeds up the *emulated* game
processor only (the picture chip, DMA and sound run as before), so scenes that slowed down on the real console
finish in time. At 100% the output is bit-identical to before; saved in the settings file under `[emulation]`
(`cpu_percent`).
**It costs Switch CPU time**: the game's code, and its waiting loops, run that many times more often. On the PC
benchmark the frame time went 1.43 ms at 100% to about 1.5 / 1.6 / 2.0 ms at 150% / 200% / 400%. Use the lowest
setting that removes the slowdown, and judge it with the `emulation ms` figure in the log. A few games may
misbehave at the top settings (timing-sensitive sound code or effects).

## NOT measured: the Switch itself
There was no Switch (or ARM compiler) here, so none of this has run on hardware. The expected effect is a few
percent of the emulation thread plus less work on the video thread and GPU; it is not a guarantee of a locked 60.

## Testing at stock clocks
**This port now starts at the stock CPU clock (1020 MHz).** Earlier builds raised it to 1785 MHz unless
`no_overclock.txt` was present; that is reversed: the boost happens only if an empty `overclock.txt` is beside the .nro.
The log's `CPU ... MHz` shows the clock in use.

Play one busy scene for a minute, then read the `perf` lines in the log file next to the .nro (`mq_port.log`). What to look at:
* `emulation X ms per game frame`: must stay well under 16.6 at stock clock.
* `the emulation waited X ms per pass` for the video thread: if this is large, the video thread is the limit.
* `over a refresh (%)` and `catch-ups`: the dropped frames. `sound: underruns`.
* `present wait` and `render`: GPU-side.
Send me a few of those lines and I can say which side is the bottleneck and what to do next.

## Next steps, in the order I would try them (each needs a Switch to judge)
1. `make clean && make OPT=-O3` and compare the `emulation ms` figure.
2. Turn High Frame Rate / rewind off while measuring (they add work every line / every few frames).
3. NEON for the plain compose loop, if the log shows the video thread is the limit.
