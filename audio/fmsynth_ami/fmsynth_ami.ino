// FM synthesizer demo for an STM32F411CEU6 (WeAct Black Pill), using the AMY
// synthesizer library (https://github.com/shorepine/amy, Arduino library
// "AMY Synthesizer").
//
// AMY renders blocks of 256 stereo frames at 44.1 kHz.  Each block goes to a
// MAX98357A over I2S2, fed by a circular DMA buffer so the CPU is free to
// render the next block while the current one plays.  The demo loops over:
//   1. A 2-operator FM tone whose modulation index sweeps up (1:1 ratio):
//      a pure sine that brightens into a sawtooth-like, brassy tone.
//   2. A 2-operator FM bell (Chowning's 1:1.4 ratio, decaying index).
//   3. A tour of AMY's built-in DX7 presets (6-operator FM).
// Progress, render load and heap use are printed on the USB serial port
// (/dev/ttyACM0).
//
// Connections, the same as modules/max98357a (SPI2/I2S2, alternate function 5):
//   MAX98357A BCLK    -> PB13 (I2S2_CK)
//   MAX98357A LRC     -> PB12 (I2S2_WS)
//   MAX98357A DIN     -> PB15 (I2S2_SD)
//   MAX98357A GND     -> Black Pill GND
//   MAX98357A VIN     -> 5 V (or 3.3 V; 5 V gives more output power)
//   MAX98357A GAIN/SD -> leave open for 9 dB gain, (L+R)/2 mono
//
// Porting notes.  AMY supports the ESP32, RP2040/RP2350 and Teensy 4 out of
// the box but has no audio or timing code for the STM32 core:
// - i2s.c compiles to nothing here, so this sketch provides the platform hooks
//   (amy_platform_init, amy_i2s_write, ...) in the extern "C" block below.
// - AMY includes <avr/pgmspace.h>, which the STM32 core does not have;
//   compat_includes/avr/pgmspace.h supplies the few macros it needs.
// - build-flags.txt builds everything with -O2 instead of the core's -Os,
//   since the render loop is what limits the voice count.
// - The F411 has 128 KiB of SRAM, so reverb (64 KiB of delay lines), echo and
//   chorus are left off and the oscillator pool is kept small.

#include <Arduino.h>
#include <AMY-Arduino.h>
#include <malloc.h>
#include "stm32f4xx.h"

