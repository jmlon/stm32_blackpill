// FM synthesizer demo for an STM32F411CEU6 (WeAct Black Pill), using the
// Mozzi sound synthesis library (https://github.com/sensorium/Mozzi, 2.x).
//
// Where the AMY demo (audio/fmsynth) plays full DX7 patches, this one builds
// FM from Mozzi's small building blocks: every voice is two Oscils, a
// modulator driving the phase of a carrier (Oscil::phMod), each with its own
// ADSR.  Four such voices play a tour of classic 2-operator patches, most
// after John Chowning's 1973 paper that introduced FM synthesis:
//   index sweep (1:1), bell (1:1.4), e-piano tine (1:14), brass (1:1),
//   clarinet (3:2), marimba (1:4) and bass (1:1).
// Each patch's name, then the CPU load and output-buffer headroom, are
// printed on the USB serial port (/dev/ttyACM0).
//
// Connections, the same as modules/max98357a (SPI2/I2S2, alternate function 5):
//   MAX98357A BCLK    -> PB13 (I2S2_CK)
//   MAX98357A LRC     -> PB12 (I2S2_WS)
//   MAX98357A DIN     -> PB15 (I2S2_SD)
//   MAX98357A GND     -> Black Pill GND
//   MAX98357A VIN     -> 5 V (or 3.3 V; 5 V gives more output power)
//   MAX98357A GAIN/SD -> leave open for 9 dB gain, (L+R)/2 mono
//
// Mozzi's STM32duino port offers PWM output or "external" output.  This
// sketch uses MOZZI_OUTPUT_EXTERNAL_CUSTOM: Mozzi computes a sample whenever
// canBufferAudioOutput() says there is room and hands it to audioOutput().
// Both are defined here and feed a ring buffer that DMA streams to the I2S
// peripheral, so the I2S clock alone sets the sample rate and no timer
// interrupt is involved.

#include <MozziConfigValues.h>
#define MOZZI_AUDIO_MODE MOZZI_OUTPUT_EXTERNAL_CUSTOM
#define MOZZI_AUDIO_CHANNELS MOZZI_MONO
#define MOZZI_AUDIO_RATE 32768  // Mozzi prefers powers of two
#define MOZZI_AUDIO_BITS 16
#define MOZZI_CONTROL_RATE 256  // updateControl() calls per second
#define MOZZI_ANALOG_READ MOZZI_ANALOG_READ_NONE

#include <Mozzi.h>
#include <ADSR.h>
#include <Oscil.h>
#include <mozzi_midi.h>
#include <tables/cos8192_int8.h>
#include "stm32f4xx.h"

namespace {

// ---------------------------------------------------------------------------
// I2S2 + DMA output

// The Black Pill runs from a 25 MHz HSE.  PLLI2S: 25 MHz / M=25 = 1 MHz VCO
// input, x N=367 = 367 MHz VCO, / R=5 = 73.4 MHz I2S clock.
constexpr uint32_t PLLI2S_M = 25;
constexpr uint32_t PLLI2S_N = 367;
constexpr uint32_t PLLI2S_R = 5;
constexpr uint32_t I2S_CLK_HZ = 25000000 / PLLI2S_M * PLLI2S_N / PLLI2S_R;

// With 16-bit channels and MCLK output disabled,
//   Fs = I2S_CLK / (32 * (2 * I2SDIV + ODD))
// so I2SDIV=35, ODD=0 gives 32767.86 Hz, 4 ppm slow.  This is the closest
// any PLLI2S setting gets to 32768 Hz; the error is inaudible.
constexpr uint32_t I2S_DIV = 35;
constexpr uint32_t I2S_ODD = 0;
constexpr uint32_t I2S_FS_X100 =
    static_cast<uint32_t>(100ULL * I2S_CLK_HZ / (32 * (2 * I2S_DIV + I2S_ODD)));
static_assert(I2S_FS_X100 >= 100 * (MOZZI_AUDIO_RATE - 1) &&
                  I2S_FS_X100 <= 100 * (MOZZI_AUDIO_RATE + 1),
              "I2S sample rate does not match MOZZI_AUDIO_RATE");

// The DMA streams the ring of RING_FRAMES L/R frames forever, from the
// moment the I2S starts.  Mozzi fills it one sample at a time, up to the
// frame before the one the DMA is reading.  512 frames are 15.6 ms: room for
// updateControl(), which runs every 128 samples, and for USB serial output.
constexpr uint32_t RING_FRAMES = 512;
constexpr uint32_t RING_WORDS = RING_FRAMES * 2;
static_assert(RING_WORDS <= 0xFFFF, "DMA NDTR is 16 bits");

// Starts out silent: the MAX98357A powers up when it detects BCLK and needs a
// few milliseconds to settle before the first samples reach it.
int16_t dmaRing[RING_WORDS];
uint32_t writeFrame = 1;

// Lowest number of frames queued ahead of the DMA since the last report.  If
// it reaches 0, the DMA has caught up with Mozzi and the output glitches.
uint32_t minQueuedFrames = RING_FRAMES;

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
  // interrupts: canBufferAudioOutput() polls the transfer counter instead.
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

// Index of the ring frame the DMA is reading.  NDTR counts the words left
// before the pointer wraps back to the start of the ring.
uint32_t frameBeingPlayed() {
  const uint32_t wordsDone = RING_WORDS - TX_DMA->NDTR;
  return (wordsDone / 2) % RING_FRAMES;
}

}  // namespace

