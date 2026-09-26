// MIDI song player for an STM32F411CEU6 (WeAct Black Pill), synthesized
// with LEAF (https://github.com/spiricom/LEAF) in the style of the
// audio/fmsynth_leaf demo: 2-operator FM voices plus a few simple drums.
//
// The song is not parsed on the board.  tools/midi2h.py turns a Standard
// MIDI File into song.h, a flat list of timed note events; to play another
// song, regenerate it:
//   python3 tools/midi2h.py path/to/song.mid
// then rebuild.  To hear a song before converting it, see tools/midiplay.py
// (its --board option renders it with this sketch's own synthesis).  The
// song plays once; press the KEY button (PA0) to play it again.  The title, then the CPU load, voice use and clipping, are printed
// on the USB serial port (/dev/ttyACM0).
//
// Instruments: each MIDI channel's General MIDI program picks one of a dozen
// 2-operator FM patches (piano, mallets, bells, organ, plucked, bass,
// strings, voices, brass, reeds, flutes, synth leads, steel drums).
// Channel 10 plays drums: kicks, snares, clap, hi-hats, toms and cymbals,
// each built from LEAF noise, sine, filter and envelope objects.  Up to
// MELODIC_VOICES notes sound at once; beyond that the oldest is stolen.
//
// It runs at 32 kHz, which keeps 8 FM voices and the drums within the
// F411's budget; this is a demo, not hi-fi.
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
#include "song.h"
#include "stm32f4xx.h"

