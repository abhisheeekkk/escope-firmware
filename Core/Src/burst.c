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
 * segment k+POST_SEGS. The 7 newest segments (k-pre .. k+post, ~4.7 ms)
 * are then uploaded and the ring is re-armed.
 *
 * Trigger (set at runtime by the PC, see Burst_Config_Rx; default PD0 rising):
 *   a rising, falling or either edge on any one channel. pre_segs of the 7
 *   sent segments come before the trigger segment. Protocol-aware triggers are
 *   deliberately not done here: the PC decodes protocols from the captured data.
 *
 * USB frame (little-endian):
 *   [0]     0xE7 magic          [1]     version (1)
 *   [2..3]  flags: bit0 = auto trigger (no real edge seen); bits 8-9 trigger
           mode, bits 10-12 trigger channel
 *   [4..7]  sample rate (Hz)    [8..11] number of samples that follow
 *   [12..15] trigger sample index within the data
 *   [16..19] frame sequence     [20..23] reserved
 *   then num_samples raw bytes, bit n = channel Dn
 */
#include "burst.h"
#include "usbd_cdc_if.h"
#include <string.h>

extern DMA_HandleTypeDef hdma_tim2_up;
extern USBD_HandleTypeDef hUsbDeviceFS;

#define SEG_BYTES       32768U   /* NDTR is 16 bit: max 65535 */
#define NUM_SEGS        8U
#define SEND_SEGS       7U       /* the 8th slot is being overwritten at stop */
#define SAMPLE_RATE_HZ  48000000U
#define NO_STOP         0xFFFFFFFFU

#define AUTO_TRIGGER_MS 500U     /* power-up default: capture anyway if no trigger shows up */

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
static uint8_t  trig_prev;                  /* previous sample (all 8 channels) */
static uint32_t trig_abs;                   /* absolute trigger sample index */
static uint16_t trig_flags;
static uint32_t armed_tick;
static uint32_t frame_seq;
static uint32_t send_step;
static uint32_t send_first_seg;

typedef struct {
    uint8_t mode;       /* TRIG_* */
    uint8_t ch;         /* trigger channel */
    uint8_t pre_segs;   /* 0..SEND_SEGS-1 segments kept before the trigger segment */
    uint16_t auto_ms;   /* Auto mode: capture anyway after this long; 0 = Normal (wait for a real trigger) */
} trig_cfg_t;

enum { TRIG_RISING = 0, TRIG_FALLING = 1, TRIG_EITHER = 2 };

static trig_cfg_t cfg     = { TRIG_RISING, 0, 3, AUTO_TRIGGER_MS };   /* power-on default: PD0 rising, Auto */
static trig_cfg_t pending;
static volatile uint8_t pending_flag;
static volatile uint8_t pull_mask, pull_flag;     /* requested "no pull-down" channels */

#define PRE_SEGS   (cfg.pre_segs)
#define POST_SEGS  (SEND_SEGS - 1U - cfg.pre_segs)

/* Host -> device command (8 bytes): C7 01 mode ch 00 pre auto xor(bytes 0..6)
 * auto = Auto-mode timeout in 10 ms units; 0 = Normal mode (wait for a real trigger). */
#define CMD_MAGIC        0xC7U
#define CMD_SET_TRIGGER  0x01U
#define CMD_SET_PULLS    0x02U   /* byte 2: bit n = 1 -> Dn has no pull-down (use for open-drain buses) */
#define CMD_LEN          8U

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
    trig_prev = (uint8_t)(GPIOD->IDR & 0xFFU);
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

