# Demos of FM Synthesis libraries

Three FM synthesizer demos for the STM32F411CEU6 Black Pill (96 MHz, 512 KB
flash, 128 KB SRAM), each built on a different library and played through a
MAX98357A I2S amplifier (wiring as in `modules/max98357a`).  All three share
the same output path: SPI2 in I2S master mode, fed by a circular DMA buffer,
with the sample rate set by PLLI2S.  Each plays its demo once, then waits for
the KEY button (PA0) to play it again, and reports CPU load on the USB serial
port.

| Demo | Library | What it plays |
|---|---|---|
| `fmsynth_ami` | AMY | 2 hand-built 2-operator patches, then 7 of AMY's built-in DX7 presets (6 operators) |
| `fmsynth_mozzi` | Mozzi | 7 textbook 2-operator patches (mostly after Chowning, 1973), 4 voices |
| `fmsynth_leaf` | LEAF | The same 7 patches, phrase and 4 voices as Mozzi; bell and e-piano add a detuned second carrier |

## Libraries

- AMY: arduino-cli lib install "AMY Synthesizer"
- Mozzi: arduino-cli lib install Mozzi
- LEAF: tools/install-leaf.sh

LEAF is not in the Arduino library index, and its current upstream head does
not compile (`leaf-physical.c`, July 2026).  The script installs the pinned
commit `751f3c8` (March 2026) as an Arduino library.

Neither AMY nor Mozzi builds out of the box on the STM32 core (see *Porting
notes*).

## Comparison

| | AMY | Mozzi | LEAF |
|---|---|---|---|
| Arithmetic | 32-bit fixed point | 8-bit wavetables and envelopes, fixed point | 32-bit float (F411 FPU) |
| Sample rate | 44.1 kHz (fixed by AMY) | 32.768 kHz | 48 kHz (exact) |
| FM model | DX7: 6 operators, 32 algorithms, feedback | 2 `Oscil`s with `phMod()` (phase modulation) | 2 `tCycle`s, phase modulation |
| Envelopes | DX7 breakpoint envelopes | `ADSR`, linear, 8-bit output | `tADSRS`, exponential (analog-style) |
| Voice allocation | Built-in synths and voice stealing | Written in the sketch | `tSimplePoly` |
| Patches | 128 DX7, 128 Juno, piano, drums built in | Build your own | Build your own |
| Porting effort on STM32 | High | Low | Medium |

### Sound quality (by ear, on the MAX98357A)

1. **AMY sounds best by far.**  The gap is the patches, not the engine:
   AMY's DX7 presets have 6 operators, several detuned carriers, feedback,
   pitch envelopes and carefully tuned envelopes.  No 2-operator patch comes
   close, especially for brass and bass.
2. **LEAF is clearly better than Mozzi, but not as good as AMY.**  With the
   same patches as Mozzi it has no audible hiss, clean bell tails, and
   smoother, analog-style envelopes.
3. **Mozzi is noticeably hissy** (8-bit wavetables).  Short, loud patches hide
   this well: its brass, bass and marimba are good.  Its **bells are awful**:
   the bell's 5 s decay steps through Mozzi's 8-bit envelope levels audibly,
   and a modulation index of 10 on 8-bit modulator samples turns into noise.

## Performance

Figures for the demos as committed.  Flash and static RAM are from the build;
CPU load is the render time over the audio time, measured on the board.

### Memory usage

| | Flash | Static RAM | Dynamic memory |
|---|---|---|---|
| AMY | 433 KB (82 %) | 12.1 KB | ~85 KB heap (estimate)¹ |
| Mozzi | 33 KB (6 %) | 7.6 KB | none |
| LEAF | 48 KB (9 %) | 13.1 KB | 3.2 KB of a 6 KB pool, included in static RAM |

¹ Measured on a PC build of AMY and scaled to the Cortex-M4's 4-byte
pointers.  Mostly AMY's event pool (a fixed 40 KB) and about 650 bytes per
oscillator (40 allocated).  The sketch prints the real figure at startup.

