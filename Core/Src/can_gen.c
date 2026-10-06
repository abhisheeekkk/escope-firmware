/*
 * CAN test traffic generator on FDCAN1 (classic CAN, no bit-rate switching).
 *
 * Bit timing is worked out from the 8 MHz HSE kernel clock selected below: one
 * bit is (HSE / bitrate) time quanta (16 at 500 kbit/s, 8 at 1 Mbit/s), with the
 * sample point at 87.5 % (75 % at 1 Mbit/s, where there are only 8 quanta).
 */
#include "can_gen.h"
#include "board_config.h"
#include <stdio.h>

#if FEATURE_CAN_TEST

#define HSE_KHZ      (HSE_VALUE / 1000U)
#define TQ_PER_BIT   (HSE_KHZ / CAN_BITRATE_KBPS)
#if (TQ_PER_BIT * CAN_BITRATE_KBPS) != HSE_KHZ || TQ_PER_BIT < 8 || TQ_PER_BIT > 80
#error "CAN_BITRATE_KBPS must divide the HSE clock into 8 to 80 time quanta per bit"
#endif
#define SEG2         ((TQ_PER_BIT >= 16U) ? (TQ_PER_BIT / 8U) : 2U)
#define SEG1         (TQ_PER_BIT - 1U - SEG2)

static FDCAN_HandleTypeDef hfdcan1;
static uint32_t sent_count;
static uint32_t next_due;
static uint8_t  step;
static uint32_t counter;
static uint8_t  transfer_id;

uint8_t CanGen_Init(void)
{
  RCC_PeriphCLKInitTypeDef clk = {0};
  GPIO_InitTypeDef gpio = {0};

  /* FDCAN kernel clock = HSE, so the bit timing is exact and independent of the PLLs */
  clk.PeriphClockSelection = RCC_PERIPHCLK_FDCAN;
  clk.FdcanClockSelection  = RCC_FDCANCLKSOURCE_HSE;
  if (HAL_RCCEx_PeriphCLKConfig(&clk) != HAL_OK) return 0;

  __HAL_RCC_FDCAN_CLK_ENABLE();
  __HAL_RCC_GPIOD_CLK_ENABLE();
  gpio.Pin       = GPIO_PIN_0 | GPIO_PIN_1;                 /* RX, TX */
  gpio.Mode      = GPIO_MODE_AF_PP;
  gpio.Pull      = GPIO_PULLUP;                              /* recessive when nothing drives RX */
  gpio.Speed     = GPIO_SPEED_FREQ_HIGH;
  gpio.Alternate = GPIO_AF9_FDCAN1;
  HAL_GPIO_Init(GPIOD, &gpio);

  hfdcan1.Instance                  = FDCAN1;
  hfdcan1.Init.FrameFormat          = FDCAN_FRAME_CLASSIC;
  hfdcan1.Init.Mode                 = CAN_LOOPBACK ? FDCAN_MODE_EXTERNAL_LOOPBACK : FDCAN_MODE_NORMAL;
  hfdcan1.Init.AutoRetransmission   = ENABLE;
  hfdcan1.Init.TransmitPause        = DISABLE;
  hfdcan1.Init.ProtocolException    = DISABLE;
  hfdcan1.Init.NominalPrescaler     = 1;
  hfdcan1.Init.NominalSyncJumpWidth = SEG2;
  hfdcan1.Init.NominalTimeSeg1      = SEG1;
  hfdcan1.Init.NominalTimeSeg2      = SEG2;
  hfdcan1.Init.DataPrescaler        = 1;                     /* unused: classic CAN only */
  hfdcan1.Init.DataSyncJumpWidth    = 1;
  hfdcan1.Init.DataTimeSeg1         = 1;
  hfdcan1.Init.DataTimeSeg2         = 1;
  hfdcan1.Init.MessageRAMOffset     = 0;
  hfdcan1.Init.StdFiltersNbr        = 0;
  hfdcan1.Init.ExtFiltersNbr        = 0;
  hfdcan1.Init.RxFifo0ElmtsNbr      = CAN_DNA_SERVER ? 8 : 0;
  hfdcan1.Init.RxFifo0ElmtSize      = FDCAN_DATA_BYTES_8;
  hfdcan1.Init.RxFifo1ElmtsNbr      = 0;
  hfdcan1.Init.RxFifo1ElmtSize      = FDCAN_DATA_BYTES_8;
  hfdcan1.Init.RxBuffersNbr         = 0;
  hfdcan1.Init.RxBufferSize         = FDCAN_DATA_BYTES_8;
  hfdcan1.Init.TxEventsNbr          = 0;
  hfdcan1.Init.TxBuffersNbr         = 0;
  hfdcan1.Init.TxFifoQueueElmtsNbr  = 4;
  hfdcan1.Init.TxFifoQueueMode      = FDCAN_TX_FIFO_OPERATION;
  hfdcan1.Init.TxElmtSize           = FDCAN_DATA_BYTES_8;
  if (HAL_FDCAN_Init(&hfdcan1) != HAL_OK) return 0;

  /* Without the node ID server nothing is received: reject everything. The controller
   * still ACKs valid frames from other nodes (the ACK does not depend on the acceptance
   * filter), which a sensor on the bus needs. With the server, accept all into FIFO 0. */
  if (HAL_FDCAN_ConfigGlobalFilter(&hfdcan1,
        CAN_DNA_SERVER ? FDCAN_ACCEPT_IN_RX_FIFO0 : FDCAN_REJECT,
        CAN_DNA_SERVER ? FDCAN_ACCEPT_IN_RX_FIFO0 : FDCAN_REJECT,
        FDCAN_REJECT_REMOTE, FDCAN_REJECT_REMOTE) != HAL_OK) return 0;
  if (HAL_FDCAN_Start(&hfdcan1) != HAL_OK) return 0;
  next_due = HAL_GetTick() + CAN_GEN_PERIOD_MS;
  return 1;
}