namespace {

// ---------------------------------------------------------------------------
// I2S2 + DMA output

// AMY's sample rate is fixed at 44.1 kHz.  The Black Pill runs from a 25 MHz
// HSE.  PLLI2S: 25 MHz / M=25 = 1 MHz VCO input, x N=302 = 302 MHz VCO,
// / R=2 = 151 MHz I2S clock.  (ST's HAL uses the same values for 44.1 kHz.)
constexpr uint32_t PLLI2S_M = 25;
constexpr uint32_t PLLI2S_N = 302;
constexpr uint32_t PLLI2S_R = 2;
constexpr uint32_t I2S_CLK_HZ = 25000000 / PLLI2S_M * PLLI2S_N / PLLI2S_R;

// With 16-bit channels and MCLK output disabled,
//   Fs = I2S_CLK / (32 * (2 * I2SDIV + ODD))
// so I2SDIV=53, ODD=1 gives 44100.47 Hz, 11 ppm fast.  44.1 kHz does not
// divide any reachable I2S clock exactly; the error is far below what an ear
// (or the MAX98357A, which tracks LRCLK) can notice.
constexpr uint32_t I2S_DIV = 53;
constexpr uint32_t I2S_ODD = 1;
constexpr uint32_t I2S_FS_X100 =
    static_cast<uint32_t>(100ULL * I2S_CLK_HZ / (32 * (2 * I2S_DIV + I2S_ODD)));
static_assert(I2S_FS_X100 >= 100 * (AMY_SAMPLE_RATE - 1) &&
                  I2S_FS_X100 <= 100 * (AMY_SAMPLE_RATE + 1),
              "I2S sample rate does not match AMY_SAMPLE_RATE");

// The DMA streams a ring of RING_BLOCKS AMY blocks, forever, from the moment
// the I2S starts.  amy_i2s_write() overwrites a block only once the DMA has
// moved past it, so rendering may run up to RING_BLOCKS - 1 blocks ahead of
// playback.  More blocks absorb longer loop() hiccups (USB serial) at the
// cost of latency: 4 blocks of 256 frames are 23 ms.  Each block is
// AMY_BLOCK_SIZE interleaved L/R frames of 16-bit samples, exactly what
// amy_update() returns.
constexpr uint32_t RING_BLOCKS = 4;
constexpr uint32_t BLOCK_WORDS = AMY_BLOCK_SIZE * AMY_NCHANS;
constexpr uint32_t RING_WORDS = RING_BLOCKS * BLOCK_WORDS;
static_assert(RING_WORDS <= 0xFFFF, "DMA NDTR is 16 bits");

// Starts out silent: the MAX98357A powers up when it detects BCLK and needs a
// few milliseconds to settle before the first rendered block reaches it.
int16_t dmaRing[RING_WORDS];
uint32_t nextBlock = 1;

// SPI2_TX is DMA1 stream 4, channel 0 (RM0383 table 27).
DMA_Stream_TypeDef *const TX_DMA = DMA1_Stream4;

void configureI2sClock() {
  // PLLI2S can only be reconfigured while it is off.  I2SSRC=0 selects it
  // (rather than the external I2S_CKIN pin) as the I2S clock.
  RCC->CR &= ~RCC_CR_PLLI2SON;
  while (RCC->CR & RCC_CR_PLLI2SRDY) {
  }
  RCC->CFGR &= ~RCC_CFGR_I2SSRC;
  RCC->PLLI2SCFGR = (PLLI2S_M << RCC_PLLI2SCFGR_PLLI2SM_Pos) |
                    (PLLI2S_N << RCC_PLLI2SCFGR_PLLI2SN_Pos) |
                    (PLLI2S_R << RCC_PLLI2SCFGR_PLLI2SR_Pos);
  RCC->CR |= RCC_CR_PLLI2SON;
  while ((RCC->CR & RCC_CR_PLLI2SRDY) == 0) {
  }
}

constexpr uint32_t I2S_PINS[] = {12, 13, 15};

void configurePins() {
  RCC->AHB1ENR |= RCC_AHB1ENR_GPIOBEN;

  // PB12 (WS), PB13 (CK), PB15 (SD): alternate function 5, push-pull, high
  // speed, no pull.  Keep the high speed: slow edges garble the audio on a
  // breadboard (see modules/max98357a).
  for (uint32_t pin : I2S_PINS) {
    GPIOB->MODER = (GPIOB->MODER & ~(0x3U << (pin * 2))) | (0x2U << (pin * 2));
    GPIOB->OTYPER &= ~(1U << pin);
    GPIOB->OSPEEDR |= 0x3U << (pin * 2);
    GPIOB->PUPDR &= ~(0x3U << (pin * 2));
    const uint32_t shift = (pin - 8) * 4;
    GPIOB->AFR[1] = (GPIOB->AFR[1] & ~(0xFU << shift)) | (5U << shift);
  }
}

void configureI2s2() {
  RCC->APB1ENR |= RCC_APB1ENR_SPI2EN;

  // Master transmit, Philips I2S standard, 16-bit data in 16-bit channels,
  // clock idle low.  MCLK output is not needed by the MAX98357A.
  SPI2->I2SCFGR = 0;
  SPI2->I2SPR = I2S_DIV | (I2S_ODD << SPI_I2SPR_ODD_Pos);
  SPI2->I2SCFGR = SPI_I2SCFGR_I2SMOD | SPI_I2SCFGR_I2SCFG_1;
}

void configureDma() {
  RCC->AHB1ENR |= RCC_AHB1ENR_DMA1EN;

  TX_DMA->CR &= ~DMA_SxCR_EN;
  while (TX_DMA->CR & DMA_SxCR_EN) {
  }
  DMA1->HIFCR = DMA_HIFCR_CTCIF4 | DMA_HIFCR_CHTIF4 | DMA_HIFCR_CTEIF4 |
                DMA_HIFCR_CDMEIF4 | DMA_HIFCR_CFEIF4;

  // Channel 0, memory to peripheral, 16-bit on both sides, incrementing
  // memory address, circular, high priority, direct mode (no FIFO).  No
  // interrupts: amy_i2s_write() polls the transfer counter instead.
  TX_DMA->PAR = reinterpret_cast<uint32_t>(&SPI2->DR);
  TX_DMA->M0AR = reinterpret_cast<uint32_t>(dmaRing);
  TX_DMA->NDTR = RING_WORDS;
  TX_DMA->FCR = 0;
  TX_DMA->CR = (0U << DMA_SxCR_CHSEL_Pos) | DMA_SxCR_PL_1 | DMA_SxCR_MSIZE_0 |
               DMA_SxCR_PSIZE_0 | DMA_SxCR_MINC | DMA_SxCR_CIRC |
               DMA_SxCR_DIR_0;
  TX_DMA->CR |= DMA_SxCR_EN;
}

void startI2S() {
  configureI2sClock();
  configurePins();
  configureI2s2();
  configureDma();

  // TXE is already set, so the DMA loads the first word (the left sample of
  // frame 0) as soon as it is allowed to.  Enabling I2S afterwards makes that
  // word go out in the left slot, and the L/R interleaving of the ring stays
  // aligned with WS from then on.
  SPI2->CR2 |= SPI_CR2_TXDMAEN;
  SPI2->I2SCFGR |= SPI_I2SCFGR_I2SE;
}

void stopI2S() {
  // Stop feeding new words, let the last one shift out (wait for TXE, then
  // for BSY to clear), then disable I2S.  With BCLK stopped the MAX98357A
  // mutes its output.
  SPI2->CR2 &= ~SPI_CR2_TXDMAEN;
  TX_DMA->CR &= ~DMA_SxCR_EN;
  while ((SPI2->SR & SPI_SR_TXE) == 0) {
  }
  while (SPI2->SR & SPI_SR_BSY) {
  }
  SPI2->I2SCFGR &= ~SPI_I2SCFGR_I2SE;

  // A disabled I2S block may stop driving its pins, and a floating BCLK wire
  // picks up noise the amplifier takes for clock edges.  Hold them low.
  for (uint32_t pin : I2S_PINS) {
    GPIOB->BSRR = 1U << (pin + 16);
    GPIOB->MODER = (GPIOB->MODER & ~(0x3U << (pin * 2))) | (0x1U << (pin * 2));
  }
}

// Index of the ring block the DMA is reading.  NDTR counts the words left
// before the pointer wraps back to the start of the ring.
uint32_t blockBeingPlayed() {
  const uint32_t wordsDone = RING_WORDS - TX_DMA->NDTR;
  return (wordsDone / BLOCK_WORDS) % RING_BLOCKS;
}

// ---------------------------------------------------------------------------
// Render-load bookkeeping, shown on the serial port

uint32_t peakRenderUs = 0;
volatile bool amyOverloaded = false;

void onAmyOverload(float) {
  // Called from inside amy_render_audio() after AMY has already silenced and
  // reset its oscillators; loop() recovers.
  amyOverloaded = true;
}

}  // namespace