namespace {

// ---------------------------------------------------------------------------
// I2S2 + DMA output

// The Black Pill runs from a 25 MHz HSE.  PLLI2S: 25 MHz / M=25 = 1 MHz VCO
// input, x N=128 = 128 MHz VCO, / R=5 = 25.6 MHz I2S clock.
constexpr uint32_t PLLI2S_M = 25;
constexpr uint32_t PLLI2S_N = 128;
constexpr uint32_t PLLI2S_R = 5;
constexpr uint32_t I2S_CLK_HZ = 25000000 / PLLI2S_M * PLLI2S_N / PLLI2S_R;

// With 16-bit channels and MCLK output disabled,
//   Fs = I2S_CLK / (32 * (2 * I2SDIV + ODD))
// so I2SDIV=12, ODD=1 gives exactly 32 kHz.
constexpr uint32_t I2S_DIV = 12;
constexpr uint32_t I2S_ODD = 1;
constexpr uint32_t SAMPLE_RATE = I2S_CLK_HZ / (32 * (2 * I2S_DIV + I2S_ODD));
static_assert(SAMPLE_RATE * 32 * (2 * I2S_DIV + I2S_ODD) == I2S_CLK_HZ,
              "sample rate is not an exact division of the I2S clock");
static_assert(SAMPLE_RATE % 1000 == 0, "song times are whole milliseconds");
constexpr uint32_t SAMPLES_PER_MS = SAMPLE_RATE / 1000;

// The DMA streams a ring of RING_BLOCKS blocks of BLOCK_FRAMES L/R frames,
// forever, from the moment the I2S starts.  loop() renders a block as soon
// as the DMA has moved past it, so rendering runs up to RING_BLOCKS - 1
// blocks (10 ms) ahead of playback.  Song events are applied once per block,
// so their timing is accurate to a block (2 ms).
constexpr uint32_t BLOCK_FRAMES = 64;
constexpr uint32_t RING_BLOCKS = 6;
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

// >>> portable synthesis
// From here to the "<<< portable synthesis" line, the code touches no
// hardware except digitalRead() for KEY: tools/midiplay.py --board compiles
// it on the PC, with LEAF, to preview a song exactly as this sketch plays it.
// It relies on SAMPLE_RATE, SAMPLES_PER_MS and BLOCK_FRAMES, defined above.

// ---------------------------------------------------------------------------
// LEAF

LEAF leaf;

// Every LEAF object lives in this pool, allocated at init (LEAF avoids
// malloc).  The objects take about 6 KiB; the load report shows it.
char leafMemory[10240];

// Noise for the drums: tNoise calls this every sample, so it must be cheap.
// xorshift32 (Marsaglia), scaled to 0..1.
Lfloat randomNumber() {
  static uint32_t state = 2463534242u;
  state ^= state << 13;
  state ^= state >> 17;
  state ^= state << 5;
  return static_cast<Lfloat>(state >> 8) * (1.0f / 16777216.0f);
}

// ---------------------------------------------------------------------------
// Filters (tBiQuad with RBJ Audio EQ Cookbook coefficients; LEAF's tSVF
// would link 32 KB of lookup tables)

void setBandPass(tBiQuad f, float hz, float q) {
  const float w0 = 2.0f * PI * hz / SAMPLE_RATE;
  const float cosW0 = cosf(w0);
  const float alpha = sinf(w0) / (2.0f * q);
  const float a0 = 1.0f + alpha;
  tBiQuad_setCoefficients(f, alpha / a0, 0.0f, -alpha / a0,
                          -2.0f * cosW0 / a0, (1.0f - alpha) / a0);
}

void setHighPass(tBiQuad f, float hz, float q) {
  const float w0 = 2.0f * PI * hz / SAMPLE_RATE;
  const float cosW0 = cosf(w0);
  const float alpha = sinf(w0) / (2.0f * q);
  const float a0 = 1.0f + alpha;
  const float b0 = (1.0f + cosW0) / (2.0f * a0);
  tBiQuad_setCoefficients(f, b0, -2.0f * b0, b0, -2.0f * cosW0 / a0,
                          (1.0f - alpha) / a0);
}

// ---------------------------------------------------------------------------
// 2-operator FM voice (as in audio/fmsynth_leaf)
//
// The modulator shifts the carrier's phase:
//   out = amp(t) * sin(2 pi fc t + I(t) * sin(2 pi fm t))
// with fc and fm the note frequency times each operator's ratio and I(t)
// the modulation index in radians.  Velocity scales both the loudness and
// the index, so harder notes are also brighter.  detuneCents adds a second
// carrier a few cents off, for the shimmer of bells and steel drums.

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

//                               c:m ratios   I   detune  amp {A, D, S, R}         index {A, D, S, R}
constexpr FmPatch EPIANO     = {1.0f, 14.0f,  1.5f, 7, {2, 2500, 0, 400},       {1, 200, 0, 100}};
constexpr FmPatch MALLET     = {1.0f,  4.0f,  3.0f, 0, {1, 700, 0, 200},        {1, 40, 0, 40}};
constexpr FmPatch BELL       = {1.0f,  1.4f,  8.0f, 3, {2, 3000, 0, 500},       {1, 1500, 0, 500}};
constexpr FmPatch ORGAN      = {1.0f,  1.0f,  1.2f, 0, {8, 50, 0.9f, 80},       {8, 50, 1.0f, 80}};
constexpr FmPatch PLUCK      = {1.0f,  3.0f,  2.5f, 0, {1, 1500, 0, 250},       {1, 300, 0.1f, 200}};
constexpr FmPatch BASS       = {1.0f,  1.0f,  4.0f, 0, {2, 800, 0.59f, 150},    {1, 300, 0.24f, 150}};
constexpr FmPatch STRINGS    = {1.0f,  1.0f,  1.5f, 0, {150, 300, 0.8f, 500},   {300, 500, 0.6f, 500}};
constexpr FmPatch VOICES     = {1.0f,  1.0f,  0.8f, 0, {120, 300, 0.85f, 400},  {150, 300, 0.7f, 400}};
constexpr FmPatch BRASS      = {1.0f,  1.0f,  5.0f, 0, {60, 150, 0.78f, 150},   {60, 150, 0.78f, 150}};
constexpr FmPatch REED       = {3.0f,  2.0f,  2.0f, 0, {50, 100, 0.86f, 120},   {50, 100, 0.63f, 120}};
constexpr FmPatch FLUTE      = {1.0f,  1.0f,  0.5f, 0, {60, 100, 0.9f, 120},    {80, 100, 0.6f, 120}};
constexpr FmPatch SYNTH_LEAD = {1.0f,  1.0f,  3.0f, 0, {5, 100, 0.9f, 150},     {5, 200, 0.8f, 150}};
// After the DX7's STEEL DRUM: a carrier modulated at 2x with a fast index
// decay, and two carriers a few cents apart.
constexpr FmPatch STEEL_DRUM = {1.0f,  2.0f,  2.0f, 3, {1, 2500, 0, 400},       {1, 400, 0.15f, 300}};

// General MIDI programs come in families of 8.
const FmPatch *patchForProgram(uint8_t program) {
  switch (program) {
    case 11: case 12: case 13: case 15:  // vibraphone, marimba, xylophone,
      return &MALLET;                    // dulcimer
    case 114:
      return &STEEL_DRUM;
    case 52: case 53: case 54: case 85: case 91:  // choir, voice, synth voice
      return &VOICES;
    default:
      break;
  }
  switch (program / 8) {
    case 0: return &EPIANO;      // pianos
    case 1: return &BELL;        // chromatic percussion
    case 2: return &ORGAN;       // organs, accordion, harmonica
    case 3: return &PLUCK;       // guitars
    case 4: return &BASS;        // basses
    case 5: return &STRINGS;     // strings, harp, timpani
    case 6: return &STRINGS;     // ensembles
    case 7: return &BRASS;       // brass
    case 8: return &REED;        // reeds
    case 9: return &FLUTE;       // pipes
    case 10: return &SYNTH_LEAD; // synth leads
    case 11: return &STRINGS;    // synth pads
    case 12: return &STRINGS;    // synth effects
    case 13: return &PLUCK;      // ethnic (sitar, banjo, koto, ...)
    case 14: return &MALLET;     // percussive
    default: return &EPIANO;     // sound effects
  }
}

// An envelope has ended once it is idle, or holds a sustain level of 0: a
// decaying patch (piano, mallets, drums) sits in its sustain stage until the
// note-off, which drums never get, but there is nothing left to hear.
bool envelopeDone(tADSRS env) {
  return env->state == env_idle ||
         (env->state == env_sustain && env->sustainLevel <= 0);
}

void applyEnvelope(tADSRS env, const Envelope &e) {
  tADSRS_setAttack(env, e.attackMs);
  tADSRS_setDecay(env, e.decayMs);
  tADSRS_setSustain(env, e.sustain);
  tADSRS_setRelease(env, e.releaseMs);
}

constexpr float DETUNE_MIX = 0.5f;

// tCycle has no phase-modulation input, but its phase accumulator is a
// public field (a uint32_t, one cycle = 2^32): shifting it around one tick
// reads the sine at phase + offset.  The offset goes through Q8.24 so that
// both conversions are single FPU instructions; |cycles| < 128 fits.
float tickWithPhaseOffset(tCycle osc, float cycles) {
  const uint32_t offset =
      static_cast<uint32_t>(static_cast<int32_t>(cycles * 16777216.0f)) << 8;
  osc->phase += offset;
  const float out = tCycle_tick(osc);
  osc->phase -= offset;
  return out;
}

struct Voice {
  tCycle carrier;
  tCycle carrier2;
  tCycle modulator;
  tADSRS amp;
  tADSRS index;
  const FmPatch *patch = nullptr;
  float indexCycles = 0;  // I / (2 pi): phase offset at full index envelope
  uint32_t startedAt = 0; // sample clock of the note-on, for stealing
  uint8_t channel = 0;
  uint8_t note = 0;
  bool held = false;      // between note-on and note-off