static void send(uint32_t id, uint32_t id_type, uint32_t frame_type, uint8_t dlc, const uint8_t *data)
{
  FDCAN_TxHeaderTypeDef h = {0};
  h.Identifier          = id;
  h.IdType              = id_type;
  h.TxFrameType         = frame_type;
  h.DataLength          = dlc;                    /* FDCAN_DLC_BYTES_n == n for 0..8 */
  h.ErrorStateIndicator = FDCAN_ESI_ACTIVE;
  h.BitRateSwitch       = FDCAN_BRS_OFF;
  h.FDFormat            = FDCAN_CLASSIC_CAN;
  h.TxEventFifoControl  = FDCAN_NO_TX_EVENTS;
  h.MessageMarker       = 0;
  if (HAL_FDCAN_AddMessageToTxFifoQ(&hfdcan1, &h, data) == HAL_OK) sent_count++;
}

#if CAN_DNA_SERVER
/* ---- DroneCAN dynamic node ID allocation server --------------------------------------
 * A device with no node ID sends anonymous
 * uavcan.protocol.dynamic_node_id.Allocation requests (message type 1, source node 0) and
 * repeats them until a server answers. The 16 byte unique ID goes over in three requests
 * (6 + 6 + 4 bytes); the server echoes the part it has, and answers the last one with the
 * allocated node ID. Replies longer than 7 bytes are multi-frame transfers with a CRC-16
 * seeded by the data type signature. */
#define DNA_SERVER_NODE    1U
#define DNA_FIRST_NODE     20U
#define DNA_MAX_NODES      4U
#define DNA_ALLOC_SIG      0x0B2A812620A11D40ULL

static uint8_t dna_uid[16];
static uint8_t dna_len;
static uint8_t dna_tid;
static uint8_t node_status_tid;
static uint32_t node_status_due;
static struct { uint8_t uid[16]; uint8_t used; } dna_table[DNA_MAX_NODES];

static uint16_t crc16_ccitt(uint16_t crc, const uint8_t *p, uint32_t n)
{
  while (n--) {
    crc ^= (uint16_t)(*p++) << 8;
    for (int k = 0; k < 8; k++) crc = (crc & 0x8000U) ? (uint16_t)((crc << 1) ^ 0x1021U) : (uint16_t)(crc << 1);
  }
  return crc;
}

static void send(uint32_t id, uint32_t id_type, uint32_t frame_type, uint8_t dlc, const uint8_t *data);