- **AMY's flash** is mostly its built-in patch and PCM tables.  Even with
  reverb, echo and chorus off, its RAM use leaves only 20–30 KB for the
  stack.  Reverb alone would need 64 KB, half the chip's SRAM.
- **Mozzi** is the smallest in every respect.
- **LEAF** gets float quality for 15 KB more flash than Mozzi.

### CPU usage

| | Measured load |
|---|---|
| AMY | 22–25 % per sounding DX7 voice, plus a fixed per-block cost; 4 voices peaked at 89–96 %.  One 2-operator voice alone costs 19 %, four 58 %. |
| Mozzi | 10–11 % constant.  All 4 voices are computed all the time, even when silent. |
| LEAF | About 11 % per voice (13 % with a detuned second carrier): peaks of 45–47 % with 4 voices, 58 % for bell and e-piano.  Silent voices are skipped, so it drops to a few percent between notes. |

- **AMY is at the limit** with 4 DX7 voices at 96 MHz.  Strings, Marimba and
  Bass run 3 voices in the demo to stay clear of AMY's overload failsafe,
  which resets it above 98 %.
- **Mozzi is the cheapest by a wide margin:** fixed-point integer math on
  8-bit tables, at a lower sample rate.
- **LEAF spends its time in the calls to its small per-sample functions**
  (`tCycle_tick`, `tADSRS_tick`), which cannot be inlined across files.  So
  `-O2` measured no faster than the default `-Os`.

## Conclusions

- **For the best size/quality ratio, use LEAF.**  It sounds clean, costs
  48 KB of flash and a little over 13 KB of RAM, and leaves half the CPU free
  with 4 voices.
- **Use Mozzi** when flash, RAM or CPU is the tightest constraint and some
  hiss is acceptable, and avoid long, soft decays such as bells.
- **Use AMY** when the sound itself matters most and you can spend most of
  the flash and CPU on it, e.g. a dedicated synth rather than a feature in a
  larger project.  A faster chip (the F411 is rated for 100 MHz) or fewer
  voices is the only way to more polyphony.

### Lessons learned while tuning the 2-operator patches

- **Phase modulation sounded better than frequency modulation** (retuning
  the carrier every sample), once feedback was involved.  It is also
  cheaper.  Chowning's two formulations give the same spectrum only for a
  pure sine modulator.
- **A detuned second carrier suits only some patches.**  Both carriers carry
  the same sidebands, so the whole note beats.  That is a pleasant shimmer on
  the electric piano and bells, but it made sustained brass and bass pulse.
- **Richer 2-operator patches did not close the gap with AMY.**  Modulator
  feedback and a low-pass filter on brass and bass sounded worse, not
  better.  Getting close to the DX7 would take 4+ operators per voice, i.e. a
  small DX7 engine, which was rejected to keep the footprint small.

## Porting notes

- **AMY** has no platform code for the STM32 core.  `fmsynth_ami` supplies
  the functions AMY expects from each platform (`amy_platform_init`,
  `amy_i2s_write`, `amy_render_audio`, ...), and a `compat_includes/`
  stand-in for `avr/pgmspace.h`.  It builds with `-O2`.  The configuration
  turns off reverb, echo, chorus and Karplus-Strong, and caps the oscillator
  pool at 40, to fit in 128 KB of SRAM.
- **Mozzi** supports the STM32 core directly.  `MOZZI_OUTPUT_EXTERNAL_CUSTOM`
  lets the sketch feed the DMA buffer itself.  STM32duino 3.x no longer puts
  its `avr/pgmspace.h` on the include path; a one-line forwarding header in
  `compat_includes/` fixes that.
- **LEAF** is plain C with no dependencies, but ships no Arduino library
  layout, hence `tools/install-leaf.sh`.  Avoid `tSVF`: it links 32 KB of
  filter lookup tables.  A `tBiQuad` with computed coefficients does the same
  job without them.