  void init() {
    tCycle_init(&carrier, &leaf);
    tCycle_init(&carrier2, &leaf);
    tCycle_init(&modulator, &leaf);
    tADSRS_init(&amp, 1, 1, 0, 1, &leaf);
    tADSRS_init(&index, 1, 1, 0, 1, &leaf);
  }

  void noteOn(uint8_t ch, uint8_t n, float velocity, const FmPatch *p,
              uint32_t now) {
    if (p != patch) {  // the envelope setters are relatively costly
      patch = p;
      applyEnvelope(amp, p->amp);
      applyEnvelope(index, p->indexEnv);
      indexCycles = p->index / (2.0f * PI);
    }
    const float hz = LEAF_midiToFrequency(n);
    tCycle_setFreq(carrier, hz * p->carrierRatio);
    tCycle_setFreq(carrier2, hz * p->carrierRatio * exp2f(p->detuneCents / 1200.0f));
    tCycle_setFreq(modulator, hz * p->modulatorRatio);
    // tADSRS squares its velocity argument: amplitude follows velocity
    // squared, the index velocity itself.  Both envelopes rise from their
    // current level, so a stolen voice glides into the new note.
    tADSRS_on(amp, velocity);
    tADSRS_on(index, sqrtf(velocity));
    channel = ch;
    note = n;
    held = true;
    startedAt = now;
  }

  void noteOff() {
    tADSRS_off(amp);
    tADSRS_off(index);
    held = false;
  }

  bool silent() const { return envelopeDone(amp); }

