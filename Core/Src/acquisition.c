#include "acquisition.h"
extern TIM_HandleTypeDef htim2;
extern DMA_HandleTypeDef hdma_tim2_up;
extern void Error_Handler(void);
#include "usbd_cdc_if.h"
#include <string.h>

#define CAPTURE_HALF  4096U
#define CAPTURE_TOTAL (CAPTURE_HALF * 2U)

static uint8_t capture_buf[CAPTURE_TOTAL]
    __attribute__((section(".dma_buffers")))
    __attribute__((aligned(32)));

#define MAX_EDGES  50U
#define HDR_SIZE   4U
#define EDGE_SIZE  5U
#define PKT_MAX    (HDR_SIZE + MAX_EDGES * EDGE_SIZE)

static uint8_t  edge_pkt[PKT_MAX];
static uint8_t  pkt_seq      = 0;
static uint8_t  edge_count   = 0;
static uint32_t timestamp_ns = 0;
static uint8_t  prev_levels  = 0;
static uint32_t ns_frac      = 0;
static volatile uint8_t half_ready = 0;
uint8_t acq_error = 0;

TIM_HandleTypeDef htim1;

static void GPIO_Acq_Init(void)
{
    GPIO_InitTypeDef gpio = {0};
    __HAL_RCC_GPIOD_CLK_ENABLE();
    gpio.Pin   = 0x00FF;
    gpio.Mode  = GPIO_MODE_INPUT;
    gpio.Pull  = GPIO_PULLDOWN;
    gpio.Speed = GPIO_SPEED_FREQ_VERY_HIGH;
    HAL_GPIO_Init(GPIOD, &gpio);
}

void Acquisition_Init(void)
{
    GPIO_Acq_Init();
    prev_levels  = (uint8_t)(GPIOD->IDR & 0xFF);
    timestamp_ns = 0;
    ns_frac      = 0;
    edge_count   = 0;
}

static void TIM1_Acq_Init(void)
{
    /* TIM2: general purpose timer, no conflict with USB or HAL tick
     * APB1 timer clock = 240MHz (APB1=120MHz, timer input = 2x = 240MHz)
     * Period=4, PSC=0 -> 240MHz/5 = 48MS/s */
    __HAL_RCC_TIM2_CLK_ENABLE();
    htim1.Instance               = TIM2;
    htim1.Init.Prescaler         = 0;
    htim1.Init.CounterMode       = TIM_COUNTERMODE_UP;
    htim1.Init.Period            = 4;
    htim1.Init.ClockDivision     = TIM_CLOCKDIVISION_DIV1;
    htim1.Init.RepetitionCounter = 0;
    htim1.Init.AutoReloadPreload = TIM_AUTORELOAD_PRELOAD_ENABLE;
    if (HAL_TIM_Base_Init(&htim1) != HAL_OK) {
        acq_error = 2;
        return;
    }
}

/* HAL_TIM_Base_Start_DMA routes through TIM period elapsed callbacks */
uint32_t dma_half_count = 0;
uint32_t dma_cplt_count = 0;
uint32_t dma_isr_val = 0;
uint32_t dma_state = 0;

void HAL_TIM_PeriodElapsedHalfCpltCallback(TIM_HandleTypeDef *h)
{
    if (h->Instance == TIM2) { dma_half_count++; half_ready = 1; }
}
void HAL_TIM_PeriodElapsedCallback(TIM_HandleTypeDef *h)
{
    if (h->Instance == TIM2) { dma_cplt_count++; half_ready = 2; }
}


void Acquisition_Start(void)
{
    TIM1_Acq_Init();

    /* Start DMA: GPIOD->IDR -> capture_buf */
    if (HAL_DMA_Start_IT(&hdma_tim2_up,
            (uint32_t)&GPIOD->IDR,
            (uint32_t)capture_buf,
            CAPTURE_TOTAL) != HAL_OK) {
        acq_error = 1;
        return;
    }
    __HAL_TIM_ENABLE_DMA(&htim1, TIM_DMA_UPDATE);
    HAL_TIM_Base_Start(&htim1);
    acq_error = 0;
}
void Acquisition_Stop(void)    {}
static void flush_packet(void)
{
    if (edge_count == 0) return;
    edge_pkt[0] = 0xE5;
    edge_pkt[1] = pkt_seq++;
    edge_pkt[2] = 0;
    edge_pkt[3] = edge_count;
    uint32_t t = HAL_GetTick();
    while (CDC_Transmit_FS(edge_pkt,
           HDR_SIZE + edge_count * EDGE_SIZE) == USBD_BUSY)
        if (HAL_GetTick() - t > 1) break;
    edge_count = 0;
}

static void process_half(const uint8_t *buf, uint32_t count)
{
    /* No DCache invalidation -- capture_buf is in non-cached .dma_buffers region */

    for (uint32_t i = 0; i < count; i++) {
        uint8_t curr = buf[i];
        uint8_t diff = curr ^ prev_levels;

        if (diff) {
            for (int ch = 0; ch < 8; ch++) {
                if (!(diff & (1 << ch))) continue;
                uint8_t *e = edge_pkt + HDR_SIZE + edge_count * EDGE_SIZE;
                e[0] = (timestamp_ns >> 24) & 0xFF;
                e[1] = (timestamp_ns >> 16) & 0xFF;
                e[2] = (timestamp_ns >>  8) & 0xFF;
                e[3] =  timestamp_ns        & 0xFF;
                e[4] = (uint8_t)(ch | (((curr >> ch) & 1) << 7));
                edge_count++;
                if (edge_count >= MAX_EDGES) flush_packet();
            }
            prev_levels = curr;
        }

        /* 125/6 ns per sample at 48MS/s */
        ns_frac += 125;
        timestamp_ns += ns_frac / 6;
        ns_frac %= 6;
    }
    flush_packet();
}

void Acquisition_Process(void)
{
    uint8_t h = half_ready;
    if (!h) return;
    half_ready = 0;
    if (h == 1) process_half(capture_buf,               CAPTURE_HALF);
    else        process_half(capture_buf + CAPTURE_HALF, CAPTURE_HALF);
}


void acq_dma_half(DMA_HandleTypeDef *h)
{
    (void)h;
    dma_half_count++;
    half_ready = 1;
}

void acq_dma_cplt(DMA_HandleTypeDef *h)
{
    (void)h;
    dma_cplt_count++;
    half_ready = 2;
}
