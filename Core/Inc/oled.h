#ifndef OLED_H
#define OLED_H

#include "main.h"

/* SSD1306 128x32 (0.91") OLED on I2C4: SCL = PD12, SDA = PD13 (AF4), 100 kHz.
 * The module's 7-bit address is 0x3C (some boards use 0x3D). */
#define OLED_I2C_ADDR_7BIT  0x3C
#define OLED_WIDTH          128
#define OLED_PAGES          4      /* 32 rows / 8 */

/* Returns 1 if the display ACKed its address and was initialised, else 0. */
uint8_t Oled_Init(void);
void    Oled_Clear(void);
/* Draw text at pixel column x on page (0-3). Lower case is shown as upper. */
void    Oled_Print(uint8_t x, uint8_t page, const char *s);   /* blocking: boot-time only */

/* Non-blocking variant for the main loop: queues the text and returns 1, or
 * returns 0 if the previous update is still going out. Oled_Task() must be called
 * every main-loop pass; it feeds the I2C peripheral a byte at a time and never
 * waits, so the capture trigger scan keeps running during an update. */
uint8_t Oled_PrintAsync(uint8_t x, uint8_t page, const char *s);
void    Oled_Task(void);
uint8_t Oled_Busy(void);

#endif
