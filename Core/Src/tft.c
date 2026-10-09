/*
 * ST7789V 240x320 SPI TFT driver (write only) for the signal generator role.
 *
 * It exists to produce known SPI traffic for the logic analyser: commands (DC low)
 * followed by parameter or pixel bytes (DC high), at a clock rate chosen in
 * board_config.h. The SPI1 kernel clock is PLL1Q = 64 MHz, so the possible rates are
 * 64 MHz / 2^n: 32000, 16000, 8000, 4000, 2000, 1000, 500 and 250 kHz.
 */
#include "tft.h"
#include "board_config.h"

#if FEATURE_TFT_SPI
#include <stdio.h>
#include "font5x7.h"

#define TFT_CS_PORT   GPIOA
#define TFT_CS_PIN    GPIO_PIN_4
#define TFT_DC_PORT   GPIOC
#define TFT_DC_PIN    GPIO_PIN_4
#define TFT_RES_PORT  GPIOC
#define TFT_RES_PIN   GPIO_PIN_5

#if   TFT_SPI_KHZ == 250
#define TFT_PRESCALER SPI_BAUDRATEPRESCALER_256
#define TFT_DIV       256U
#elif TFT_SPI_KHZ == 500
#define TFT_PRESCALER SPI_BAUDRATEPRESCALER_128
#define TFT_DIV       128U
#elif TFT_SPI_KHZ == 1000
#define TFT_PRESCALER SPI_BAUDRATEPRESCALER_64
#define TFT_DIV       64U
#elif TFT_SPI_KHZ == 2000
#define TFT_PRESCALER SPI_BAUDRATEPRESCALER_32
#define TFT_DIV       32U
#elif TFT_SPI_KHZ == 4000
#define TFT_PRESCALER SPI_BAUDRATEPRESCALER_16
#define TFT_DIV       16U
#elif TFT_SPI_KHZ == 8000
#define TFT_PRESCALER SPI_BAUDRATEPRESCALER_8
#define TFT_DIV       8U
#elif TFT_SPI_KHZ == 16000
#define TFT_PRESCALER SPI_BAUDRATEPRESCALER_4
#define TFT_DIV       4U
#elif TFT_SPI_KHZ == 32000
#define TFT_PRESCALER SPI_BAUDRATEPRESCALER_2
#define TFT_DIV       2U
#else
#error "TFT_SPI_KHZ must be 250, 500, 1000, 2000, 4000, 8000, 16000 or 32000"
#endif

#define TFT_UPDATE_MS 250U

/* Output edge speed of SCK and MOSI: slow edges mean less ringing and crosstalk on jumper wires.
 * Use the slowest setting that still gives clean edges at the chosen clock rate. */
#if   TFT_SPI_KHZ <= 2000
#define TFT_PIN_SPEED GPIO_SPEED_FREQ_LOW
#elif TFT_SPI_KHZ <= 8000
#define TFT_PIN_SPEED GPIO_SPEED_FREQ_MEDIUM
#elif TFT_SPI_KHZ <= 16000
#define TFT_PIN_SPEED GPIO_SPEED_FREQ_HIGH
#else
#define TFT_PIN_SPEED GPIO_SPEED_FREQ_VERY_HIGH
#endif

/* RGB565 */
#define RGB565(r, g, b) ((uint16_t)((((r) & 0xF8) << 8) | (((g) & 0xFC) << 3) | ((b) >> 3)))
#define C_BLACK   0x0000
#define C_WHITE   0xFFFF
#define C_RED     RGB565(255, 0, 0)
#define C_GREEN   RGB565(0, 255, 0)
#define C_BLUE    RGB565(0, 0, 255)
#define C_YELLOW  RGB565(255, 255, 0)
#define C_CYAN    RGB565(0, 255, 255)

static SPI_HandleTypeDef hspi1;
static uint8_t  tft_ok;
static uint32_t next_due;
static uint32_t count;
static uint32_t shown = 0xFFFFFFFFU;