/* Returns the offset of the first trigger event in this segment, or -1. */
static int32_t scan_segment(const uint8_t *seg)
{
    const uint8_t  ch   = cfg.ch;
    const uint32_t mask = (1U << ch) * 0x01010101U;
    const uint32_t *w = (const uint32_t *)seg;
    uint8_t prev = (trig_prev >> ch) & 1U;

    for (uint32_t i = 0; i < SEG_BYTES / 4U; i++) {
        uint32_t v = w[i];
        if ((v & mask) == (prev ? mask : 0U)) continue;     /* no change in these 4 samples */

        for (uint32_t k = 0; k < 4U; k++) {
            uint8_t cur = (uint8_t)((v >> (8U * k + ch)) & 1U);
            uint8_t hit = cfg.mode == TRIG_RISING  ? (!prev && cur)
                        : cfg.mode == TRIG_FALLING ? (prev && !cur)
                                                   : (prev != cur);
            prev = cur;
            if (hit) {
                trig_prev = (uint8_t)(v >> (8U * k));
                return (int32_t)(i * 4U + k);
            }
        }
    }
    trig_prev = (uint8_t)(w[SEG_BYTES / 4U - 1U] >> 24);
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
    hdr.flags       = trig_flags | ((uint16_t)cfg.mode << 8) | ((uint16_t)cfg.ch << 10);
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

/* Called from the USB receive callback with whatever bytes arrived. Commands
 * are 8 bytes starting with CMD_MAGIC; anything else is skipped to resync. */
void Burst_Config_Rx(const uint8_t *data, uint32_t len)
{
    static uint8_t cmd[CMD_LEN];
    static uint32_t n;

    for (uint32_t i = 0; i < len; i++) {
        if (n == 0 && data[i] != CMD_MAGIC) continue;
        cmd[n++] = data[i];
        if (n < CMD_LEN) continue;
        n = 0;

        uint8_t x = 0;
        for (uint32_t k = 0; k < CMD_LEN - 1U; k++) x ^= cmd[k];
        if (x != cmd[CMD_LEN - 1U]) continue;
        if (cmd[1] == CMD_SET_PULLS) {
            pull_mask = cmd[2];
            __DMB();
            pull_flag = 1;
            continue;
        }
        if (cmd[1] != CMD_SET_TRIGGER) continue;
        if (cmd[2] > TRIG_EITHER || cmd[3] > 7U || cmd[5] >= SEND_SEGS) continue;

        pending.mode = cmd[2]; pending.ch = cmd[3];
        pending.pre_segs = cmd[5];
        pending.auto_ms = (uint16_t)cmd[6] * 10U;
        __DMB();                                /* fields visible before the flag */
        pending_flag = 1;
    }
}

/* Take a new trigger setting when no capture is in progress. */
static void apply_pending(void)
{
    if (!pending_flag) return;
    __DMB();
    cfg = pending;
    pending_flag = 0;
}

/* PD0-PD7 are inputs with a pull-down so unused channels read low. An
 * open-drain bus (I2C) already has its own pull-ups; the pull-down would fight
 * them and eat into the high level, so those pins are released. */
static void apply_pulls(void)
{
    if (!pull_flag) return;
    __DMB();
    const uint32_t m = pull_mask;
    pull_flag = 0;
    uint32_t v = GPIOD->PUPDR;
    for (uint32_t ch = 0; ch < 8U; ch++) {
        v &= ~(3UL << (2U * ch));
        if (!(m & (1UL << ch))) v |= 2UL << (2U * ch);      /* 10 = pull-down */
    }
    GPIOD->PUPDR = v;
}

void Burst_Process(void)
{
    apply_pulls();
    if (pending_flag && state == ST_ARMED && stop_seg == NO_STOP && !captured) {
        apply_pending();
        arm();                                  /* restart the ring with the new trigger */
        return;
    }

    if (state == ST_ARMED) {
        if (stop_seg == NO_STOP) {
            uint32_t done = seg_done;
            if (done - scanned >= NUM_SEGS - 1U) scanned = done - 1U; /* fell behind */

            while (scanned < done && stop_seg == NO_STOP) {
                uint32_t k = scanned++;
                int32_t off = scan_segment(burst_buf[k % NUM_SEGS]);
                /* edge mode needs ~3 segments of history before the trigger */
                if (off >= 0 && k >= PRE_SEGS) request_stop(k, k * SEG_BYTES + (uint32_t)off, 0);
            }

            if (stop_seg == NO_STOP && seg_done >= 4U &&
                cfg.auto_ms && HAL_GetTick() - armed_tick >= cfg.auto_ms) {
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
        apply_pending();
        arm();
    }
}