// ---------------------------------------------------------------------------
// Mozzi external output hooks (MOZZI_OUTPUT_EXTERNAL_CUSTOM)

// audioHook() asks this before computing each sample.  There is room while
// writing the next frame would not reach the frame the DMA is playing.
bool canBufferAudioOutput() {
  return (writeFrame + 1) % RING_FRAMES != frameBeingPlayed();
}

void audioOutput(const AudioOutput f) {
  const uint32_t queued =
      (writeFrame + RING_FRAMES - frameBeingPlayed()) % RING_FRAMES;
  if (queued < minQueuedFrames) {
    minQueuedFrames = queued;
  }

  // The MAX98357A plays (L+R)/2 by default; the same sample in both slots
  // makes the output independent of its SD_MODE setting.
  const int16_t sample = f.l();
  dmaRing[writeFrame * 2] = sample;
  dmaRing[writeFrame * 2 + 1] = sample;
  writeFrame = (writeFrame + 1) % RING_FRAMES;
}

namespace {

// ---------------------------------------------------------------------------
// 2-operator FM voice
//
// The modulator's output shifts the phase of the carrier:
//   out = amp(t) * cos(2 pi fc t + I(t) * cos(2 pi fm t))
// where fc and fm are the note frequency times each operator's ratio, and
// the modulation index I(t), in radians, sets how much energy spreads from
// the carrier into sidebands at fc +/- k * fm.  With integer ratios the
// sidebands land on harmonics and the tone stays pitched; other ratios give
// inharmonic, bell-like spectra.  An ADSR on I(t) makes the brightness evolve
// during the note, which is what brings FM tones to life.
//
// The 8192-entry table keeps the phase quantization noise of phMod() low;
// the samples themselves are 8 bits, as in all of Mozzi's wavetables.

using Osc = Oscil<COS8192_NUM_CELLS, MOZZI_AUDIO_RATE>;

// Amplitude envelopes are interpolated at audio rate (smooth attacks); index
// envelopes at control rate, which is plenty for timbre changes.
using AmpEnvelope = ADSR<MOZZI_CONTROL_RATE, MOZZI_AUDIO_RATE>;
using IndexEnvelope = ADSR<MOZZI_CONTROL_RATE, MOZZI_CONTROL_RATE>;

// An envelope rises to full level (255) in attackMs, falls to sustainLevel
// in decayMs, holds it until note-off, then fades out in releaseMs.
struct Envelope {
  uint16_t attackMs;
  uint16_t decayMs;
  uint8_t sustainLevel;
  uint16_t releaseMs;
};

struct FmPatch {
  float carrierRatio;
  float modulatorRatio;
  float index;  // peak modulation index I, in radians
  Envelope amp;
  Envelope indexEnv;
};

// Longer than any note: the envelopes leave the sustain phase on note-off.
constexpr unsigned int HOLD_MS = 60000;

template <typename Adsr>
void applyEnvelope(Adsr &adsr, const Envelope &e) {
  adsr.setADLevels(255, e.sustainLevel);
  adsr.setTimes(e.attackMs, e.decayMs, HOLD_MS, e.releaseMs);
}

// phMod() takes the phase offset as a Q15n16 fraction of a whole cycle, and
// the modulator's samples span +/-128.  So an index of I radians at full
// envelope level (255) is a scale factor of I / (2 pi) * 65536 / 128 per
// modulator step, applied in two parts: the envelope level times
// indexScale, shifted right by 8 (about / 255).  phMod() multiplies the
// offset by the table size in 32-bit arithmetic, which bounds the index to
// about 25 radians with this 8192-entry table; the patches stay at 10 or less.
constexpr float INDEX_SCALE_PER_RADIAN = 65536.0f / 128.0f / (2.0f * PI);

struct Voice {
  Osc carrier{COS8192_DATA};
  Osc modulator{COS8192_DATA};
  AmpEnvelope amp;
  IndexEnvelope index;
  int32_t indexScale = 0;  // per-patch, see INDEX_SCALE_PER_RADIAN
  int32_t depth = 0;       // updated at control rate from the index envelope
  uint32_t startTick = 0;
  uint32_t offTick = 0;
  bool held = false;

