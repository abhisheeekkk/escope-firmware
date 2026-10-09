#ifndef TFT_H
#define TFT_H

#include "main.h"

/* ST7789V 240x320 TFT on SPI1 (4-wire SPI: SCK, MOSI, CS, DC, plus RES), 16-bit colour.
 * Pins (3.3 V):
 *   SCK  PA5   (SPI1_SCK,  AF5)       display pin SCL
 *   MOSI PA7   (SPI1_MOSI, AF5)       display pin SDA
 *   CS   PA4   (GPIO, chip select, active low, held low for a whole command + data)
 *   DC   PC4   (GPIO, low = command byte, high = data bytes)
 *   RES  PC5   (GPIO, reset, active low)
 * SPI mode 0 (idle low, sample on the rising edge), MSB first, write only: the display's
 * data output is not used. The clock rate is TFT_SPI_KHZ in board_config.h. */
#define TFT_WIDTH   240
#define TFT_HEIGHT  320

/* Initialises the display and draws a test screen. Returns 1 when the SPI peripheral
 * started (a display cannot be detected: it has no read-back on this interface). */
uint8_t  Tft_Init(void);
/* Call every main-loop pass: updates the counter on the screen every few hundred ms. */
void     Tft_Task(uint32_t now_ms);
/* The SPI clock actually generated, in kHz (kernel clock divided by the prescaler). */
uint32_t Tft_ActualKhz(void);

#endif
