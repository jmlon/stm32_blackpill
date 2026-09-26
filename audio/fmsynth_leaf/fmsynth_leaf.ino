// FM synthesizer demo for an STM32F411CEU6 (WeAct Black Pill), using LEAF,
// the Lightweight Embedded Audio Framework (https://github.com/spiricom/LEAF).
//
// The third of the FM demos, next to audio/fmsynth_ami (AMY, DX7 patches)
// and audio/fmsynth_mozzi (Mozzi, 8-bit fixed point).  LEAF computes in
// single-precision float, which the F411's FPU does in hardware, from
// interpolated wavetables, at 48 kHz.  It plays the same seven 2-operator
// patches, phrase and 4 voices as the Mozzi demo, and the same phase
// modulation; only the bell and the electric piano add a second, detuned
// carrier.  Silent voices are not computed at all.
//
// This sketch keeps the core's default -Os.  The time goes into LEAF's small
// per-sample functions (tCycle_tick, tADSRS_tick, ...), which the compiler
// cannot inline across files at any level, so -O2 measured no faster on the
// board.
// Each patch's name, then the CPU load and LEAF's memory-pool use, are
// printed on the USB serial port (/dev/ttyACM0).
//
// LEAF is not in the Arduino library index; install it once with
//   tools/install-leaf.sh
//
// Connections, the same as modules/max98357a (SPI2/I2S2, alternate function 5):
//   MAX98357A BCLK    -> PB13 (I2S2_CK)
//   MAX98357A LRC     -> PB12 (I2S2_WS)
//   MAX98357A DIN     -> PB15 (I2S2_SD)
//   MAX98357A GND     -> Black Pill GND
//   MAX98357A VIN     -> 5 V (or 3.3 V; 5 V gives more output power)
//   MAX98357A GAIN/SD -> leave open for 9 dB gain, (L+R)/2 mono

#include <Arduino.h>
#include <leaf.h>
#include <stdlib.h>
#include "stm32f4xx.h"