/* one character cell at scale 3 is 18 x 24 pixels = 864 bytes */
static uint8_t cell[2 * 18 * 24];
static uint8_t fill_buf[480];

void HAL_SPI_MspInit(SPI_HandleTypeDef *h)
{
  if (h->Instance != SPI1) return;
  GPIO_InitTypeDef g = {0};
  __HAL_RCC_SPI1_CLK_ENABLE();
  __HAL_RCC_GPIOA_CLK_ENABLE();
  g.Pin       = GPIO_PIN_5 | GPIO_PIN_7;              /* SCK, MOSI */
  g.Mode      = GPIO_MODE_AF_PP;
  g.Pull      = GPIO_NOPULL;
  g.Speed     = TFT_PIN_SPEED;
  g.Alternate = GPIO_AF5_SPI1;
  HAL_GPIO_Init(GPIOA, &g);
}

static void cs(uint8_t level) { HAL_GPIO_WritePin(TFT_CS_PORT, TFT_CS_PIN, level ? GPIO_PIN_SET : GPIO_PIN_RESET); }
static void dc(uint8_t level) { HAL_GPIO_WritePin(TFT_DC_PORT, TFT_DC_PIN, level ? GPIO_PIN_SET : GPIO_PIN_RESET); }

static void tx(const uint8_t *p, uint16_t n) { HAL_SPI_Transmit(&hspi1, (uint8_t *)p, n, 2000); }

/* one command with optional parameter bytes; CS stays low for the whole thing */
static void cmd(uint8_t c, const uint8_t *data, uint16_t n)
{
  cs(0);
  dc(0);
  tx(&c, 1);
  if (n) { dc(1); tx(data, n); }
  cs(1);
}

static void window(uint16_t x0, uint16_t y0, uint16_t x1, uint16_t y1)
{
  const uint8_t ca[4] = {(uint8_t)(x0 >> 8), (uint8_t)x0, (uint8_t)(x1 >> 8), (uint8_t)x1};
  const uint8_t ra[4] = {(uint8_t)(y0 >> 8), (uint8_t)y0, (uint8_t)(y1 >> 8), (uint8_t)y1};
  cmd(0x2A, ca, 4);                                /* CASET */
  cmd(0x2B, ra, 4);                                /* RASET */
}

/* RAMWR then pixel data in one chip-select window */
static void ram_begin(void) { const uint8_t c = 0x2C; cs(0); dc(0); tx(&c, 1); dc(1); }
static void ram_end(void)   { cs(1); }

static void fill_rect(uint16_t x, uint16_t y, uint16_t w, uint16_t h, uint16_t color)
{
  window(x, y, (uint16_t)(x + w - 1), (uint16_t)(y + h - 1));
  for (uint16_t i = 0; i < sizeof fill_buf; i += 2) { fill_buf[i] = (uint8_t)(color >> 8); fill_buf[i + 1] = (uint8_t)color; }
  uint32_t left = 2UL * w * h;
  ram_begin();
  while (left) {
    uint16_t n = left > sizeof fill_buf ? (uint16_t)sizeof fill_buf : (uint16_t)left;
    tx(fill_buf, n);
    left -= n;
  }
  ram_end();
}

/* 5x7 character, 1 column and 1 row gap, scaled 1 to 3 times */
static void draw_char(uint16_t x, uint16_t y, char c, uint8_t scale, uint16_t fg, uint16_t bg)
{
  if (c >= 'a' && c <= 'z') c = (char)(c - 32);
  if (c < ' ' || c > 'Z') c = '?';
  const uint8_t *glyph = font5x7[c - ' '];
  const uint16_t w = (uint16_t)(6U * scale), h = (uint16_t)(8U * scale);
  uint16_t n = 0;
  for (uint16_t py = 0; py < h; py++) {
    for (uint16_t px = 0; px < w; px++) {
      const uint16_t col = px / scale, row = py / scale;
      const uint8_t on = (col < 5 && row < 7) ? (uint8_t)((glyph[col] >> row) & 1U) : 0U;
      const uint16_t color = on ? fg : bg;
      cell[n++] = (uint8_t)(color >> 8);
      cell[n++] = (uint8_t)color;
    }
  }
  window(x, y, (uint16_t)(x + w - 1), (uint16_t)(y + h - 1));
  ram_begin();
  tx(cell, n);
  ram_end();
}

