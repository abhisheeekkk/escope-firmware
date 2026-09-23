#ifndef __ACQUISITION_H
#define __ACQUISITION_H
#ifdef __cplusplus
extern "C" {
#endif

#include "stm32h7xx_hal.h"

void Acquisition_Init(void);
void Acquisition_Start(void);
void Acquisition_Stop(void);
void Acquisition_Process(void);

extern TIM_HandleTypeDef htim1;
extern uint8_t acq_error;

#ifdef __cplusplus
}
#endif
#endif
