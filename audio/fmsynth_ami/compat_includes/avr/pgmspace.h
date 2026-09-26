// Shim for AMY, which includes <avr/pgmspace.h> on every Arduino core that is
// not ESP32 for PROGMEM, DMAMEM and FASTRUN.  The STM32 core ships no such
// header on its include path.  On a Cortex-M, const data already stays in
// flash, so all three markers can expand to nothing.
#ifndef FMSYNTH_COMPAT_AVR_PGMSPACE_H
#define FMSYNTH_COMPAT_AVR_PGMSPACE_H

#ifndef PROGMEM
#define PROGMEM
#endif
#ifndef DMAMEM
#define DMAMEM
#endif
#ifndef FASTRUN
#define FASTRUN
#endif

#endif
