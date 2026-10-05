/*
 * SSD1306 128x32 OLED driver over I2C4 (blocking HAL transfers).
 * Every transfer is one I2C transaction: [addr][control][payload...], where
 * control 0x00 = commands, 0x40 = display data.
 */
#include "oled.h"
#include "board_config.h"

#if FEATURE_OLED_I2C
#include <string.h>

/* I2C4 timing for the speed chosen by I2C_SPEED_KHZ in board_config.h. The kernel
 * clock is PCLK4 = 120 MHz. TIMINGR = PRESC<<28 | SCLDEL<<20 | SDADEL<<16 |
 * SCLH<<8 | SCLL; a phase lasts (field + 1) ticks of (PRESC + 1) / 120 MHz. On the
 * wire each phase is longer than programmed (see below), and each setting meets the
 * I2C minimum low time, high time and data setup time for its mode as measured on
 * the wire.
 *
 *   100 kHz  PRESC 11 (100 ns): low 5.4 us, high 4.3 us, setup 400 ns
 *   400 kHz  PRESC  2 ( 25 ns): low 1.4 us, high 0.75 us, setup 125 ns
 *                                (the first try, high 0.8 us, measured 390 kHz: the
 *                                 fixed overhead is ~0.36 us, not 0.3)
 *  1000 kHz  PRESC  0 (8.3 ns): low 0.40 us, high 0.225 us, setup  67 ns
 *
 * What is programmed is not what appears on the wire: the peripheral's input sync
 * and the rise time on the pull-up add about +0.12 us to the high phase and +0.26 us
 * to the low phase (measured at 400 kHz and again at 1000 kHz, same split). So the
 * wire sees high 0.35 us and low 0.66 us at 1000 kHz, still above the 0.26 / 0.5 us
 * minimums. The first 1000 kHz setting (programmed 0.33 / 0.52 us) measured only
 * 813 kHz because it ignored this overhead. */
#if I2C_SPEED_KHZ == 100
#define OLED_I2C_TIMING  0xB0322A35U
#define OLED_PIN_SPEED   GPIO_SPEED_FREQ_LOW
#define OLED_FM_PLUS     0
#elif I2C_SPEED_KHZ == 400
#define OLED_I2C_TIMING  0x20421D37U
#define OLED_PIN_SPEED   GPIO_SPEED_FREQ_MEDIUM
#define OLED_FM_PLUS     0
#else   /* 1000 */
#define OLED_I2C_TIMING  0x00711A2FU
#define OLED_PIN_SPEED   GPIO_SPEED_FREQ_HIGH
#define OLED_FM_PLUS     1
#endif

static I2C_HandleTypeDef hi2c4;
static uint8_t oled_ok;

/* 5x7 font, ASCII 0x20-0x5A (space to 'Z'), one byte per column, LSB on top */
static const uint8_t font5x7[][5] = {
  {0x00,0x00,0x00,0x00,0x00}, {0x00,0x00,0x5F,0x00,0x00}, {0x00,0x07,0x00,0x07,0x00},
  {0x14,0x7F,0x14,0x7F,0x14}, {0x24,0x2A,0x7F,0x2A,0x12}, {0x23,0x13,0x08,0x64,0x62},
  {0x36,0x49,0x55,0x22,0x50}, {0x00,0x05,0x03,0x00,0x00}, {0x00,0x1C,0x22,0x41,0x00},
  {0x00,0x41,0x22,0x1C,0x00}, {0x14,0x08,0x3E,0x08,0x14}, {0x08,0x08,0x3E,0x08,0x08},
  {0x00,0x50,0x30,0x00,0x00}, {0x08,0x08,0x08,0x08,0x08}, {0x00,0x60,0x60,0x00,0x00},
  {0x20,0x10,0x08,0x04,0x02}, {0x3E,0x51,0x49,0x45,0x3E}, {0x00,0x42,0x7F,0x40,0x00},
  {0x42,0x61,0x51,0x49,0x46}, {0x21,0x41,0x45,0x4B,0x31}, {0x18,0x14,0x12,0x7F,0x10},
  {0x27,0x45,0x45,0x45,0x39}, {0x3C,0x4A,0x49,0x49,0x30}, {0x01,0x71,0x09,0x05,0x03},
  {0x36,0x49,0x49,0x49,0x36}, {0x06,0x49,0x49,0x29,0x1E}, {0x00,0x36,0x36,0x00,0x00},
  {0x00,0x56,0x36,0x00,0x00}, {0x08,0x14,0x22,0x41,0x00}, {0x14,0x14,0x14,0x14,0x14},
  {0x00,0x41,0x22,0x14,0x08}, {0x02,0x01,0x51,0x09,0x06}, {0x32,0x49,0x79,0x41,0x3E},
  {0x7E,0x11,0x11,0x11,0x7E}, {0x7F,0x49,0x49,0x49,0x36}, {0x3E,0x41,0x41,0x41,0x22},
  {0x7F,0x41,0x41,0x22,0x1C}, {0x7F,0x49,0x49,0x49,0x41}, {0x7F,0x09,0x09,0x09,0x01},
  {0x3E,0x41,0x49,0x49,0x7A}, {0x7F,0x08,0x08,0x08,0x7F}, {0x00,0x41,0x7F,0x41,0x00},
  {0x20,0x40,0x41,0x3F,0x01}, {0x7F,0x08,0x14,0x22,0x41}, {0x7F,0x40,0x40,0x40,0x40},
  {0x7F,0x02,0x0C,0x02,0x7F}, {0x7F,0x04,0x08,0x10,0x7F}, {0x3E,0x41,0x41,0x41,0x3E},
  {0x7F,0x09,0x09,0x09,0x06}, {0x3E,0x41,0x51,0x21,0x5E}, {0x7F,0x09,0x19,0x29,0x46},
  {0x46,0x49,0x49,0x49,0x31}, {0x01,0x01,0x7F,0x01,0x01}, {0x3F,0x40,0x40,0x40,0x3F},
  {0x1F,0x20,0x40,0x20,0x1F}, {0x3F,0x40,0x38,0x40,0x3F}, {0x63,0x14,0x08,0x14,0x63},
  {0x07,0x08,0x70,0x08,0x07}, {0x61,0x51,0x49,0x45,0x43},
};