// ---------------------------------------------------------------------------
// AMY platform hooks.  AMY calls these from amy_start() and amy_update(); on
// the ESP32, RP2040 and Teensy they live in AMY's i2s.c.

extern "C" {

void amy_platform_init() {
  if (AMY_HAS_I2S) {
    startI2S();
  }
}

void amy_platform_deinit() {
  if (AMY_HAS_I2S) {
    stopI2S();
  }
}

// Called at the start of every amy_update(), before rendering.  Other ports
// poll UART MIDI here; this demo has no MIDI input.
void amy_update_tasks() { amy_execute_deltas(); }

int16_t *amy_render_audio() {
  // AMY's own timer (amy_get_us) falls back to gettimeofday(), which the
  // STM32 core does not keep, so time the render with micros().  AMY smooths
  // this into its render load and resets itself if the load stays too high.
  const uint32_t t0 = micros();
  amy_render(0, AMY_OSCS, 0);
  int16_t *block = amy_fill_buffer();
  const uint32_t renderUs = micros() - t0;
  if (renderUs > peakRenderUs) {
    peakRenderUs = renderUs;
  }
  amy_overload_check(renderUs);
  return block;
}

// amy_update() hands every rendered block to this function.  It blocks until
// the DMA has finished reading the ring block to be overwritten, which paces
// loop() to the audio clock.
size_t amy_i2s_write(const uint8_t *buffer, size_t nbytes) {
  if (nbytes > BLOCK_WORDS * sizeof(int16_t)) {
    nbytes = BLOCK_WORDS * sizeof(int16_t);
  }
  while (blockBeingPlayed() == nextBlock) {
  }
  memcpy(&dmaRing[nextBlock * BLOCK_WORDS], buffer, nbytes);
  nextBlock = (nextBlock + 1) % RING_BLOCKS;
  return nbytes;
}

// No MIDI transport on this port.
void run_midi() {}
void stop_midi() {}

}  // extern "C"