  void setPatch(const FmPatch &p) {
    applyEnvelope(amp, p.amp);
    applyEnvelope(index, p.indexEnv);
    indexScale = static_cast<int32_t>(p.index * INDEX_SCALE_PER_RADIAN);
  }

  void noteOn(uint8_t note, const FmPatch &p, uint32_t now, uint32_t off) {
    const float hz = mtof(static_cast<float>(note));
    carrier.setFreq(hz * p.carrierRatio);
    modulator.setFreq(hz * p.modulatorRatio);
    amp.noteOn();         // glides from the current level if stolen: no click
    index.noteOn(true);   // the timbre always restarts from its initial level
    startTick = now;
    offTick = off;
    held = true;
  }

  void updateControl(uint32_t now) {
    if (held && static_cast<int32_t>(now - offTick) >= 0) {
      amp.noteOff();
      index.noteOff();
      held = false;
    }
    amp.update();
    index.update();
    depth = (index.next() * indexScale) >> 8;
  }

  // Returns the carrier sample scaled by the amplitude envelope, 16 bits.
  int32_t next() {
    const Q15n16 phaseOffset = modulator.next() * depth;
    return carrier.phMod(phaseOffset) * amp.next();
  }
};

constexpr uint8_t NUM_VOICES = 4;
Voice voices[NUM_VOICES];

// ---------------------------------------------------------------------------
// Demo sequence
//
// Each part sets one patch on every voice and plays a phrase with it.  A
// note goes to a voice whose envelope has finished, else to the one playing
// the longest.

constexpr uint32_t TICKS_PER_STEP = MOZZI_CONTROL_RATE / 4;  // 250 ms steps,
                                                             // 8ths at 120 BPM

struct Note {
  uint8_t step;   // when it starts, in steps from the start of the part
  uint8_t steps;  // how long it is held
  uint8_t note;   // MIDI note number
};

struct Part {
  const char *name;
  FmPatch patch;
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
// notes are held at once; the 4th voice lets release tails ring.
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

#define CHORD_PART(name, transpose, ...) \
  {name, __VA_ARGS__, transpose, CHORD_NOTES, countOf(CHORD_NOTES), 28}

//   ratios c:m, index I, amp {A, D, S, R}, index {A, D, S, R}
const Part PARTS[] = {
    // A pure sine that brightens into a brassy, sawtooth-like tone as I
    // rises from 0 to 5 over 4 s.
    {"FM index sweep (1:1, I 0 -> 5)",
     {1.0f, 1.0f, 5.0f, {10, 0, 255, 300}, {4000, 0, 255, 300}},
     0, SWEEP_NOTES, countOf(SWEEP_NOTES), 24},

    // Chowning's bell: inharmonic 1:1.4, I decaying from 10 faster than the
    // amplitude, so the strike is clangorous and the tail almost a sine.
    {"FM bell (1:1.4, I 10)",
     {1.0f, 1.4f, 10.0f, {2, 5000, 0, 500}, {0, 2500, 0, 500}},
     0, BELL_NOTES, countOf(BELL_NOTES), 32},

    // A sine with a short, bright 14th-harmonic "tine" at the attack, the
    // idea behind the DX7's famous electric piano.
    CHORD_PART("FM e-piano tine (1:14, I 1.5)", 0,
               {1.0f, 14.0f, 1.5f, {2, 2500, 0, 400}, {0, 200, 0, 100}}),

    // Chowning's brass: the index follows the amplitude, so louder is also
    // brighter, the way a real brass instrument behaves.
    CHORD_PART("FM brass (1:1, I 5)", 0,
               {1.0f, 1.0f, 5.0f, {60, 150, 200, 150}, {60, 150, 200, 150}}),

    // Carrier at 3x and modulator at 2x the note: sidebands fall on the odd
    // harmonics, the hollow sound of a clarinet.
    CHORD_PART("FM clarinet (3:2, I 2)", 0,
               {3.0f, 2.0f, 2.0f, {50, 100, 220, 120}, {50, 100, 160, 120}}),

    // A very short, bright index burst over a fast-decaying sine.
    CHORD_PART("FM marimba (1:4, I 3)", 0,
               {1.0f, 4.0f, 3.0f, {1, 700, 0, 200}, {0, 40, 0, 40}}),

    // A punchy bass: the index drops from 4 to a low sustain, like a pick.
    CHORD_PART("FM bass (1:1, I 4)", -12,
               {1.0f, 1.0f, 4.0f, {2, 800, 150, 150}, {0, 300, 60, 150}}),
};
constexpr size_t PART_COUNT = sizeof(PARTS) / sizeof(PARTS[0]);

size_t partIndex = 0;
uint32_t partStartTick = 0;
uint8_t nextNote = 0;
uint32_t controlTick = 0;

// updateControl() runs inside audioHook(), so leave printing to loop().
volatile bool partChanged = true;
volatile bool demoFinished = false;

// The demo plays once, then waits for the Black Pill's KEY button.  KEY
// shorts PA0 (USER_BTN) to ground, so it reads LOW while pressed.  A press
// only counts after the button has been seen released, so holding KEY as
// the demo ends does not restart it, and contact bounce cannot either:
// the button is not read again until the demo is over.  It is polled from
// updateControl(), 256 times a second.
bool waitingForKey = false;
bool keyWasUp = false;

bool keyPressed() {
  const bool down = digitalRead(USER_BTN) == LOW;
  const bool pressed = down && keyWasUp;
  keyWasUp = !down;
  return pressed;
}

void startPart(uint32_t now) {
  for (Voice &v : voices) {
    v.setPatch(PARTS[partIndex].patch);
  }
  partStartTick = now;
  nextNote = 0;
  partChanged = true;
}

Voice &allocateVoice() {
  Voice *oldest = &voices[0];
  for (Voice &v : voices) {
    if (!v.amp.playing()) {
      return v;
    }
    if (static_cast<int32_t>(v.startTick - oldest->startTick) < 0) {
      oldest = &v;
    }
  }
  return *oldest;
}

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
  const uint32_t elapsedSteps = (now - partStartTick) / TICKS_PER_STEP;