namespace {

// ---------------------------------------------------------------------------
// I2S2 + DMA output

// The Black Pill runs from a 25 MHz HSE.  PLLI2S: 25 MHz / M=25 = 1 MHz VCO
// input, x N=192 = 192 MHz VCO, / R=5 = 38.4 MHz I2S clock.
constexpr uint32_t PLLI2S_M = 25;
constexpr uint32_t PLLI2S_N = 192;
constexpr uint32_t PLLI2S_R = 5;
constexpr uint32_t I2S_CLK_HZ = 25000000 / PLLI2S_M * PLLI2S_N / PLLI2S_R;

// With 16-bit channels and MCLK output disabled,
//   Fs = I2S_CLK / (32 * (2 * I2SDIV + ODD))
// so I2SDIV=12, ODD=1 gives exactly 48 kHz.
constexpr uint32_t I2S_DIV = 12;
constexpr uint32_t I2S_ODD = 1;
constexpr uint32_t SAMPLE_RATE = I2S_CLK_HZ / (32 * (2 * I2S_DIV + I2S_ODD));
static_assert(SAMPLE_RATE * 32 * (2 * I2S_DIV + I2S_ODD) == I2S_CLK_HZ,
              "sample rate is not an exact division of the I2S clock");

// The DMA streams a ring of RING_BLOCKS blocks of BLOCK_FRAMES L/R frames,
// forever, from the moment the I2S starts.  loop() renders a block as soon
// as the DMA has moved past it, so rendering runs up to RING_BLOCKS - 1
// blocks (8 ms) ahead of playback.  The sequencer runs once per block.
constexpr uint32_t BLOCK_FRAMES = 128;  // 2.7 ms
constexpr uint32_t RING_BLOCKS = 4;
constexpr uint32_t BLOCK_WORDS = BLOCK_FRAMES * 2;
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
  // interrupts: loop() polls the transfer counter instead.
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

// Index of the ring block the DMA is reading.  NDTR counts the words left
// before the pointer wraps back to the start of the ring.
uint32_t blockBeingPlayed() {
  const uint32_t wordsDone = RING_WORDS - TX_DMA->NDTR;
  return (wordsDone / BLOCK_WORDS) % RING_BLOCKS;
}

// ---------------------------------------------------------------------------
// LEAF

LEAF leaf;

// Every LEAF object lives in this pool, allocated at init (LEAF avoids
// malloc).  The demo's objects take about 3.5 KiB; the load report shows it.
char leafMemory[6144];

Lfloat randomNumber() { return static_cast<Lfloat>(rand()) / RAND_MAX; }

// ---------------------------------------------------------------------------
// 2-operator FM voice
//
// The modulator's output shifts the phase of the carrier, as in Mozzi's
// Oscil::phMod() and the DX7:
//   out = amp(t) * sin(2 pi fc t + I(t) * sin(2 pi fm t))
// where fc and fm are the note frequency times each operator's ratio, and
// the modulation index I(t), in radians, sets how much energy spreads from
// the carrier into sidebands at fc +/- k * fm.  With integer ratios the
// sidebands land on harmonics and the tone stays pitched; other ratios give
// inharmonic, bell-like spectra.  An envelope on I makes the brightness
// evolve during the note, which is what brings FM tones to life.
//
// tCycle is a 2048-entry float sine table with linear interpolation.
//
// detuneCents adds a second carrier, modulated the same way but tuned a few
// cents off, mixed in at DETUNE_MIX.  Both carry the same sidebands, so the
// whole note beats: a slow shimmer that suits the electric piano and bells,
// but makes sustained tones pulse.  At half level the beat dips to a third,
// not to silence.
//
// tADSRS envelopes follow exponential, analog-style curves (times in ms,
// levels 0..1).

struct Envelope {
  float attackMs;
  float decayMs;
  float sustain;
  float releaseMs;
};

struct FmPatch {
  float carrierRatio;
  float modulatorRatio;
  float index;        // peak modulation index I, in radians
  float detuneCents;  // second carrier's offset; 0 = single carrier
  Envelope amp;
  Envelope indexEnv;
};

constexpr float DETUNE_MIX = 0.5f;

// tCycle has no phase-modulation input, but its phase accumulator is a
// public field (a uint32_t, one cycle = 2^32): shifting it around one tick
// reads the sine at phase + offset.  The offset goes through Q8.24 so that
// both conversions are single FPU instructions; |cycles| < 128 fits, far
// beyond the patches' 10 / (2 pi) = 1.6 cycles.
float tickWithPhaseOffset(tCycle osc, float cycles) {
  const uint32_t offset =
      static_cast<uint32_t>(static_cast<int32_t>(cycles * 16777216.0f)) << 8;
  osc->phase += offset;
  const float out = tCycle_tick(osc);
  osc->phase -= offset;
  return out;
}

void applyEnvelope(tADSRS env, const Envelope &e) {
  tADSRS_setAttack(env, e.attackMs);
  tADSRS_setDecay(env, e.decayMs);
  tADSRS_setSustain(env, e.sustain);
  tADSRS_setRelease(env, e.releaseMs);
}

struct Voice {
  tCycle carrier;
  tCycle carrier2;
  tCycle modulator;
  tADSRS amp;
  tADSRS index;
  float indexCycles = 0;  // I / (2 pi): phase offset at full index envelope
  bool detuned = false;
  bool releasing = false;

  void init() {
    tCycle_init(&carrier, &leaf);
    tCycle_init(&carrier2, &leaf);
    tCycle_init(&modulator, &leaf);
    tADSRS_init(&amp, 1, 1, 0, 1, &leaf);
    tADSRS_init(&index, 1, 1, 0, 1, &leaf);
  }