static HAL_StatusTypeDef oled_write(uint8_t control, const uint8_t *data, uint16_t len)
{
  return HAL_I2C_Mem_Write(&hi2c4, OLED_I2C_ADDR_7BIT << 1, control, I2C_MEMADD_SIZE_8BIT,
                           (uint8_t *)data, len, 20);
}

static void oled_cmds(const uint8_t *c, uint16_t len) { oled_write(0x00, c, len); }

static void oled_set_cursor(uint8_t x, uint8_t page)
{
  const uint8_t c[] = {0x21, x, OLED_WIDTH - 1, 0x22, page, OLED_PAGES - 1};
  oled_cmds(c, sizeof(c));
}

uint8_t Oled_Init(void)
{
  GPIO_InitTypeDef gpio = {0};

  __HAL_RCC_GPIOD_CLK_ENABLE();
  __HAL_RCC_I2C4_CLK_ENABLE();

  gpio.Pin = GPIO_PIN_12 | GPIO_PIN_13;           /* SCL, SDA */
  gpio.Mode = GPIO_MODE_AF_OD;
  gpio.Pull = GPIO_PULLUP;                        /* module has its own pull-ups too */
  gpio.Speed = OLED_PIN_SPEED;
  gpio.Alternate = GPIO_AF4_I2C4;
  HAL_GPIO_Init(GPIOD, &gpio);

  hi2c4.Instance = I2C4;
  hi2c4.Init.Timing = OLED_I2C_TIMING;
  hi2c4.Init.OwnAddress1 = 0;
  hi2c4.Init.AddressingMode = I2C_ADDRESSINGMODE_7BIT;
  hi2c4.Init.DualAddressMode = I2C_DUALADDRESS_DISABLE;
  hi2c4.Init.OwnAddress2 = 0;
  hi2c4.Init.GeneralCallMode = I2C_GENERALCALL_DISABLE;
  hi2c4.Init.NoStretchMode = I2C_NOSTRETCH_DISABLE;
  if (HAL_I2C_Init(&hi2c4) != HAL_OK) return 0;
  HAL_I2CEx_ConfigAnalogFilter(&hi2c4, I2C_ANALOGFILTER_ENABLE);
#if OLED_FM_PLUS
  HAL_I2CEx_EnableFastModePlus(I2C_FASTMODEPLUS_I2C4);   /* stronger SCL/SDA drive for 1 MHz */
#endif

  if (HAL_I2C_IsDeviceReady(&hi2c4, OLED_I2C_ADDR_7BIT << 1, 3, 20) != HAL_OK) return 0;

  static const uint8_t init[] = {
    0xAE,             /* display off */
    0xD5, 0x80,       /* clock divide / oscillator */
    0xA8, 0x1F,       /* multiplex ratio: 32 rows */
    0xD3, 0x00,       /* display offset */
    0x40,             /* start line 0 */
    0x8D, 0x14,       /* charge pump on */
    0x20, 0x00,       /* horizontal addressing mode */
    0xA1,             /* segment remap */
    0xC8,             /* COM scan direction remapped */
    0xDA, 0x02,       /* COM pins for 128x32 */
    0x81, 0x8F,       /* contrast */
    0xD9, 0xF1,       /* pre-charge */
    0xDB, 0x40,       /* VCOMH */
    0xA4,             /* follow RAM */
    0xA6,             /* normal (not inverted) */
    0xAF,             /* display on */
  };
  oled_cmds(init, sizeof(init));
  Oled_Clear();
  oled_ok = 1;
  return 1;
}

void Oled_Clear(void)
{
  static const uint8_t zeros[OLED_WIDTH] = {0};
  for (uint8_t p = 0; p < OLED_PAGES; p++) {
    oled_set_cursor(0, p);
    oled_write(0x40, zeros, sizeof(zeros));
  }
}

