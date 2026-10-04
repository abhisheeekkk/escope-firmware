#ifndef __BURST_H
#define __BURST_H
#ifdef __cplusplus
extern "C" {
#endif

#include "stm32h7xx_hal.h"

/* Burst capture: 8 channels at 48 MS/s into a 256 KB ring (5.5 ms), edge
 * trigger, then the ~4.7 ms window around the trigger is uploaded over USB. */
void Burst_Init(void);
void Burst_Start(void);
void Burst_Process(void);
/* Feed bytes received from the PC (trigger configuration commands). */
void Burst_Config_Rx(const uint8_t *data, uint32_t len);

extern volatile uint32_t burst_count;   /* frames uploaded */
extern volatile uint32_t burst_auto;    /* frames that were auto-triggered */

#ifdef __cplusplus
}
#endif
#endif