namespace {

// ---------------------------------------------------------------------------
// FM patches built from scratch
//
// An AMY FM voice is one ALGO oscillator driving up to six SINE operators,
// wired as one of the 32 DX7 algorithms.  These patches use DX7 algorithm 1
// with only operators 2 and 1: operator 2 (the modulator, osc 2) modulates
// the phase of operator 1 (the carrier, osc 1).  algo_source (O) lists the
// operator oscillators from operator 6 down to operator 1, empty if unused.
//
// Each operator's frequency is `ratio` (I) times the note frequency.  The
// modulator's amplitude is the modulation index: it sets how much energy
// spreads from the carrier into sidebands at carrier +/- k * modulator.
// Operator amplitudes (a) are coefficient lists const,note,vel,eg0: level
// times envelope 0, not scaled by velocity (the ALGO osc applies that once).
// AMY divides an FM voice's output by 4, so a carrier level of 6 is about as
// loud as one voice of the DX7 presets.
// Envelopes (A) are time(ms),level pairs, each time counted from the
// previous breakpoint; the last pair is the release after note-off.
//
// User patches live in AMY's RAM as numbers 1024 and up.

constexpr uint16_t PATCH_FM_SWEEP = 1024;
constexpr uint16_t PATCH_FM_BELL = 1025;

void defineFmPatches() {
  // Index sweep, 1:1 ratio.  With equal frequencies the sidebands land on
  // harmonics of the note, so the tone stays pitched and just brightens as
  // the modulator's envelope rises from 0 to 1 over 4 seconds.
  amy_add_message(const_cast<char *>(
      "K1024v2w0I1a1,0,0,1A0,0,4000,1,300,0Z"));   // modulator
  amy_add_message(const_cast<char *>(
      "K1024v1w0I1a6,0,0,1A10,1,300,0Z"));         // carrier
  amy_add_message(const_cast<char *>(
      "K1024v0w8o1O,,,,2,1a1,0,1,0Z"));            // ALGO, algorithm 1

  // Bell, 1:1.4 ratio (John Chowning, 1973).  The non-integer ratio puts the
  // sidebands between the harmonics, which the ear hears as a clangorous,
  // bell-like spectrum.  The index decays faster than the amplitude, so the
  // strike is bright and the tail mellows towards a pure sine.
  amy_add_message(const_cast<char *>(
      "K1025v2w0I1.4a1,0,0,1A0,1,2500,0,500,0Z"));  // modulator
  amy_add_message(const_cast<char *>(
      "K1025v1w0I1a6,0,0,1A0,1,5000,0,500,0Z"));    // carrier
  amy_add_message(const_cast<char *>(
      "K1025v0w8o1O,,,,2,1a1,0,1,0Z"));             // ALGO, algorithm 1
}

// ---------------------------------------------------------------------------
// Demo sequence
//
// Each part loads one patch on synth 1 and plays a phrase on it.  Synths are
// AMY's voice managers: a note sent to the synth is given to a free voice
// (or steals the oldest), each voice being a copy of the patch.

constexpr uint8_t SYNTH = 1;
constexpr uint32_t STEP_MS = 250;  // an eighth note at 120 BPM
constexpr float NOTE_VELOCITY = 0.8f;

// AMY's output volume (bus 0).  At its default of 1 the mix peaks near 20 %
// of full scale with four DX7 voices; 2.5 makes good use of the 16 bits.
// Much higher and chords run into AMY's output soft-clipper.
constexpr float OUTPUT_VOLUME = 2.5f;

// A DX7 voice takes 8 oscillators (ALGO, LFO and six operators), so 4 voices
// fit in MAX_OSCS.  Each oscillator AMY allocates costs about 650 bytes of
// heap, and AMY's event pool takes another 40 KiB, so MAX_OSCS is what
// bounds RAM use.
//
// CPU is the tighter limit.  Measured at 96 MHz, each busy DX7 voice takes
// 22-25 % of the render budget, on top of a fixed per-block cost; a released
// note stays busy until its envelope ends.  With 4 voices most presets peak
// at 89-91 %, but Marimba and Bass reached 95-96 % and Strings (4 voices)
// overloaded, so those get 3 voices, which the phrase below never exceeds.
// The 4th voice of the other parts lets release tails ring instead of being
// stolen.
constexpr uint16_t DX7_VOICES = 4;
constexpr uint16_t DX7_REDUCED_VOICES = 3;
constexpr uint16_t FM2_VOICES = 4;
constexpr uint16_t MAX_OSCS = 40;

struct Note {
  uint8_t step;   // when it starts, in steps from the start of the part
  uint8_t steps;  // how long it is held
  uint8_t note;   // MIDI note number
};

struct Part {
  const char *name;
  uint16_t patch;
  uint16_t voices;
  int8_t transpose;
  const Note *notes;  // sorted by step
  uint8_t noteCount;
  uint8_t lengthSteps;  // includes the rest that lets the last note ring
};

constexpr Note SWEEP_NOTES[] = {{0, 20, 45}};  // A2, held for 5 s

constexpr Note BELL_NOTES[] = {
    {0, 16, 76}, {4, 16, 83}, {8, 16, 79}, {12, 16, 72},
};

// C - Am - F - G as broken chords over a bass note, ending on C.  At most 3
// notes are held at once.
constexpr Note CHORD_NOTES[] = {
    {0, 4, 48},  {0, 2, 60},  {1, 2, 64},  {2, 2, 67},  {3, 2, 72},   // C
    {4, 4, 45},  {4, 2, 57},  {5, 2, 60},  {6, 2, 64},  {7, 2, 69},   // Am
    {8, 4, 41},  {8, 2, 53},  {9, 2, 57},  {10, 2, 60}, {11, 2, 65},  // F
    {12, 4, 43}, {12, 2, 55}, {13, 2, 59}, {14, 2, 62}, {15, 1, 67},  // G
    {16, 8, 48}, {16, 8, 64}, {16, 8, 67},                            // C
};

template <size_t N>
constexpr uint8_t countOf(const Note (&)[N]) {
  return N;
}

#define DX7_PART(name, patch, voices, transpose)                      \
  {                                                                   \
    name, patch, voices, transpose, CHORD_NOTES, countOf(CHORD_NOTES), \
        28                                                            \
  }

const Part PARTS[] = {
    {"2-op FM, index sweep (1:1)", PATCH_FM_SWEEP, 1, 0, SWEEP_NOTES,
     countOf(SWEEP_NOTES), 24},
    {"2-op FM bell (1:1.4)", PATCH_FM_BELL, FM2_VOICES, 0, BELL_NOTES,
     countOf(BELL_NOTES), 32},
    DX7_PART("DX7 E.PIANO 1", 138, DX7_VOICES, 0),
    DX7_PART("DX7 BRASS 1", 128, DX7_VOICES, 0),
    DX7_PART("DX7 MARIMBA", 149, DX7_REDUCED_VOICES, 0),
    DX7_PART("DX7 TUB BELLS", 153, DX7_VOICES, 0),
    DX7_PART("DX7 HARPSICH 1", 146, DX7_VOICES, 0),
    DX7_PART("DX7 STRINGS 1", 131, DX7_REDUCED_VOICES, 0),
    DX7_PART("DX7 BASS 1", 142, DX7_REDUCED_VOICES, -12),
};
constexpr size_t PART_COUNT = sizeof(PARTS) / sizeof(PARTS[0]);

size_t partIndex = 0;
uint32_t partStartMs = 0;
uint8_t nextNote = 0;

// The demo plays once, then waits for the Black Pill's KEY button.  KEY
// shorts PA0 (USER_BTN) to ground, so it reads LOW while pressed.  A press
// only counts after the button has been seen released, so holding KEY as
// the demo ends does not restart it, and contact bounce cannot either:
// the button is not read again until the demo is over.
bool waitingForKey = false;
bool keyWasUp = false;

bool keyPressed() {
  const bool down = digitalRead(USER_BTN) == LOW;
  const bool pressed = down && keyWasUp;
  keyWasUp = !down;
  return pressed;
}

void sendNote(uint8_t note, float velocity, uint32_t time) {
  amy_event e = amy_default_event();
  e.synth = SYNTH;
  e.midi_note = note;
  e.velocity = velocity;  // 0 is a note-off
  e.time = time;
  amy_add_event(&e);
}

void startPart(uint32_t now) {
  const Part &part = PARTS[partIndex];

  // Loading a patch rebuilds the synth's voices, which also silences any
  // note still releasing from the previous part.
  amy_event e = amy_default_event();
  e.synth = SYNTH;
  e.patch_number = part.patch;
  e.num_voices = part.voices;
  amy_add_event(&e);

  partStartMs = now;
  nextNote = 0;
  Serial.print("Part: ");
  Serial.print(part.name);
  Serial.print(" (patch ");
  Serial.print(part.patch);
  Serial.println(")");
}

// Moves on to the next part, or after the last one waits for KEY.
void nextPart(uint32_t now) {
  if (partIndex + 1 < PART_COUNT) {
    ++partIndex;
    startPart(now);
  } else {
    waitingForKey = true;
    keyWasUp = false;
    Serial.println("Demo finished; press KEY (PA0) to play it again.");
  }
}

// Called once per rendered block.  Notes start as their step comes up; each
// note-off is scheduled at once with an AMY event time, so the note lengths
// do not depend on how promptly loop() comes back.
void runDemo(uint32_t now) {
  if (waitingForKey) {
    if (keyPressed()) {
      waitingForKey = false;
      partIndex = 0;
      startPart(now);
    }
    return;
  }

  const Part &part = PARTS[partIndex];
  const uint32_t elapsedSteps = (now - partStartMs) / STEP_MS;

  while (nextNote < part.noteCount && part.notes[nextNote].step <= elapsedSteps) {
    const Note &n = part.notes[nextNote++];
    const uint8_t note = n.note + part.transpose;
    sendNote(note, NOTE_VELOCITY, 0);
    sendNote(note, 0, partStartMs + (n.step + n.steps) * STEP_MS);
  }

  if (elapsedSteps >= part.lengthSteps) {
    nextPart(now);
  }
}


void reportLoad(uint32_t now) {
  static uint32_t lastReportMs = 0;
  if (now - lastReportMs < 2000) {
    return;
  }
  lastReportMs = now;

  // Load = render time / audio time per block.  Above 100 % the ring runs
  // dry and the output stutters; AMY resets itself if the smoothed load
  // stays above 98 % for 250 ms.
  const float avg = 100.0f * amy_get_render_load();
  const float peak = 100.0f * peakRenderUs / AMY_BLOCK_US;
  peakRenderUs = 0;
  Serial.print("  render load: avg ");
  Serial.print(avg, 1);
  Serial.print(" %, peak ");
  Serial.print(peak, 1);
  Serial.println(" %");
}

}  // namespace

