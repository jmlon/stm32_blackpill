// Shim for Mozzi, which includes <avr/pgmspace.h> on the STM32 core for its
// pgm_read_*() table accessors.  STM32duino 3.x ships a complete AVR
// compatibility header, but no longer puts it on the include path; forward
// to it (cores/arduino is on the path, so this relative name resolves).
#include "api/deprecated-avr-comp/avr/pgmspace.h"
