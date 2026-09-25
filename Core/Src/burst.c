/*
 * Burst capture: 8 channels (PD0-PD7) at 48 MS/s.
 *
 * TIM2 update events (every 20.83 ns) trigger DMA1 Stream0, which copies
 * GPIOD->IDR (1 byte = 8 channels) into a ring of 8 segments x 32 KB in D2
 * SRAM. The stream runs in double-buffer mode: two segments are live at a
 * time and the completion ISR re-points the finished one at the segment
 * two ahead, so the ring is 256 KB even though NDTR is only 16 bits.
 *
 * The main loop scans each completed segment for the trigger edge. On a
 * hit at segment k it asks the ISR to stop the sampler at the end of
 * segment k+POST_SEGS. The 7 newest segments (k-3 .. k+3 typically, ~4.7 ms)
 * are then uploaded and the ring is re-armed.
 *
 * USB frame (little-endian):
 *   [0]     0xE7 magic          [1]     version (1)
 *   [2..3]  flags (bit0 = auto trigger, no real edge seen)
 *   [4..7]  sample rate (Hz)    [8..11] number of samples that follow
 *   [12..15] trigger sample index within the data
 *   [16..19] frame sequence     [20..23] reserved
 *   then num_samples raw bytes, bit n = channel Dn
 */
#include "burst.h"
#include "usbd_cdc_if.h"

extern DMA_HandleTypeDef hdma_tim2_up;
extern USBD_HandleTypeDef hUsbDeviceFS;

#define SEG_BYTES       32768U   /* NDTR is 16 bit: max 65535 */
#define NUM_SEGS        8U
#define SEND_SEGS       7U       /* the 8th slot is being overwritten at stop */
#define POST_SEGS       3U
#define SAMPLE_RATE_HZ  48000000U
#define NO_STOP         0xFFFFFFFFU

#define TRIG_CH         0U       /* PD0 */
#define TRIG_RISING     1U
#define TRIG_WORD_MASK  ((1U << TRIG_CH) * 0x01010101U)
#define AUTO_TRIGGER_MS 500U     /* capture anyway if no edge shows up */

#define FLAG_AUTO       0x0001U

static uint8_t burst_buf[NUM_SEGS][SEG_BYTES]
    __attribute__((section(".dma_buffers")))
    __attribute__((aligned(32)));

typedef struct __attribute__((packed)) {
    uint8_t  magic;
    uint8_t  version;
    uint16_t flags;
    uint32_t sample_rate;
    uint32_t num_samples;
    uint32_t trigger_index;
    uint32_t seq;
    uint32_t reserved;
} burst_hdr_t;

extern volatile uint32_t pin_sweep_result;   /* main.c: GPIO pin sweep */

static burst_hdr_t hdr;

typedef enum { ST_ARMED, ST_SENDING } state_t;
static state_t state = ST_ARMED;

static volatile uint32_t seg_done;          /* segments completed (ISR) */
static volatile uint32_t stop_seg = NO_STOP;/* stop after this segment completes */
static volatile uint32_t stopped_at;
static volatile uint8_t  captured;

static uint32_t scanned;
static uint8_t  trig_prev;
static uint32_t trig_abs;                   /* absolute trigger sample index */
static uint16_t trig_flags;
static uint32_t armed_tick;
static uint32_t frame_seq;
static uint32_t send_step;
static uint32_t send_first_seg;

volatile uint32_t burst_count = 0;
volatile uint32_t burst_auto  = 0;

/* Runs in the DMA ISR when either double-buffer half finishes. */
static void seg_complete(DMA_HandleTypeDef *h)
{
    uint32_t j = seg_done;                  /* segment that just finished */
    seg_done = j + 1U;

    /* The finished buffer is free again: point it at segment j+2. */
    HAL_DMAEx_ChangeMemory(h, (uint32_t)burst_buf[(j + 2U) % NUM_SEGS],
                           (j & 1U) ? MEMORY1 : MEMORY0);

    if (j == stop_seg) {
        TIM2->CR1 &= ~TIM_CR1_CEN;          /* freeze the sampler */
        stopped_at = j;
        captured = 1;
    }
}

static void arm(void)
{
    DMA_Stream_TypeDef *s = (DMA_Stream_TypeDef *)hdma_tim2_up.Instance;

    TIM2->CR1  &= ~TIM_CR1_CEN;
    TIM2->DIER &= ~TIM_DIER_UDE;
    if (hdma_tim2_up.State != HAL_DMA_STATE_READY) HAL_DMA_Abort(&hdma_tim2_up);
    s->CR &= ~(DMA_SxCR_DBM | DMA_SxCR_CT); /* start on memory 0 again */

    hdma_tim2_up.XferHalfCpltCallback   = NULL;
    hdma_tim2_up.XferM1HalfCpltCallback = NULL;
    HAL_DMA_RegisterCallback(&hdma_tim2_up, HAL_DMA_XFER_CPLT_CB_ID,   seg_complete);
    HAL_DMA_RegisterCallback(&hdma_tim2_up, HAL_DMA_XFER_M1CPLT_CB_ID, seg_complete);

    seg_done  = 0;
    stop_seg  = NO_STOP;
    captured  = 0;
    scanned   = 0;
    trig_prev = (GPIOD->IDR >> TRIG_CH) & 1U;
    armed_tick = HAL_GetTick();

    HAL_DMAEx_MultiBufferStart_IT(&hdma_tim2_up, (uint32_t)&GPIOD->IDR,
                                  (uint32_t)burst_buf[0], (uint32_t)burst_buf[1],
                                  SEG_BYTES);

    /* 240 MHz / 5 = 48 MS/s */
    TIM2->PSC = 0;
    TIM2->ARR = 4;
    TIM2->CNT = 0;
    TIM2->EGR = TIM_EGR_UG;
    TIM2->SR  = 0;
    TIM2->DIER |= TIM_DIER_UDE;
    TIM2->CR1  |= TIM_CR1_CEN;

    state = ST_ARMED;
}