  void setPatch(const FmPatch &p) {
    applyEnvelope(amp, p.amp);
    applyEnvelope(index, p.indexEnv);
    indexCycles = p.index / (2.0f * PI);
    detuned = p.detuneCents != 0;
  }

  void noteOn(uint8_t note, const FmPatch &p) {
    const float hz = LEAF_midiToFrequency(note);
    tCycle_setFreq(carrier, hz * p.carrierRatio);
    tCycle_setFreq(carrier2, hz * p.carrierRatio * exp2f(p.detuneCents / 1200.0f));
    tCycle_setFreq(modulator, hz * p.modulatorRatio);
    // Both envelopes rise from their current level, so a stolen voice
    // glides into the new note instead of clicking.
    tADSRS_on(amp, 1.0f);
    tADSRS_on(index, 1.0f);
    releasing = false;
  }

  void noteOff() {
    tADSRS_off(amp);
    tADSRS_off(index);
    releasing = true;
  }

  bool silent() const { return amp->state == env_idle; }

  float tick() {
    const float phaseMod =
        indexCycles * tCycle_tick(modulator) * tADSRS_tick(index);
    float out = tickWithPhaseOffset(carrier, phaseMod);
    if (detuned) {
      out = (out + DETUNE_MIX * tickWithPhaseOffset(carrier2, phaseMod)) /
            (1.0f + DETUNE_MIX);
    }
    return out * tADSRS_tick(amp);
  }
};

constexpr uint8_t NUM_VOICES = 4;
Voice voices[NUM_VOICES];

// LEAF's voice allocator: notes go to a free voice, then to one that is
// releasing, then steal.  Voices stay reserved while their release rings.
tSimplePoly poly;

// ---------------------------------------------------------------------------
// Demo sequence
//
// Each part sets one patch on every voice and plays a phrase with it.

constexpr uint32_t SAMPLES_PER_STEP = SAMPLE_RATE / 4;  // 250 ms steps,
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

// The patches of audio/fmsynth_mozzi (sustain as 0..1), plus detune.
//   ratios c:m, index I, detune (cents), amp {A, D, S, R}, index {A, D, S, R}
const Part PARTS[] = {
    // A pure sine that brightens into a brassy, sawtooth-like tone as I
    // rises from 0 to 5 over 4 s.
    {"FM index sweep (1:1, I 0 -> 5)",
     {1.0f, 1.0f, 5.0f, 0, {10, 1, 1.0f, 300}, {4000, 1, 1.0f, 300}},
     0, SWEEP_NOTES, countOf(SWEEP_NOTES), 24},

    // Chowning's bell: inharmonic 1:1.4, I decaying from 10 faster than the
    // amplitude, so the strike is clangorous and the tail almost a sine.
    // The 3-cent detune makes the tail beat slowly, like a real bell.
    {"FM bell (1:1.4, I 10)",
     {1.0f, 1.4f, 10.0f, 3, {2, 5000, 0, 500}, {1, 2500, 0, 500}},
     0, BELL_NOTES, countOf(BELL_NOTES), 32},

    // A sine with a short, bright 14th-harmonic "tine" at the attack, the
    // idea behind the DX7's famous electric piano, and like it a detuned
    // pair of carriers.
    CHORD_PART("FM e-piano tine (1:14, I 1.5)", 0,
               {1.0f, 14.0f, 1.5f, 7, {2, 2500, 0, 400}, {1, 200, 0, 100}}),

    // Chowning's brass: the index follows the amplitude, so louder is also
    // brighter, the way a real brass instrument behaves.
    CHORD_PART("FM brass (1:1, I 5)", 0,
               {1.0f, 1.0f, 5.0f, 0, {60, 150, 0.78f, 150},
                {60, 150, 0.78f, 150}}),

    // Carrier at 3x and modulator at 2x the note: sidebands fall on the odd
    // harmonics, the hollow sound of a clarinet.
    CHORD_PART("FM clarinet (3:2, I 2)", 0,
               {3.0f, 2.0f, 2.0f, 0, {50, 100, 0.86f, 120},
                {50, 100, 0.63f, 120}}),

    // A very short, bright index burst over a fast-decaying sine.
    CHORD_PART("FM marimba (1:4, I 3)", 0,
               {1.0f, 4.0f, 3.0f, 0, {1, 700, 0, 200}, {1, 40, 0, 40}}),

    // A punchy bass: the index drops from 4 to a low sustain, like a pick.
    CHORD_PART("FM bass (1:1, I 4)", -12,
               {1.0f, 1.0f, 4.0f, 0, {2, 800, 0.59f, 150},
                {1, 300, 0.24f, 150}}),
};
constexpr size_t PART_COUNT = sizeof(PARTS) / sizeof(PARTS[0]);

size_t partIndex = 0;
uint32_t partStartSample = 0;
uint8_t nextNote = 0;
uint32_t sampleClock = 0;  // frames rendered so far
bool partChanged = true;

// The demo plays once, then waits for the Black Pill's KEY button.  KEY
// shorts PA0 (USER_BTN) to ground, so it reads LOW while pressed.  A press
// only counts after the button has been seen released, so holding KEY as
// the demo ends does not restart it, and contact bounce cannot either:
// the button is not read again until the demo is over.  It is polled once
// per block, 375 times a second.
bool waitingForKey = false;
bool keyWasUp = false;
bool demoFinished = false;

bool keyPressed() {
  const bool down = digitalRead(USER_BTN) == LOW;
  const bool pressed = down && keyWasUp;
  keyWasUp = !down;
  return pressed;
}

// Note-offs waiting for their time, by MIDI note.
struct PendingOff {
  uint8_t note;
  uint32_t atSample;
};
PendingOff pendingOffs[16];
uint8_t pendingOffCount = 0;

void startPart() {
  for (Voice &v : voices) {
    v.setPatch(PARTS[partIndex].patch);
  }
  partStartSample = sampleClock;
  nextNote = 0;
  partChanged = true;
}

void playNote(uint8_t note, uint32_t offAt) {
  const int v = tSimplePoly_noteOn(poly, note, 100);
  if (v < 0) {
    return;  // already sounding
  }
  voices[v].noteOn(note, PARTS[partIndex].patch);
  if (pendingOffCount < sizeof(pendingOffs) / sizeof(pendingOffs[0])) {
    pendingOffs[pendingOffCount++] = {note, offAt};
  }
}

void releaseNote(uint8_t note) {
  // The voice stays reserved until its release has finished (see
  // runDemo()); a note that was stolen meanwhile has no voice left.
  const int v = tSimplePoly_markPendingNoteOff(poly, note);
  if (v >= 0) {
    voices[v].noteOff();
  }
}

// Called once per block, before rendering it.
void runDemo() {
  for (uint8_t i = 0; i < pendingOffCount;) {
    if (static_cast<int32_t>(sampleClock - pendingOffs[i].atSample) >= 0) {
      releaseNote(pendingOffs[i].note);
      pendingOffs[i] = pendingOffs[--pendingOffCount];
    } else {
      ++i;
    }
  }

  for (uint8_t v = 0; v < NUM_VOICES; ++v) {
    if (voices[v].releasing && voices[v].silent()) {
      voices[v].releasing = false;
      tSimplePoly_deactivateVoice(poly, v);
    }
  }

  // The note-offs and voice releases above keep running while waiting, so
  // the last notes ring out.
  if (waitingForKey) {
    if (keyPressed()) {
      waitingForKey = false;
      partIndex = 0;
      startPart();
    }
    return;
  }

  const Part &part = PARTS[partIndex];
  const uint32_t elapsedSteps = (sampleClock - partStartSample) / SAMPLES_PER_STEP;

  while (nextNote < part.noteCount &&
         part.notes[nextNote].step <= elapsedSteps) {
    const Note &n = part.notes[nextNote++];
    playNote(n.note + part.transpose,
             partStartSample + (n.step + n.steps) * SAMPLES_PER_STEP);
  }

  if (elapsedSteps >= part.lengthSteps) {
    if (partIndex + 1 < PART_COUNT) {
      ++partIndex;
      startPart();
    } else {
      waitingForKey = true;
      keyWasUp = false;
      demoFinished = true;
    }
  }
}

void renderBlock(int16_t *out) {
  runDemo();

  // Only voices that are sounding are computed.  A voice started by
  // runDemo() above is already in its attack; one that finishes its
  // release during the block just contributes zeros until the next one.
  Voice *active[NUM_VOICES];
  uint8_t activeCount = 0;
  for (Voice &v : voices) {
    if (!v.silent()) {
      active[activeCount++] = &v;
    }
  }

  // Each voice is within +/-1, so scaling by 1 / NUM_VOICES can never clip;
  // one voice alone peaks at a quarter of full scale, like the Mozzi demo.
  constexpr float SCALE = 32767.0f / NUM_VOICES;
  for (uint32_t i = 0; i < BLOCK_FRAMES; ++i) {
    float mix = 0;
    for (uint8_t k = 0; k < activeCount; ++k) {
      mix += active[k]->tick();
    }
    // The MAX98357A plays (L+R)/2 by default; the same sample in both slots
    // makes the output independent of its SD_MODE setting.
    const int16_t sample = static_cast<int16_t>(mix * SCALE);
    out[2 * i] = sample;
    out[2 * i + 1] = sample;
  }
  sampleClock += BLOCK_FRAMES;
}

// ---------------------------------------------------------------------------
// CPU load, measured with the Cortex-M4 cycle counter

uint32_t busyCycles = 0;
uint32_t peakBlockCycles = 0;

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