  while (nextNote < part.noteCount &&
         part.notes[nextNote].step <= elapsedSteps) {
    const Note &n = part.notes[nextNote++];
    const uint32_t off = partStartTick + (n.step + n.steps) * TICKS_PER_STEP;
    allocateVoice().noteOn(n.note + part.transpose, part.patch, now, off);
  }

  if (elapsedSteps >= part.lengthSteps) {
    if (partIndex + 1 < PART_COUNT) {
      ++partIndex;
      startPart(now);
    } else {
      waitingForKey = true;
      keyWasUp = false;
      demoFinished = true;
    }
  }
}

// ---------------------------------------------------------------------------
// CPU load, measured with the Cortex-M4 cycle counter

uint32_t busyCycles = 0;

void startCycleCounter() {
  CoreDebug->DEMCR |= CoreDebug_DEMCR_TRCENA_Msk;
  DWT->CYCCNT = 0;
  DWT->CTRL |= DWT_CTRL_CYCCNTENA_Msk;
}

void report() {
  static uint32_t lastCycles = DWT->CYCCNT;
  static uint32_t lastMs = 0;

  if (partChanged) {
    partChanged = false;
    Serial.print("Part: ");
    Serial.println(PARTS[partIndex].name);
  }
  if (demoFinished) {
    demoFinished = false;
    Serial.println("Demo finished; press KEY (PA0) to play it again.");
  }

  const uint32_t nowMs = millis();
  if (nowMs - lastMs < 2000) {
    return;
  }
  lastMs = nowMs;

  // Load = cycles spent computing samples / cycles elapsed.  The rest of the
  // time loop() spins waiting for room in the ring.
  const uint32_t nowCycles = DWT->CYCCNT;
  const float load = 100.0f * busyCycles / (nowCycles - lastCycles);
  lastCycles = nowCycles;
  busyCycles = 0;

  Serial.print("  CPU load: ");
  Serial.print(load, 1);
  Serial.print(" %, ring low-water mark: ");
  Serial.print(minQueuedFrames);
  Serial.print(" of ");
  Serial.print(RING_FRAMES);
  Serial.println(" frames");
  minQueuedFrames = RING_FRAMES;
}

}  // namespace

// ---------------------------------------------------------------------------
// Mozzi callbacks

void updateControl() {
  runDemo(controlTick);
  for (Voice &v : voices) {
    v.updateControl(controlTick);
  }
  ++controlTick;
}

AudioOutput updateAudio() {
  int32_t mix = 0;
  for (Voice &v : voices) {
    mix += v.next();
  }
  // Each voice is at most 16 bits, so 4 voices fit in 18 bits: the mix can
  // never clip, and one voice alone peaks at a quarter of full scale.
  return MonoOutput::fromNBit(18, mix);
}

void setup() {
  Serial.begin(115200);
  pinMode(USER_BTN, INPUT_PULLUP);
  startCycleCounter();
  startPart(0);
  startI2S();
  startMozzi();
}

void loop() {
  // audioHook() computes a sample (and, every 128 samples, runs
  // updateControl()) whenever the ring has room, else returns at once.
  const uint32_t t0 = DWT->CYCCNT;
  const uint32_t framesBefore = writeFrame;
  audioHook();
  if (writeFrame != framesBefore) {
    busyCycles += DWT->CYCCNT - t0;
  }

  report();
}
