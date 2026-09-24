// MAX98357A I2S demo for an STM32F411CEU6 (WeAct Black Pill).
//
// Unlike the Blue Pill's STM32F103C8, the F411 has real I2S hardware: every
// SPI block (SPI1..SPI5) has an I2S mode (RM0383 §20), clocked from the
// dedicated PLLI2S rather than from APB1.  SPI2 in I2S master-transmit mode
// generates BCLK (CK), LRCLK (WS) and the serial data (SD) by itself, so no
// timer or jumper wire is needed to make LRCLK.
//
// Connections (all SPI2/I2S2, alternate function 5):
//   MAX98357A BCLK    -> PB13 (I2S2_CK)
//   MAX98357A LRC     -> PB12 (I2S2_WS)
//   MAX98357A DIN     -> PB15 (I2S2_SD)
//   MAX98357A GND     -> Black Pill GND
//   MAX98357A VIN     -> 5 V (or 3.3 V; 5 V gives more output power)
//   MAX98357A GAIN/SD -> leave open for 9 dB gain, (L+R)/2 mono
//
// The MAX98357A is a digital I2S amplifier, not an SPI peripheral.  Do not
// share these SPI2 pins with other SPI devices.

#include <Arduino.h>
#include <math.h>
#include "stm32f4xx.h"

namespace {

// The Black Pill runs from a 25 MHz HSE.  PLLI2S: 25 MHz / M=25 = 1 MHz VCO
// input, x N=192 = 192 MHz VCO, / R=5 = 38.4 MHz I2S clock.
constexpr uint32_t PLLI2S_M = 25;
constexpr uint32_t PLLI2S_N = 192;
constexpr uint32_t PLLI2S_R = 5;
constexpr uint32_t I2S_CLK_HZ = 25000000 / PLLI2S_M * PLLI2S_N / PLLI2S_R;

// With 16-bit channels and MCLK output disabled,
//   Fs = I2S_CLK / (32 * (2 * I2SDIV + ODD))
// so I2SDIV=12, ODD=1 gives exactly 48 kHz (BCLK = 1.536 MHz, 32 BCLK per
// LRCLK period).  The MAX98357A accepts 8 kHz to 96 kHz.
constexpr uint32_t I2S_DIV = 12;
constexpr uint32_t I2S_ODD = 1;
constexpr uint32_t SAMPLE_RATE = I2S_CLK_HZ / (32 * (2 * I2S_DIV + I2S_ODD));
static_assert(SAMPLE_RATE * 32 * (2 * I2S_DIV + I2S_ODD) == I2S_CLK_HZ,
              "sample rate is not an exact division of the I2S clock");

constexpr uint32_t TONE_HZ = 1000;
constexpr int16_t AMPLITUDE = 12000;

// The MAX98357A powers up when it detects BCLK and needs a few milliseconds
// to settle, so the clocks run with silent data before the tone and again
// after it.  The tone fades in and out so neither edge clicks, and only then
// are the clocks stopped.
constexpr float SILENCE_SECONDS = 0.05f;
constexpr float TONE_SECONDS = 2.0f;
constexpr float FADE_SECONDS = 0.02f;
constexpr uint32_t SILENCE_FRAMES =
    static_cast<uint32_t>(SILENCE_SECONDS * SAMPLE_RATE);
constexpr uint32_t TONE_FRAMES = static_cast<uint32_t>(TONE_SECONDS * SAMPLE_RATE);
constexpr uint32_t FADE_FRAMES = static_cast<uint32_t>(FADE_SECONDS * SAMPLE_RATE);
constexpr uint32_t TOTAL_FRAMES = SILENCE_FRAMES + TONE_FRAMES + SILENCE_FRAMES;

// The tone divides the sample rate exactly (48 samples per cycle), so the
// table holds one exact period and is replayed sample by sample.  Unlike a
// phase accumulator indexing a coarser table, this adds no phase-truncation
// distortion: the only error is the 16-bit rounding of each sample.
constexpr uint32_t SINE_TABLE_SIZE = SAMPLE_RATE / TONE_HZ;
static_assert(SINE_TABLE_SIZE * TONE_HZ == SAMPLE_RATE,
              "TONE_HZ must divide SAMPLE_RATE");
int16_t sineTable[SINE_TABLE_SIZE];

void buildSineTable() {
  for (uint32_t i = 0; i < SINE_TABLE_SIZE; ++i) {
    const float radians = 2.0f * PI * i / SINE_TABLE_SIZE;
    sineTable[i] = static_cast<int16_t>(lroundf(sinf(radians) * AMPLITUDE));
  }
}

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
  // speed, no pull.  PB14 is unused and stays a GPIO.  Keep the high speed:
  // with medium speed the tone was badly garbled on a breadboard, because a
  // slow edge lingers near the logic threshold where supply/ground noise from
  // the class-D amplifier can make the MAX98357A count a BCLK edge twice.
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
  // clock idle low (data changes on the falling edge and is sampled on the
  // rising edge).  MCLK output is not needed by the MAX98357A.
  SPI2->I2SCFGR = 0;
  SPI2->I2SPR = I2S_DIV | (I2S_ODD << SPI_I2SPR_ODD_Pos);
  SPI2->I2SCFGR = SPI_I2SCFGR_I2SMOD | SPI_I2SCFGR_I2SCFG_1;
}