  float tick() {
    const float phaseMod =
        indexCycles * tCycle_tick(modulator) * tADSRS_tick(index);
    float out = tickWithPhaseOffset(carrier, phaseMod);
    if (patch->detuneCents != 0) {
      out = (out + DETUNE_MIX * tickWithPhaseOffset(carrier2, phaseMod)) /
            (1.0f + DETUNE_MIX);
    }
    return out * tADSRS_tick(amp);
  }
};

// The song needs at most 7 at once; the rest covers release tails.
constexpr uint8_t MELODIC_VOICES = 8;
Voice voices[MELODIC_VOICES];
uint32_t stolenVoices = 0;

// A note of a channel goes to the voice already playing that note (a
// retrigger), else to a silent voice, else to the voice that was released
// the longest ago, else to the oldest held note.
Voice &allocateVoice(uint8_t channel, uint8_t note) {
  Voice *released = nullptr;
  Voice *oldest = &voices[0];
  for (Voice &v : voices) {
    if (!v.silent() && v.channel == channel && v.note == note) {
      return v;
    }
  }
  for (Voice &v : voices) {
    if (v.silent()) {
      return v;
    }
    if (!v.held && (!released || static_cast<int32_t>(v.startedAt - released->startedAt) < 0)) {
      released = &v;
    }
    if (static_cast<int32_t>(v.startedAt - oldest->startedAt) < 0) {
      oldest = &v;
    }
  }
  if (released) {
    return *released;
  }
  ++stolenVoices;
  return *oldest;
}

// ---------------------------------------------------------------------------
// Drums (General MIDI channel 10)
//
// Every drum is the same small recipe with different settings: a sine whose
// pitch drops from toneHz + dropHz to toneHz (kicks and toms), plus white
// noise through a band-pass or high-pass filter (snares, clap, hi-hats,
// cymbals), under an exponential decay.  Drums ignore note-offs.  Each kind
// has its own voice, so a hit cuts the previous one of the same kind; the
// open hi-hat is cut by the closed one, as on a real kit.

enum class NoiseFilter : uint8_t { BandPass, HighPass };

struct DrumSound {
  float toneHz;
  float dropHz;      // extra pitch at the hit, decaying with dropMs
  float dropMs;
  float toneLevel;
  float noiseLevel;
  NoiseFilter filter;
  float filterHz;
  float filterQ;
  float decayMs;
};

//                              tone   drop  dropMs tone  noise  filter                  Hz     Q      decay
constexpr DrumSound KICK       = {48,   120,  40,    1.0f, 0.05f, NoiseFilter::HighPass,  3000, 0.7f,  350};
constexpr DrumSound SIDE_STICK = {420,  0,    1,     0.4f, 0.5f,  NoiseFilter::BandPass,  2500, 1.0f,  40};
constexpr DrumSound SNARE      = {185,  40,   20,    0.35f, 0.9f, NoiseFilter::BandPass,  3000, 0.6f,  180};
constexpr DrumSound CLAP       = {0,    0,    1,     0,    1.0f,  NoiseFilter::BandPass,  1200, 1.5f,  150};
constexpr DrumSound CLOSED_HAT = {0,    0,    1,     0,    0.8f,  NoiseFilter::HighPass,  7000, 0.7f,  45};
constexpr DrumSound OPEN_HAT   = {0,    0,    1,     0,    0.8f,  NoiseFilter::HighPass,  7000, 0.7f,  350};
constexpr DrumSound CRASH      = {0,    0,    1,     0,    0.7f,  NoiseFilter::HighPass,  5000, 0.7f,  1200};
constexpr DrumSound RIDE       = {0,    0,    1,     0,    0.5f,  NoiseFilter::HighPass,  6000, 1.0f,  800};

struct DrumVoice {
  tCycle tone;
  tNoise noise;
  tBiQuad filter;
  tADSRS amp;
  tADSRS pitch;
  const DrumSound *sound = nullptr;
  float toneHz = 0;

  void init() {
    tCycle_init(&tone, &leaf);
    tNoise_init(&noise, WhiteNoise, &leaf);
    tBiQuad_init(&filter, &leaf);
    tADSRS_init(&amp, 0.5f, 100, 0, 5, &leaf);
    tADSRS_init(&pitch, 0.1f, 40, 0, 5, &leaf);
  }