void Burst_Init(void)
{
    /* D2 SRAM clocks are off at reset; burst_buf lives in RAM_D2 */
    __HAL_RCC_D2SRAM1_CLK_ENABLE();
    __HAL_RCC_D2SRAM2_CLK_ENABLE();
    __HAL_RCC_D2SRAM3_CLK_ENABLE();

    GPIO_InitTypeDef gpio = {0};
    __HAL_RCC_GPIOD_CLK_ENABLE();
    gpio.Pin   = 0x00FF;
    gpio.Mode  = GPIO_MODE_INPUT;
    gpio.Pull  = GPIO_PULLDOWN;
    gpio.Speed = GPIO_SPEED_FREQ_VERY_HIGH;
    HAL_GPIO_Init(GPIOD, &gpio);
}

void Burst_Start(void)
{
    arm();
}

/* Returns the offset of the first trigger edge in this segment, or -1. */
static int32_t scan_segment(const uint8_t *seg)
{
    const uint32_t *w = (const uint32_t *)seg;

    for (uint32_t i = 0; i < SEG_BYTES / 4U; i++) {
        uint32_t v = w[i];
        if ((v & TRIG_WORD_MASK) == (trig_prev ? TRIG_WORD_MASK : 0U)) continue;

        for (uint32_t k = 0; k < 4U; k++) {
            uint8_t cur = (uint8_t)((v >> (8U * k + TRIG_CH)) & 1U);
            uint8_t hit = TRIG_RISING ? (!trig_prev && cur) : (trig_prev && !cur);
            trig_prev = cur;
            if (hit) return (int32_t)(i * 4U + k);
        }
    }
    return -1;
}

static void request_stop(uint32_t k, uint32_t abs_idx, uint16_t flags)
{
    trig_abs   = abs_idx;
    trig_flags = flags;
    uint32_t want = k + POST_SEGS;
    uint32_t next = seg_done + 1U;          /* never a segment that already finished */
    stop_seg = want > next ? want : next;
}

/* True when USB is up and its IN endpoint is free. */
static uint8_t usb_idle(void)
{
    if (hUsbDeviceFS.dev_state != USBD_STATE_CONFIGURED) return 0;
    USBD_CDC_HandleTypeDef *hcdc = (USBD_CDC_HandleTypeDef *)hUsbDeviceFS.pClassData;
    return hcdc && hcdc->TxState == 0;
}

static void begin_send(void)
{
    uint32_t last  = stopped_at;
    uint32_t first = last - (SEND_SEGS - 1U);
    uint32_t start = first * SEG_BYTES;

    hdr.magic       = 0xE7;
    hdr.version     = 1;
    hdr.flags       = trig_flags;
    hdr.sample_rate = SAMPLE_RATE_HZ;
    hdr.num_samples = SEND_SEGS * SEG_BYTES;
    hdr.trigger_index = (trig_abs >= start && trig_abs - start < hdr.num_samples)
                        ? trig_abs - start : 0;
    hdr.seq         = frame_seq++;
    hdr.reserved    = pin_sweep_result;

    send_first_seg = first;
    send_step = 0;
    state = ST_SENDING;
}

void Burst_Process(void)
{
    if (state == ST_ARMED) {
        if (stop_seg == NO_STOP) {
            uint32_t done = seg_done;
            if (done - scanned >= NUM_SEGS - 1U) scanned = done - 1U; /* fell behind */

            while (scanned < done && stop_seg == NO_STOP) {
                uint32_t k = scanned++;
                int32_t off = scan_segment(burst_buf[k % NUM_SEGS]);
                /* need ~3 segments of history before the trigger */
                if (off >= 0 && k >= 3U) request_stop(k, k * SEG_BYTES + (uint32_t)off, 0);
            }

            if (stop_seg == NO_STOP && seg_done >= 4U &&
                HAL_GetTick() - armed_tick >= AUTO_TRIGGER_MS) {
                uint32_t k = seg_done - 1U;
                request_stop(k, k * SEG_BYTES, FLAG_AUTO);
                burst_auto++;
            }
        }
        if (captured) begin_send();
        return;
    }

    /* ST_SENDING: header, then the segments in time order. Never blocks. */
    if (!usb_idle()) return;

    if (send_step == 0) {
        if (CDC_Transmit_FS((uint8_t *)&hdr, sizeof(hdr)) == USBD_OK) send_step++;
    } else if (send_step <= SEND_SEGS) {
        uint32_t seg = send_first_seg + (send_step - 1U);
        if (CDC_Transmit_FS(burst_buf[seg % NUM_SEGS], SEG_BYTES) == USBD_OK) send_step++;
    } else {
        burst_count++;
        arm();
    }
}