/* One DroneCAN transfer (single or multi-frame) as a broadcast from the server node. */
static void dna_send_transfer(uint32_t id, const uint8_t *payload, uint8_t len, uint8_t *tid, uint64_t sig)
{
  if (len <= 7U) {
    uint8_t f[8];
    for (uint8_t i = 0; i < len; i++) f[i] = payload[i];
    f[len] = (uint8_t)(0xC0U | (*tid & 0x1FU));
    send(id, FDCAN_EXTENDED_ID, FDCAN_DATA_FRAME, (uint8_t)(len + 1U), f);
  } else {
    uint8_t sigb[8], buf[2 + 17];
    for (int i = 0; i < 8; i++) sigb[i] = (uint8_t)(sig >> (8 * i));
    uint16_t crc = crc16_ccitt(crc16_ccitt(0xFFFFU, sigb, 8), payload, len);
    buf[0] = (uint8_t)crc; buf[1] = (uint8_t)(crc >> 8);
    for (uint8_t i = 0; i < len; i++) buf[2 + i] = payload[i];
    uint8_t total = (uint8_t)(len + 2U), pos = 0, toggle = 0;
    while (pos < total) {
      uint8_t f[8], rest = (uint8_t)(total - pos), n = (rest > 7U) ? 7U : rest;
      for (uint8_t i = 0; i < n; i++) f[i] = buf[pos + i];
      uint8_t tail = (uint8_t)(*tid & 0x1FU);
      if (pos == 0) tail |= 0x80U;
      if (pos + n >= total) tail |= 0x40U;
      if (toggle) tail |= 0x20U;
      f[n] = tail;
      send(id, FDCAN_EXTENDED_ID, FDCAN_DATA_FRAME, (uint8_t)(n + 1U), f);
      pos = (uint8_t)(pos + n);
      toggle ^= 1U;
    }
  }
  (*tid)++;
}

static uint8_t dna_lookup(void)
{
  for (uint8_t i = 0; i < DNA_MAX_NODES; i++)
    if (dna_table[i].used) {
      uint8_t same = 1;
      for (uint8_t k = 0; k < 16; k++) if (dna_table[i].uid[k] != dna_uid[k]) { same = 0; break; }
      if (same) return (uint8_t)(DNA_FIRST_NODE + i);
    }
  for (uint8_t i = 0; i < DNA_MAX_NODES; i++)
    if (!dna_table[i].used) {
      dna_table[i].used = 1;
      for (uint8_t k = 0; k < 16; k++) dna_table[i].uid[k] = dna_uid[k];
      return (uint8_t)(DNA_FIRST_NODE + i);
    }
  return 0;                                       /* table full: leave unallocated */
}

static uint32_t stat_rx, stat_req, stat_rsp;

static void dna_handle(const FDCAN_RxHeaderTypeDef *h, const uint8_t *d)
{
  stat_rx++;
  if (h->IdType != FDCAN_EXTENDED_ID || h->RxFrameType != FDCAN_DATA_FRAME) return;
  const uint32_t id = h->Identifier;
  const uint8_t  dlc = (uint8_t)h->DataLength;           /* this HAL returns the DLC as a plain 0..8 number */
  if ((id & 0x7FU) != 0U || (id & 0x80U) != 0U || ((id >> 8) & 3U) != 1U) return;  /* anonymous Allocation */
  if (dlc < 3U || (d[dlc - 1U] & 0xC0U) != 0xC0U) return;                          /* single-frame request */
  stat_req++;
  const uint8_t first = d[0] & 1U, chunk = (uint8_t)(dlc - 2U);
  if (first) dna_len = 0;
  else if (dna_len == 0) return;                                                   /* no first stage seen */
  for (uint8_t i = 0; i < chunk && dna_len < 16U; i++) dna_uid[dna_len++] = d[1 + i];

  uint8_t rsp[17], n = 0, node = 0;
  if (dna_len >= 16U) node = dna_lookup();
  rsp[n++] = (uint8_t)(node << 1);                /* node_id (0 until complete), first_part = 0 */
  for (uint8_t i = 0; i < dna_len; i++) rsp[n++] = dna_uid[i];
  if (HAL_FDCAN_GetTxFifoFreeLevel(&hfdcan1) >= 3U) {
    stat_rsp++;
    dna_send_transfer((30UL << 24) | (1UL << 8) | DNA_SERVER_NODE, rsp, n, &dna_tid, DNA_ALLOC_SIG);
  }
  if (dna_len >= 16U) dna_len = 0;
}