void setup() {
  Serial.begin(115200);
  pinMode(USER_BTN, INPUT_PULLUP);

  amy_config_t config = amy_default_config();
  config.audio = AMY_AUDIO_IS_I2S;  // output through amy_i2s_write() above
  config.midi = AMY_MIDI_IS_NONE;
  config.platform.multicore = 0;
  config.platform.multithread = 0;
  config.features.reverb = 0;
  config.features.echo = 0;
  config.features.chorus = 0;
  config.features.partials = 0;
  config.features.custom = 0;
  config.features.default_synths = 0;
  config.features.startup_bleep = 0;
  config.ks_oscs = 0;  // no Karplus-Strong strings, saves a 3 KiB buffer
  config.max_oscs = MAX_OSCS;
  config.max_buses = 1;
  config.max_voices = 8;
  config.max_synths = 2;
  config.max_memory_patches = 2;
  config.max_sequencer_tags = 8;
  config.amy_external_overload_hook = onAmyOverload;
  amy_start(config);

  amy_event e = amy_default_event();
  e.volume = OUTPUT_VOLUME;
  amy_add_event(&e);

  defineFmPatches();
  startPart(amy_sysclock());

  const struct mallinfo heap = mallinfo();
  Serial.print("AMY started, heap in use: ");
  Serial.print(heap.uordblks);
  Serial.println(" bytes");
}

void loop() {
  // Renders one block and queues it for the DMA (see amy_i2s_write()).
  amy_update();

  // amy_sysclock() counts rendered audio, in milliseconds.
  const uint32_t now = amy_sysclock();

  if (amyOverloaded) {
    // Retrying the same part would most likely overload again, so move on
    // (unless the demo is already over and waiting for KEY).
    amyOverloaded = false;
    if (!waitingForKey) {
      Serial.print("AMY overloaded and reset itself during ");
      Serial.print(PARTS[partIndex].name);
      Serial.println("; skipping to the next part.  Give it fewer voices.");
      nextPart(now);
    }
  }

  runDemo(now);
  reportLoad(now);
}
