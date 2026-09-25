/*

@author         : abhishekshukla9586@gmail.com

Hardware:  TIM2 overflows (every 20.83ns)
               │
               │ Update Event (UEV) on TIM2->SR
               ▼
Hardware:  DMAMUX1 sees TIM2_UP request
               │
               │ fires DMA1_Stream0 transfer
               ▼
Hardware:  DMA copies 1 byte: GPIOD->IDR → capture_buf[i]
               │
               │ after 4096 bytes (half buffer full)
               ▼
Hardware:  DMA raises HTIF0 flag in DMA1->LISR
               │
               │ NVIC sees DMA1_Stream0_IRQn pending
               ▼
CPU:       DMA1_Stream0_IRQHandler()        ← stm32h7xx_it.c
               │
               ▼
CPU:       HAL_DMA_IRQHandler(&hdma_tim2_up) ← HAL driver
               │
               │ reads DMA1->LISR, sees HTIF0 set
               │ clears HTIF0 (that's why LISR=0 when we read it after)
               │ checks hdma->XferHalfCpltCallback != NULL
               ▼
CPU:       acq_dma_half(hdma)               ← acquisition.c
               │
               ▼
           half_ready = 1
           dma_half_count++
               │
               │ ISR returns
               ▼
CPU:       main loop resumes
               │
               ▼
CPU:       Acquisition_Process()
               │
               │ sees half_ready == 1
               ▼
CPU:       process_half(capture_buf, 4096)
               │
               ▼
CPU:       usb_pump() → CDC_Transmit_FS()

After another 4096 bytes (full buffer):


Hardware:  DMA raises TCIF0 in DMA1->LISR
               │
               ▼
CPU:       DMA1_Stream0_IRQHandler()
               │
               ▼
CPU:       HAL_DMA_IRQHandler()
               │
               │ sees TCIF0, clears it
               │ DMA wraps back to start of capture_buf (circular mode)
               ▼
CPU:       acq_dma_cplt(hdma)
               │
               ▼
           half_ready = 2
           dma_cplt_count++
               │
               ▼
CPU:       Acquisition_Process()
               │
               ▼
CPU:       process_half(capture_buf + 4096, 4096)

*/



#include "acquisition.h"




extern TIM_HandleTypeDef htim2;
extern DMA_HandleTypeDef hdma_tim2_up;
extern void Error_Handler(void);
#include "usbd_cdc_if.h"
extern USBD_HandleTypeDef hUsbDeviceFS;
#include <string.h>

#define CAPTURE_HALF  4096U
#define CAPTURE_TOTAL (CAPTURE_HALF * 2U)

static uint8_t capture_buf[CAPTURE_TOTAL]
    __attribute__((section(".dma_buffers")))
    __attribute__((aligned(32)));

/* ---- USB stream format -------------------------------------------------
 * Edges are batched into self-contained chunks:
 *   [0]      0xE6 magic
 *   [1]      seq (wraps)
 *   [2..3]   payload length in bytes (LE)
 *   [4..11]  base sample index (LE u64); edge times accumulate from here
 *   [12..13] lost count since previous chunk (LE u16): skipped halves +
 *            dropped edges. Nonzero => data was lost before this chunk.
 *   payload: LEB128 varints, one per edge:
 *            v = (delta_samples << 4) | (level << 3) | channel
 *            delta = samples since previous edge (0 for simultaneous edges)
 * Sample period is 125/6 ns (48 MS/s). Chunks are filled by the main loop
 * and sent whenever the USB endpoint is idle, so nothing ever blocks. */
#define CHUNK_MAGIC  0xE6U
#define CHUNK_HDR    14U
#define CHUNK_SIZE   512U
#define CHUNK_COUNT  8U
#define VARINT_MAX   10U