static void draw_text(uint16_t x, uint16_t y, const char *s, uint8_t scale, uint16_t fg, uint16_t bg)
{
  for (; *s; s++, x = (uint16_t)(x + 6U * scale)) draw_char(x, y, *s, scale, fg, bg);
}

uint32_t Tft_ActualKhz(void)
{
  return HAL_RCCEx_GetPeriphCLKFreq(RCC_PERIPHCLK_SPI123) / TFT_DIV / 1000U;
}

uint8_t Tft_Init(void)
{
  GPIO_InitTypeDef g = {0};
  __HAL_RCC_GPIOA_CLK_ENABLE();
  __HAL_RCC_GPIOC_CLK_ENABLE();

  RCC_PeriphCLKInitTypeDef clk = {0};              /* SPI1 kernel clock = PLL1Q (64 MHz) */
  clk.PeriphClockSelection = RCC_PERIPHCLK_SPI123;
  clk.Spi123ClockSelection = RCC_SPI123CLKSOURCE_PLL;
  if (HAL_RCCEx_PeriphCLKConfig(&clk) != HAL_OK) return 0;

  g.Mode  = GPIO_MODE_OUTPUT_PP;
  g.Speed = GPIO_SPEED_FREQ_LOW;                   /* CS, DC and RES change rarely: slow edges, less crosstalk onto SCK */
  g.Pin = TFT_CS_PIN;  HAL_GPIO_WritePin(TFT_CS_PORT, TFT_CS_PIN, GPIO_PIN_SET);   HAL_GPIO_Init(TFT_CS_PORT, &g);
  g.Pin = TFT_DC_PIN;  HAL_GPIO_Init(TFT_DC_PORT, &g);
  g.Pin = TFT_RES_PIN; HAL_GPIO_WritePin(TFT_RES_PORT, TFT_RES_PIN, GPIO_PIN_SET); HAL_GPIO_Init(TFT_RES_PORT, &g);

  hspi1.Instance                        = SPI1;
  hspi1.Init.Mode                       = SPI_MODE_MASTER;
  hspi1.Init.Direction                  = SPI_DIRECTION_2LINES_TXONLY;
  hspi1.Init.DataSize                   = SPI_DATASIZE_8BIT;
  hspi1.Init.CLKPolarity                = SPI_POLARITY_LOW;
  hspi1.Init.CLKPhase                   = SPI_PHASE_1EDGE;
  hspi1.Init.NSS                        = SPI_NSS_SOFT;
  hspi1.Init.BaudRatePrescaler          = TFT_PRESCALER;
  hspi1.Init.FirstBit                   = SPI_FIRSTBIT_MSB;
  hspi1.Init.TIMode                     = SPI_TIMODE_DISABLE;
  hspi1.Init.CRCCalculation             = SPI_CRCCALCULATION_DISABLE;
  hspi1.Init.CRCPolynomial              = 7;
  hspi1.Init.CRCLength                  = SPI_CRC_LENGTH_DATASIZE;
  hspi1.Init.NSSPMode                   = SPI_NSS_PULSE_DISABLE;
  hspi1.Init.NSSPolarity                = SPI_NSS_POLARITY_LOW;
  hspi1.Init.FifoThreshold              = SPI_FIFO_THRESHOLD_01DATA;
  hspi1.Init.MasterSSIdleness           = SPI_MASTER_SS_IDLENESS_00CYCLE;
  hspi1.Init.MasterInterDataIdleness    = SPI_MASTER_INTERDATA_IDLENESS_00CYCLE;
  hspi1.Init.MasterReceiverAutoSusp     = SPI_MASTER_RX_AUTOSUSP_DISABLE;
  hspi1.Init.MasterKeepIOState          = SPI_MASTER_KEEP_IO_STATE_ENABLE;   /* SCK stays idle low between transfers */
  hspi1.Init.IOSwap                     = SPI_IO_SWAP_DISABLE;
  if (HAL_SPI_Init(&hspi1) != HAL_OK) return 0;

  /* hardware reset, then the minimal ST7789V start-up */
  HAL_GPIO_WritePin(TFT_RES_PORT, TFT_RES_PIN, GPIO_PIN_RESET); HAL_Delay(20);
  HAL_GPIO_WritePin(TFT_RES_PORT, TFT_RES_PIN, GPIO_PIN_SET);   HAL_Delay(120);
  cmd(0x01, 0, 0);                                 /* SWRESET */
  HAL_Delay(150);
  cmd(0x11, 0, 0);                                 /* SLPOUT */
  HAL_Delay(120);
  { const uint8_t v = 0x55; cmd(0x3A, &v, 1); }    /* COLMOD: 16 bit/pixel (RGB565) */
  { const uint8_t v = TFT_BGR ? 0x08 : 0x00; cmd(0x36, &v, 1); }   /* MADCTL: portrait, RGB/BGR order */
#if TFT_INVERT
  cmd(0x21, 0, 0);                                 /* INVON: most IPS panels need it */
#endif
  cmd(0x13, 0, 0);                                 /* NORON */
  cmd(0x29, 0, 0);                                 /* DISPON */
  HAL_Delay(20);

  /* test screen */
  fill_rect(0, 0, TFT_WIDTH, TFT_HEIGHT, C_BLACK);
  draw_text(8, 8,  "ST7789V", 3, C_WHITE, C_BLACK);
  char line[24];
  snprintf(line, sizeof line, "SPI1 %lu KHZ", (unsigned long)Tft_ActualKhz());
  draw_text(8, 40, line, 2, C_CYAN, C_BLACK);
  draw_text(8, 64, "240X320 MODE 0", 2, C_YELLOW, C_BLACK);
  fill_rect(0,   100, 60, 40, C_RED);
  fill_rect(60,  100, 60, 40, C_GREEN);
  fill_rect(120, 100, 60, 40, C_BLUE);
  fill_rect(180, 100, 60, 40, C_WHITE);
  draw_text(8, 160, "COUNT", 2, C_YELLOW, C_BLACK);

  tft_ok = 1;
  next_due = HAL_GetTick() + TFT_UPDATE_MS;
  return 1;
}

void Tft_Task(uint32_t now_ms)
{
  if (!tft_ok || (int32_t)(now_ms - next_due) < 0) return;
  next_due += TFT_UPDATE_MS;
  count++;

  /* redraw only the digits that changed: a few small windows, keeps the main loop free */
  char now_s[8], old_s[8];
  snprintf(now_s, sizeof now_s, "%06lu", (unsigned long)(count % 1000000UL));
  snprintf(old_s, sizeof old_s, "%06lu", (unsigned long)(shown % 1000000UL));
  for (uint8_t i = 0; i < 6; i++)
    if (shown == 0xFFFFFFFFU || now_s[i] != old_s[i])
      draw_char((uint16_t)(8U + 18U * i), 184, now_s[i], 3, C_WHITE, C_BLACK);
  shown = count;
}

#else  /* !FEATURE_TFT_SPI */

uint8_t  Tft_Init(void)               { return 0; }
void     Tft_Task(uint32_t now_ms)    { (void)now_ms; }
uint32_t Tft_ActualKhz(void)          { return 0; }

#endif