  // tonePitchHz overrides the sound's tone (toms are tuned per note).
  void hit(const DrumSound &s, float velocity, float tonePitchHz = 0) {
    if (&s != sound) {
      sound = &s;
      tADSRS_setDecay(amp, s.decayMs);
      tADSRS_setDecay(pitch, s.dropMs);
      if (s.filter == NoiseFilter::BandPass) {
        setBandPass(filter, s.filterHz, s.filterQ);
      } else {
        setHighPass(filter, s.filterHz, s.filterQ);
      }
    }
    toneHz = tonePitchHz > 0 ? tonePitchHz : s.toneHz;
    tCycle_setPhase(tone, 0);  // every kick starts the same way
    tADSRS_on(amp, velocity);
    tADSRS_on(pitch, 1.0f);
  }

  bool silent() const { return envelopeDone(amp); }

  float tick() {
    float out = 0;
    if (sound->toneLevel != 0) {
      tCycle_setFreq(tone, toneHz + sound->dropHz * tADSRS_tick(pitch));
      out = sound->toneLevel * tCycle_tick(tone);
    }
    if (sound->noiseLevel != 0) {
      out += sound->noiseLevel * tBiQuad_tick(filter, tNoise_tick(noise));
    }
    return out * tADSRS_tick(amp);
  }
};

enum DrumSlot : uint8_t { KICK_SLOT, SNARE_SLOT, CLAP_SLOT, HAT_SLOT, TOM_SLOT, CYMBAL_SLOT, DRUM_SLOTS };
DrumVoice drums[DRUM_SLOTS];

void playDrum(uint8_t note, float velocity) {
  switch (note) {
    case 35: case 36: drums[KICK_SLOT].hit(KICK, velocity); break;
    case 37: drums[SNARE_SLOT].hit(SIDE_STICK, velocity); break;
    case 38: case 40: drums[SNARE_SLOT].hit(SNARE, velocity); break;
    case 39: drums[CLAP_SLOT].hit(CLAP, velocity); break;
    case 42: case 44: drums[HAT_SLOT].hit(CLOSED_HAT, velocity); break;
    case 46: drums[HAT_SLOT].hit(OPEN_HAT, velocity); break;
    case 49: case 52: case 55: case 57: drums[CYMBAL_SLOT].hit(CRASH, velocity); break;
    case 51: case 59: drums[CYMBAL_SLOT].hit(RIDE, velocity); break;
    case 41: case 43: case 45: case 47: case 48: case 50:
      // Low floor tom (41) to high tom (50), about 70 to 200 Hz.
      drums[TOM_SLOT].hit(KICK, velocity, 70.0f * exp2f((note - 41) / 6.0f));
      break;
    default: break;  // tools/midi2h.py already dropped the others
  }
}

// ---------------------------------------------------------------------------
// Song player

constexpr uint8_t DRUM_CHANNEL = 9;

uint8_t channelProgram[16];
uint32_t eventIndex = 0;
uint32_t nextEventMs = 0;
uint32_t songStartSample = 0;
uint32_t sampleClock = 0;  // frames rendered so far
bool playing = false;

// The song plays once, then waits for the Black Pill's KEY button.  KEY
// shorts PA0 (USER_BTN) to ground, so it reads LOW while pressed.  A press
// only counts after the button has been seen released, so holding KEY as
// the song ends does not restart it, and contact bounce cannot either.
bool keyWasUp = false;
bool songStarted = false;
bool songFinished = false;

bool keyPressed() {
  const bool down = digitalRead(USER_BTN) == LOW;
  const bool pressed = down && keyWasUp;
  keyWasUp = !down;
  return pressed;
}

void startSong() {
  for (uint8_t &p : channelProgram) {
    p = 0;  // GM default: Acoustic Grand Piano
  }
  eventIndex = 0;
  nextEventMs = SONG_EVENT_COUNT ? SONG_EVENTS[0].deltaMs : 0;
  songStartSample = sampleClock;
  stolenVoices = 0;
  playing = true;
  songStarted = true;
}

void noteOff(uint8_t channel, uint8_t note) {
  for (Voice &v : voices) {
    if (v.held && v.channel == channel && v.note == note) {
      v.noteOff();
    }
  }
}

void applyEvent(const SongEvent &e) {
  if (e.channel == SONG_NOP) {
    return;
  }
  if (e.channel & SONG_PROGRAM) {
    channelProgram[e.channel & 0x0F] = e.note;
    return;
  }
  const float velocity = e.velocity / 127.0f;
  if (e.channel == DRUM_CHANNEL) {
    if (e.velocity) {
      playDrum(e.note, velocity);
    }
  } else if (e.velocity) {
    allocateVoice(e.channel, e.note)
        .noteOn(e.channel, e.note, velocity,
                patchForProgram(channelProgram[e.channel]), sampleClock);
  } else {
    noteOff(e.channel, e.note);
  }
}

bool anythingSounding() {
  for (const Voice &v : voices) {
    if (!v.silent()) return true;
  }
  for (const DrumVoice &d : drums) {
    if (!d.silent()) return true;
  }
  return false;
}

// Called once per block, before rendering it.
void runSong() {
  if (!playing) {
    if (keyPressed()) {
      startSong();
    }
    return;
  }
  const uint32_t nowMs = (sampleClock - songStartSample) / SAMPLES_PER_MS;
  while (eventIndex < SONG_EVENT_COUNT && nextEventMs <= nowMs) {
    applyEvent(SONG_EVENTS[eventIndex++]);
    if (eventIndex < SONG_EVENT_COUNT) {
      nextEventMs += SONG_EVENTS[eventIndex].deltaMs;
    }
  }
  if (eventIndex >= SONG_EVENT_COUNT && !anythingSounding()) {
    playing = false;
    keyWasUp = false;
    songFinished = true;
  }
}

// ---------------------------------------------------------------------------
// Rendering

// Output gain: a single full-velocity voice peaks at 45 % of full scale.
// The sample song then peaks near 75 %; denser songs can sum past full
// scale, and those samples are clipped and counted in the load report.
constexpr float OUTPUT_GAIN = 0.45f;
uint32_t clippedSamples = 0;
uint8_t peakVoicesInUse = 0;

void renderBlock(int16_t *out) {
  runSong();

  // Only sounding voices are computed.  One started by runSong() above is
  // already in its attack; one that ends during the block adds zeros.
  Voice *active[MELODIC_VOICES];
  uint8_t activeCount = 0;
  for (Voice &v : voices) {
    if (!v.silent()) active[activeCount++] = &v;
  }
  DrumVoice *activeDrums[DRUM_SLOTS];
  uint8_t drumCount = 0;
  for (DrumVoice &d : drums) {
    if (!d.silent()) activeDrums[drumCount++] = &d;
  }
  if (activeCount > peakVoicesInUse) peakVoicesInUse = activeCount;

  constexpr float SCALE = 32767.0f * OUTPUT_GAIN;
  for (uint32_t i = 0; i < BLOCK_FRAMES; ++i) {
    float mix = 0;
    for (uint8_t k = 0; k < activeCount; ++k) mix += active[k]->tick();
    for (uint8_t k = 0; k < drumCount; ++k) mix += activeDrums[k]->tick();
    float scaled = mix * SCALE;
    if (scaled > 32767.0f || scaled < -32767.0f) {
      scaled = scaled > 0 ? 32767.0f : -32767.0f;
      ++clippedSamples;
    }
    // The MAX98357A plays (L+R)/2 by default; the same sample in both slots
    // makes the output independent of its SD_MODE setting.
    const int16_t sample = static_cast<int16_t>(scaled);
    out[2 * i] = sample;
    out[2 * i + 1] = sample;
  }
  sampleClock += BLOCK_FRAMES;
}

// <<< portable synthesis

// ---------------------------------------------------------------------------
// Reporting, with the Cortex-M4 cycle counter for the CPU load

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

  if (songStarted) {
    songStarted = false;
    Serial.print("Playing: ");
    Serial.print(SONG_TITLE);
    Serial.print(" (");
    Serial.print(SONG_LENGTH_MS / 1000.0f, 1);
    Serial.println(" s)");
  }
  if (songFinished) {
    songFinished = false;
    Serial.print("Song finished, ");
    Serial.print(stolenVoices);
    Serial.println(" notes stolen; press KEY (PA0) to play it again.");
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
  Serial.print(" %; voices: ");
  Serial.print(peakVoicesInUse);
  Serial.print(" of ");
  Serial.print(MELODIC_VOICES);
  Serial.print("; clipped samples: ");
  Serial.print(clippedSamples);
  Serial.print("; LEAF pool: ");
  Serial.print(leaf_pool_get_used(&leaf));
  Serial.print(" of ");
  Serial.print(sizeof(leafMemory));
  Serial.println(" bytes");
  peakVoicesInUse = 0;
  clippedSamples = 0;
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
  for (DrumVoice &d : drums) {
    d.init();
  }

  startSong();
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