static void dna_poll(uint32_t now_ms)
{
  FDCAN_RxHeaderTypeDef h;
  uint8_t d[8];
  while (HAL_FDCAN_GetRxFifoFillLevel(&hfdcan1, FDCAN_RX_FIFO0) > 0U &&
         HAL_FDCAN_GetRxMessage(&hfdcan1, FDCAN_RX_FIFO0, &h, d) == HAL_OK)
    dna_handle(&h, d);

  /* The server node itself announces NodeStatus once a second. */
  if ((int32_t)(now_ms - node_status_due) >= 0) {
    node_status_due = now_ms + 1000U;
    uint32_t up = now_ms / 1000U;
    uint8_t p[7] = {(uint8_t)up, (uint8_t)(up >> 8), (uint8_t)(up >> 16), (uint8_t)(up >> 24), 0, 0, 0};
    if (HAL_FDCAN_GetTxFifoFreeLevel(&hfdcan1) >= 1U)
      dna_send_transfer((24UL << 24) | (341UL << 8) | DNA_SERVER_NODE, p, 7, &node_status_tid, 0);
  }
}
#endif /* CAN_DNA_SERVER */

void CanGen_Task(uint32_t now_ms)
{
#if CAN_DNA_SERVER
  dna_poll(now_ms);
#endif
  if ((int32_t)(now_ms - next_due) < 0) return;
  next_due += CAN_GEN_PERIOD_MS;
  if (!CAN_GEN_TX) return;                /* listen only */

  uint8_t d[8];
  switch (step++ & 3U) {
  case 0:       /* standard 11-bit ID, 8 data bytes: a counter and a fixed pattern */
    d[0] = (uint8_t)counter;         d[1] = (uint8_t)(counter >> 8);
    d[2] = (uint8_t)(counter >> 16); d[3] = (uint8_t)(counter >> 24);
    d[4] = 0xA5; d[5] = 0x5A; d[6] = 0xFF; d[7] = 0x00;
    send(0x123, FDCAN_STANDARD_ID, FDCAN_DATA_FRAME, 8, d);
    counter++;
    break;
  case 1: {     /* DroneCAN NodeStatus (message type 341) from node 10, one frame, as an
                 * DroneCAN device sends it. ID = priority 24 | type 341 << 8 | node 10. */
    uint32_t uptime_s = now_ms / 1000U;
    d[0] = (uint8_t)uptime_s;         d[1] = (uint8_t)(uptime_s >> 8);
    d[2] = (uint8_t)(uptime_s >> 16); d[3] = (uint8_t)(uptime_s >> 24);
    d[4] = 0x00;                       /* health OK, mode operational */
    d[5] = 0x00; d[6] = 0x00;          /* vendor specific status */
    d[7] = (uint8_t)(0xC0U | (transfer_id++ & 0x1FU));   /* tail: start + end of transfer */
    send((24UL << 24) | (341UL << 8) | 10UL, FDCAN_EXTENDED_ID, FDCAN_DATA_FRAME, 8, d);
    break;
  }
  case 2:       /* highest standard ID, no data */
    send(0x7FF, FDCAN_STANDARD_ID, FDCAN_DATA_FRAME, 0, d);
    break;
  default:      /* remote frame: asks for 2 bytes, carries none */
    send(0x321, FDCAN_STANDARD_ID, FDCAN_REMOTE_FRAME, 2, d);
    break;
  }
}

uint32_t CanGen_Sent(void) { return sent_count; }

void CanGen_Stats(char *out, uint32_t size)
{
#if CAN_DNA_SERVER
  snprintf(out, size, "RX%lu REQ%lu RSP%lu", (unsigned long)stat_rx, (unsigned long)stat_req, (unsigned long)stat_rsp);
#else
  snprintf(out, size, "TX%lu", (unsigned long)sent_count);
#endif
}

#else  /* !FEATURE_CAN_TEST */

uint8_t  CanGen_Init(void)             { return 0; }
void     CanGen_Task(uint32_t now_ms)  { (void)now_ms; }
uint32_t CanGen_Sent(void)             { return 0; }
void     CanGen_Stats(char *out, uint32_t size) { if (size) out[0] = 0; }

#endif