  // Average load = cycles spent rendering / cycles elapsed; peak = the
  // slowest block against the time one block plays.
  const uint32_t nowCycles = DWT->CYCCNT;
  const float avg = 100.0f * busyCycles / (nowCycles - lastCycles);
  const float peak =
      100.0f * peakBlockCycles / (SystemCoreClock / SAMPLE_RATE * BLOCK_FRAMES);
  lastCycles = nowCycles;
  busyCycles = 0;
  peakBlockCycles = 0;

  Serial.print("  CPU load: avg ");
  Serial.print(avg, 1);
  Serial.print(" %, peak ");
  Serial.print(peak, 1);
  Serial.print(" %; LEAF pool: ");
  Serial.print(leaf_pool_get_used(&leaf));
  Serial.print(" of ");
  Serial.print(sizeof(leafMemory));
  Serial.println(" bytes");
}

}  // namespace

void setup() {
  Serial.begin(115200);
  pinMode(USER_BTN, INPUT_PULLUP);
  startCycleCounter();

  LEAF_init(&leaf, SAMPLE_RATE, leafMemory, sizeof(leafMemory), randomNumber);
  for (Voice &v : voices) {
    v.init();
  }
  tSimplePoly_init(&poly, NUM_VOICES, &leaf);
  // A stolen note is not resumed when a voice frees up: its envelopes would
  // not be retriggered, so it would come back silent anyway.
  poly->recover_stolen = 0;

  startPart();
  startI2S();
}

void loop() {
  // Render into the ring block after the last one written, once the DMA has
  // finished playing it.
  if (blockBeingPlayed() != nextBlock) {
    const uint32_t t0 = DWT->CYCCNT;
    renderBlock(&dmaRing[nextBlock * BLOCK_WORDS]);
    const uint32_t cycles = DWT->CYCCNT - t0;
    busyCycles += cycles;
    if (cycles > peakBlockCycles) {
      peakBlockCycles = cycles;
    }
    nextBlock = (nextBlock + 1) % RING_BLOCKS;
  }

  report();
}