static uint8_t  chunk_buf[CHUNK_COUNT][CHUNK_SIZE];
static uint16_t fill_len     = 0;        /* bytes used in the chunk being filled */
static uint8_t  fill_open    = 0;
static uint32_t chunks_closed = 0;       /* closed (ready or sent) */
static uint32_t chunks_sent   = 0;       /* handed to USB */
static uint32_t chunks_done   = 0;       /* USB finished, slot reusable */
static uint8_t  chunk_seq    = 0;
static uint32_t lost_pending = 0;
static uint64_t last_edge_idx = 0;
static uint32_t snap_buf[CAPTURE_HALF / 4U];   /* private copy of the half being processed */
static uint8_t  resync       = 0;
static uint64_t sample_idx   = 0;   /* samples since start; ns = idx*125/6 */
static uint8_t  prev_levels  = 0;
/* Halves completed by DMA (ISR) vs. consumed by main loop. Half k lives in
 * buffer offset (k&1)*CAPTURE_HALF. Counters, not a flag, so slow USB TX
 * cannot silently drop halves and compress the timeline. */
static volatile uint32_t halves_produced = 0;
static uint32_t halves_consumed = 0;
uint32_t acq_overruns = 0;
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
    /* D2 SRAM clocks are off at reset; capture_buf lives in RAM_D2 */
    __HAL_RCC_D2SRAM1_CLK_ENABLE();
    __HAL_RCC_D2SRAM2_CLK_ENABLE();
    __HAL_RCC_D2SRAM3_CLK_ENABLE();
    GPIO_Acq_Init();
    prev_levels  = (uint8_t)(GPIOD->IDR & 0xFF);
    sample_idx   = 0;
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
    if (h->Instance == TIM2) { dma_half_count++; halves_produced++; }
}
void HAL_TIM_PeriodElapsedCallback(TIM_HandleTypeDef *h)
{
    if (h->Instance == TIM2) { dma_cplt_count++; halves_produced++; }
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
static void chunk_close(void)
{
    uint8_t *c = chunk_buf[chunks_closed % CHUNK_COUNT];
    uint16_t payload = fill_len - CHUNK_HDR;
    uint16_t lost = lost_pending > 0xFFFFU ? 0xFFFFU : (uint16_t)lost_pending;
    lost_pending = 0;
    c[1]  = chunk_seq++;
    c[2]  = payload & 0xFF;
    c[3]  = payload >> 8;
    c[12] = lost & 0xFF;
    c[13] = lost >> 8;
    chunks_closed++;
    fill_open = 0;
}

static uint8_t *chunk_open(void)
{
    if (fill_open) return chunk_buf[chunks_closed % CHUNK_COUNT];
    if (chunks_closed - chunks_done >= CHUNK_COUNT) return NULL;  /* all busy */
    uint8_t *c = chunk_buf[chunks_closed % CHUNK_COUNT];
    c[0] = CHUNK_MAGIC;
    for (int i = 0; i < 8; i++) c[4 + i] = (uint8_t)(last_edge_idx >> (8 * i));
    fill_len  = CHUNK_HDR;
    fill_open = 1;
    return c;
}

static void emit_edge(uint64_t idx, uint8_t ch, uint8_t level)
{
    uint8_t *c = chunk_open();
    if (!c) { lost_pending++; return; }

    uint64_t v = ((idx - last_edge_idx) << 4) | ((uint64_t)level << 3) | ch;
    last_edge_idx = idx;
    uint8_t *p = c + fill_len;
    do {
        uint8_t b = v & 0x7F;
        v >>= 7;
        if (v) b |= 0x80;
        *p++ = b;
    } while (v);
    fill_len = (uint16_t)(p - c);

    if (fill_len > CHUNK_SIZE - VARINT_MAX) chunk_close();
}

/* Feed USB whenever its endpoint is idle; never blocks. */
static void usb_pump(void)
{
    if (hUsbDeviceFS.dev_state != USBD_STATE_CONFIGURED) return;
    USBD_CDC_HandleTypeDef *hcdc = (USBD_CDC_HandleTypeDef *)hUsbDeviceFS.pClassData;
    if (!hcdc) return;

    if (chunks_sent != chunks_done && hcdc->TxState == 0) chunks_done = chunks_sent;
    if (chunks_sent != chunks_done) return;          /* still in flight */

    /* Nothing queued: flush the partly filled chunk so latency stays low. */
    if (chunks_sent == chunks_closed && fill_open && fill_len > CHUNK_HDR)
        chunk_close();

    if (chunks_sent != chunks_closed) {
        uint8_t *c = chunk_buf[chunks_sent % CHUNK_COUNT];
        uint16_t len = CHUNK_HDR + (uint16_t)(c[2] | (c[3] << 8));
        if (CDC_Transmit_FS(c, len) == USBD_OK) chunks_sent++;
    }
}

static void process_half(const uint8_t *buf, uint32_t count)
{
    /* No DCache invalidation -- capture_buf is in non-cached .dma_buffers region.
     * Fast path: compare 4 samples per word against the current level; only
     * touch per-sample / timestamp math when something changed. */
    const uint32_t *w = (const uint32_t *)buf;

    /* After skipped halves our idea of the line state is stale; take the
     * first sample as truth instead of inventing an edge at the half start. */
    if (resync) { prev_levels = buf[0]; resync = 0; }
    uint32_t prev_w = prev_levels * 0x01010101U;

    for (uint32_t i = 0; i < count / 4U; i++) {
        uint32_t v = w[i];
        if (v == prev_w) continue;

        for (uint32_t k = 0; k < 4U; k++) {
            uint8_t curr = (uint8_t)(v >> (8U * k));
            uint8_t diff = curr ^ prev_levels;
            if (!diff) continue;

            uint64_t idx = sample_idx + i * 4U + k;
            for (int ch = 0; ch < 8; ch++) {
                if (diff & (1 << ch))
                    emit_edge(idx, (uint8_t)ch, (curr >> ch) & 1U);
            }
            prev_levels = curr;
        }
        prev_w = prev_levels * 0x01010101U;
    }

    sample_idx += count;
}

void Acquisition_Process(void)
{
    usb_pump();

    uint32_t n = halves_produced - halves_consumed;
    if (!n) return;

    /* DMA finishing half k+1 means half k is being overwritten. If we are
     * that far behind, skip to the newest complete half. */
    if (n >= 2) {
        uint32_t skip = n - 1;
        sample_idx += (uint64_t)skip * CAPTURE_HALF;
        halves_consumed += skip;
        acq_overruns++;
        lost_pending += skip;
        resync = 1;
    }

    /* Snapshot the half before touching it: processing can take longer than
     * one half period, and the DMA would overwrite the slot mid-scan and
     * splice later samples into this one (corrupted edge times). The copy is
     * much faster than the DMA writer, so it stays ahead of it. */
    uint32_t h = halves_consumed;
    const uint32_t *src = (const uint32_t *)(capture_buf + (h & 1U) * CAPTURE_HALF);
    for (uint32_t i = 0; i < CAPTURE_HALF / 4U; i++) snap_buf[i] = src[i];

    halves_consumed = h + 1;

    /* Half h+1 completing means the DMA started rewriting this slot; if that
     * happened before/while we copied, the snapshot may be spliced. Drop it. */
    if (halves_produced - h >= 2U) {
        sample_idx += CAPTURE_HALF;
        acq_overruns++;
        lost_pending++;
        resync = 1;
        return;
    }

    process_half((const uint8_t *)snap_buf, CAPTURE_HALF);
}


void acq_dma_half(DMA_HandleTypeDef *h)
{
    (void)h;
    GPIOC->ODR ^= GPIO_PIN_8;  /* PC8 debug: toggles once per 4096 samples */
    dma_half_count++;
    halves_produced++;
}

void acq_dma_cplt(DMA_HandleTypeDef *h)
{
    (void)h;
    GPIOC->ODR ^= GPIO_PIN_8;
    dma_cplt_count++;
    halves_produced++;
}