void startI2S() {
  configureI2sClock();
  configurePins();
  configureI2s2();

  // The first word written after enabling goes out in the left slot.
  SPI2->I2SCFGR |= SPI_I2SCFGR_I2SE;
}

void writeStereoSample(int16_t sample) {
  // A MAX98357A selects one I2S channel (or the mix) using its SD_MODE pin.
  // Sending the same value in both slots makes the demo work regardless of
  // that setting and produces a mono tone from the amplifier.
  while ((SPI2->SR & SPI_SR_TXE) == 0) {
  }
  SPI2->DR = static_cast<uint16_t>(sample);

  while ((SPI2->SR & SPI_SR_TXE) == 0) {
  }
  SPI2->DR = static_cast<uint16_t>(sample);
}

void stopI2S() {
  // Let the last frame shift out completely before disabling I2S (RM0383
  // procedure for 16-bit data in 16-bit channels: wait for TXE, then for BSY
  // to clear).  With BCLK stopped the MAX98357A detects the missing clock and
  // mutes its output.
  while ((SPI2->SR & SPI_SR_TXE) == 0) {
  }
  while (SPI2->SR & SPI_SR_BSY) {
  }
  SPI2->I2SCFGR &= ~SPI_I2SCFGR_I2SE;

  // A disabled I2S block may stop driving its pins.  A floating BCLK wire
  // picks up noise that the amplifier takes for clock edges, heard as
  // scratching, so hand the pins back to GPIO and hold them low.
  for (uint32_t pin : I2S_PINS) {
    GPIOB->BSRR = 1U << (pin + 16);
    GPIOB->MODER = (GPIOB->MODER & ~(0x3U << (pin * 2))) | (0x1U << (pin * 2));
  }
}

// Sample for frame n of the whole sequence: silence, tone, silence.
int16_t sampleAt(uint32_t n) {
  if (n < SILENCE_FRAMES || n >= SILENCE_FRAMES + TONE_FRAMES) {
    return 0;
  }
  const uint32_t t = n - SILENCE_FRAMES;
  int32_t sample = sineTable[t % SINE_TABLE_SIZE];
  const uint32_t framesLeft = TONE_FRAMES - t;
  if (t < FADE_FRAMES) {
    sample = sample * static_cast<int32_t>(t) / FADE_FRAMES;
  } else if (framesLeft < FADE_FRAMES) {
    sample = sample * static_cast<int32_t>(framesLeft) / FADE_FRAMES;
  }
  return static_cast<int16_t>(sample);
}

}  // namespace

void setup() {
  buildSineTable();
  startI2S();
}

void loop() {
  static uint32_t framesSent = 0;

  if (framesSent >= TOTAL_FRAMES) {
    return;
  }

  // Stream a modest-sized block, then return to the Arduino loop.  The writes
  // are blocking; each 16-bit word takes about 1000 CPU cycles (96 MHz) to
  // shift out, which leaves ample time for the table lookup and the loop()
  // overhead.  A DMA ring buffer would free the CPU entirely, and the F411's
  // 128 KiB of SRAM has room for one, but polling keeps the demo simple.
  for (uint16_t i = 0; i < 256 && framesSent < TOTAL_FRAMES; ++i) {
    writeStereoSample(sampleAt(framesSent));
    ++framesSent;
  }

  if (framesSent >= TOTAL_FRAMES) {
    stopI2S();
  }
}