void Oled_Print(uint8_t x, uint8_t page, const char *s)
{
  uint8_t buf[OLED_WIDTH];
  uint16_t n = 0;

  if (page >= OLED_PAGES) return;
  for (; *s && (size_t)(n + 6) <= sizeof(buf) && x + n + 6 <= OLED_WIDTH; s++) {
    char c = *s;
    if (c >= 'a' && c <= 'z') c -= 32;
    if (c < ' ' || c > 'Z') c = '?';
    memcpy(&buf[n], font5x7[c - ' '], 5);
    buf[n + 5] = 0x00;                            /* 1-column gap */
    n += 6;
  }
  if (!n) return;

  const uint8_t c[] = {0x21, x, (uint8_t)(x + n - 1), 0x22, page, page};
  oled_cmds(c, sizeof(c));
  oled_write(0x40, buf, n);
}

/* ---- Non-blocking updates ------------------------------------------------
 * An update is two I2C writes: set the cursor (control 0x00 + 6 commands), then
 * the pixel data (control 0x40 + up to 36 bytes). After Oled_Init the I2C4
 * peripheral is fed from its own interrupt, one TXDR byte per TXIS event, so
 * the clock never stalls waiting for the main loop (which can be busy scanning
 * for the capture trigger) and the main loop is never held up either. */
#define ASYNC_MAX_BYTES  (1 + OLED_WIDTH)
#define I2C4_INT_MASK    (I2C_CR1_TXIE | I2C_CR1_STOPIE | I2C_CR1_NACKIE)

static uint8_t           tx_buf[2][ASYNC_MAX_BYTES];
static uint8_t           tx_len[2];
static volatile uint8_t  tx_stage;        /* 0 idle, 1 cursor write in flight, 2 data write in flight */
static volatile uint8_t  tx_idx;
static volatile uint32_t tx_t0;

static void i2c_start_write(uint8_t n)
{
  I2C4->ICR = I2C_ICR_STOPCF | I2C_ICR_NACKCF;
  I2C4->CR2 = ((uint32_t)(OLED_I2C_ADDR_7BIT << 1) & I2C_CR2_SADD) |
              ((uint32_t)n << I2C_CR2_NBYTES_Pos) | I2C_CR2_AUTOEND | I2C_CR2_START;
  tx_idx = 0;
}

uint8_t Oled_Busy(void) { return tx_stage != 0; }

uint8_t Oled_PrintAsync(uint8_t x, uint8_t page, const char *s)
{
  if (!oled_ok || tx_stage || page >= OLED_PAGES) return 0;

  uint8_t *d = &tx_buf[1][1];
  uint8_t n = 0;
  for (; *s && n + 6 <= OLED_WIDTH - 1 && x + n + 6 <= OLED_WIDTH; s++) {
    char c = *s;
    if (c >= 'a' && c <= 'z') c -= 32;
    if (c < ' ' || c > 'Z') c = '?';
    memcpy(&d[n], font5x7[c - ' '], 5);
    d[n + 5] = 0x00;
    n += 6;
  }
  if (!n) return 0;

  const uint8_t cur[] = {0x00, 0x21, x, (uint8_t)(x + n - 1), 0x22, page, page};
  memcpy(tx_buf[0], cur, sizeof(cur));
  tx_len[0] = sizeof(cur);
  tx_buf[1][0] = 0x40;
  tx_len[1] = (uint8_t)(n + 1);

  tx_t0 = HAL_GetTick();
  tx_stage = 1;
  HAL_NVIC_SetPriority(I2C4_EV_IRQn, 1, 0);       /* below the capture DMA, above the main loop */
  HAL_NVIC_EnableIRQ(I2C4_EV_IRQn);
  I2C4->CR1 |= I2C4_INT_MASK;
  i2c_start_write(tx_len[0]);
  return 1;
}

void I2C4_EV_IRQHandler(void)
{
  uint32_t isr = I2C4->ISR;
  const uint8_t i = (tx_stage ? tx_stage : 1) - 1;

  if (isr & I2C_ISR_NACKF) {                      /* display did not answer: drop this update */
    I2C4->ICR = I2C_ICR_NACKCF;
    tx_stage = 0;                                 /* STOPF follows (AUTOEND) and is cleaned up below */
  }
  if ((isr & I2C_ISR_TXIS) && tx_stage && tx_idx < tx_len[i])
    I2C4->TXDR = tx_buf[i][tx_idx++];

  if (isr & I2C_ISR_STOPF) {                      /* AUTOEND: transfer complete */
    I2C4->ICR = I2C_ICR_STOPCF;
    if (tx_stage == 1) {
      tx_stage = 2;
      i2c_start_write(tx_len[1]);
    } else {
      tx_stage = 0;
      I2C4->CR1 &= ~I2C4_INT_MASK;
    }
  }
}

/* Watchdog only; the transfer itself runs from the interrupt. */
void Oled_Task(void)
{
  if (tx_stage && HAL_GetTick() - tx_t0 > 30) {   /* stuck: reset the peripheral */
    I2C4->CR1 &= ~(I2C4_INT_MASK | I2C_CR1_PE);
    I2C4->CR1 |= I2C_CR1_PE;
    tx_stage = 0;
  }
}

#endif /* FEATURE_OLED_I2C */
