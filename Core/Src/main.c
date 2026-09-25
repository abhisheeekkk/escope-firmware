/* USER CODE BEGIN Header */
/**
  ******************************************************************************
  * @file           : main.c
  * @brief          : Main program body
  * @author         : abhishekshukla9586@gmail.com
  ******************************************************************************
  */
/* USER CODE END Header */
/* Includes ------------------------------------------------------------------*/
#include "main.h"
#include "burst.h"
#include "usb_device.h"

/* Private includes ----------------------------------------------------------*/
/* USER CODE BEGIN Includes */
#include "acquisition.h"
#include "usbd_cdc_if.h"
#include <stdio.h>

/* USER CODE END Includes */

/* Private typedef -----------------------------------------------------------*/
/* USER CODE BEGIN PTD */

/* USER CODE END PTD */

/* Private define ------------------------------------------------------------*/
/* USER CODE BEGIN PD */

/* USER CODE END PD */

/* Private macro -------------------------------------------------------------*/
/* USER CODE BEGIN PM */

/* USER CODE END PM */

/* Private variables ---------------------------------------------------------*/

TIM_HandleTypeDef htim2;
DMA_HandleTypeDef hdma_tim2_up;
/* Pin sweep result, sent in every burst header (reserved word):
 * bits 0-10 = PA0-PA10 that could not follow the output, bit 11 = sweep ran,
 * bits 16-31 = PB0-PB15 that could not follow. Read by the decoder. */
volatile uint32_t pin_sweep_result;

/* USER CODE BEGIN PV */
static uint32_t last_tx = 0;
static uint32_t last_blink = 0;
extern volatile uint32_t dma_irq_count;
extern uint32_t dma_half_count;
extern uint32_t dma_cplt_count;
extern uint32_t dma_isr_val;
extern uint32_t dma_state;

/* USER CODE END PV */

/* Private function prototypes -----------------------------------------------*/
void SystemClock_Config(void);
static void MPU_Config(void);
static void MX_GPIO_Init(void);
static void MX_DMA_Init(void);
static void MX_TIM2_Init(void);
static void Test_PWM_Init(void);
/* USER CODE BEGIN PFP */

/* USER CODE END PFP */

/* Private user code ---------------------------------------------------------*/
/* USER CODE BEGIN 0 */

/* USER CODE END 0 */

/**
  * @brief  The application entry point.
  * @retval int
  */
int main(void)
{

  /* USER CODE BEGIN 1 */

  /* USER CODE END 1 */

  /* MPU Configuration--------------------------------------------------------*/
  MPU_Config();

  /* MCU Configuration--------------------------------------------------------*/

  /* Reset of all peripherals, Initializes the Flash interface and the Systick. */
  SCB_EnableICache();
  HAL_Init();

  /* USER CODE BEGIN Init */

  /* USER CODE END Init */

  /* Configure the system clock */
  SystemClock_Config();

  /* USER CODE BEGIN SysInit */

  /* USER CODE END SysInit */

  /* Initialize all configured peripherals */
  MX_GPIO_Init();
  MX_DMA_Init();
  MX_USB_DEVICE_Init();
  MX_TIM2_Init();
  /* USER CODE BEGIN 2 */
  Test_PWM_Init();
  Burst_Init();
  /* Print DMA state immediately after start */
  {
    extern DMA_HandleTypeDef hdma_tim2_up;
    char dbg[128];
    int l = snprintf(dbg, sizeof(dbg),
        "DMA state before start: %d, err: %lu | pin sweep fail PA=0x%03lX PB=0x%04lX\r\n",
        (int)hdma_tim2_up.State,
        (unsigned long)hdma_tim2_up.ErrorCode,
        (unsigned long)(pin_sweep_result & 0x7FFU),
        (unsigned long)(pin_sweep_result >> 16));
    HAL_Delay(2000);  /* wait for USB to enumerate */
    CDC_Transmit_FS((uint8_t*)dbg, l);
    HAL_Delay(100);
  }
  Burst_Start();

  /* USER CODE END 2 */

  /* Infinite loop */
  /* USER CODE BEGIN WHILE */
  while (1)
  {
    /* USER CODE END WHILE */

    /* USER CODE BEGIN 3 */
    Burst_Process();
    uint32_t now = HAL_GetTick();

    if (now - last_blink >= 500) {
      last_blink = now;
      HAL_GPIO_TogglePin(LED_GPIO_Port, LED_Pin);
    }
#if 0
    if (now - last_tx >= 1000) {
      last_tx = now;
      char msg[128];
      uint8_t pd = (uint8_t)(GPIOD->IDR & 0xFF);
      int len = snprintf(msg, sizeof(msg),
        "EmbeddedScope | uptime %lums | PD=0x%02X | irq=%lu | half=%lu | cplt=%lu | isr=0x%08lX | st=%lu\r\n",
        (unsigned long)now, pd,
        (unsigned long)dma_irq_count,
        (unsigned long)dma_half_count,
        (unsigned long)dma_cplt_count,
        (unsigned long)dma_isr_val,
        (unsigned long)dma_state);
      CDC_Transmit_FS((uint8_t*)msg, (uint16_t)len);
    }
#endif
  }
  /* USER CODE END 3 */
}

/**
  * @brief System Clock Configuration
  * @retval None
  */
void SystemClock_Config(void)
{
  RCC_OscInitTypeDef RCC_OscInitStruct = {0};
  RCC_ClkInitTypeDef RCC_ClkInitStruct = {0};

  /** Supply configuration update enable
  */
  HAL_PWREx_ConfigSupply(PWR_LDO_SUPPLY);

  /** Configure the main internal regulator output voltage
  */
  __HAL_PWR_VOLTAGESCALING_CONFIG(PWR_REGULATOR_VOLTAGE_SCALE0);

  while(!__HAL_PWR_GET_FLAG(PWR_FLAG_VOSRDY)) {}

  /** Initializes the RCC Oscillators according to the specified parameters
  * in the RCC_OscInitTypeDef structure.
  */
  RCC_OscInitStruct.OscillatorType = RCC_OSCILLATORTYPE_HSI48|RCC_OSCILLATORTYPE_HSE;
  RCC_OscInitStruct.HSEState = RCC_HSE_ON;
  RCC_OscInitStruct.HSI48State = RCC_HSI48_ON;
  RCC_OscInitStruct.PLL.PLLState = RCC_PLL_ON;
  RCC_OscInitStruct.PLL.PLLSource = RCC_PLLSOURCE_HSE;
  RCC_OscInitStruct.PLL.PLLM = 1;
  RCC_OscInitStruct.PLL.PLLN = 120;
  RCC_OscInitStruct.PLL.PLLP = 2;
  RCC_OscInitStruct.PLL.PLLQ = 15;
  RCC_OscInitStruct.PLL.PLLR = 2;
  RCC_OscInitStruct.PLL.PLLRGE = RCC_PLL1VCIRANGE_3;
  RCC_OscInitStruct.PLL.PLLVCOSEL = RCC_PLL1VCOWIDE;
  RCC_OscInitStruct.PLL.PLLFRACN = 0;
  if (HAL_RCC_OscConfig(&RCC_OscInitStruct) != HAL_OK)
  {
    Error_Handler();
  }

  /** Initializes the CPU, AHB and APB buses clocks
  */
  RCC_ClkInitStruct.ClockType = RCC_CLOCKTYPE_HCLK|RCC_CLOCKTYPE_SYSCLK
                              |RCC_CLOCKTYPE_PCLK1|RCC_CLOCKTYPE_PCLK2
                              |RCC_CLOCKTYPE_D3PCLK1|RCC_CLOCKTYPE_D1PCLK1;
  RCC_ClkInitStruct.SYSCLKSource = RCC_SYSCLKSOURCE_PLLCLK;
  RCC_ClkInitStruct.SYSCLKDivider = RCC_SYSCLK_DIV1;
  RCC_ClkInitStruct.AHBCLKDivider = RCC_HCLK_DIV2;
  RCC_ClkInitStruct.APB3CLKDivider = RCC_APB3_DIV2;
  RCC_ClkInitStruct.APB1CLKDivider = RCC_APB1_DIV2;
  RCC_ClkInitStruct.APB2CLKDivider = RCC_APB2_DIV2;
  RCC_ClkInitStruct.APB4CLKDivider = RCC_APB4_DIV2;

  if (HAL_RCC_ClockConfig(&RCC_ClkInitStruct, FLASH_LATENCY_4) != HAL_OK)
  {
    Error_Handler();
  }
}

/**
  * @brief TIM2 Initialization Function
  * @param None
  * @retval None
  */
static void MX_TIM2_Init(void)
{

  /* USER CODE BEGIN TIM2_Init 0 */

  /* USER CODE END TIM2_Init 0 */

  TIM_ClockConfigTypeDef sClockSourceConfig = {0};
  TIM_MasterConfigTypeDef sMasterConfig = {0};

  /* USER CODE BEGIN TIM2_Init 1 */

  /* USER CODE END TIM2_Init 1 */
  htim2.Instance = TIM2;
  htim2.Init.Prescaler = 0;
  htim2.Init.CounterMode = TIM_COUNTERMODE_UP;
  htim2.Init.Period = 4;
  htim2.Init.ClockDivision = TIM_CLOCKDIVISION_DIV1;
  htim2.Init.AutoReloadPreload = TIM_AUTORELOAD_PRELOAD_DISABLE;
  if (HAL_TIM_Base_Init(&htim2) != HAL_OK)
  {
    Error_Handler();
  }
  sClockSourceConfig.ClockSource = TIM_CLOCKSOURCE_INTERNAL;
  if (HAL_TIM_ConfigClockSource(&htim2, &sClockSourceConfig) != HAL_OK)
  {
    Error_Handler();
  }
  sMasterConfig.MasterOutputTrigger = TIM_TRGO_UPDATE;
  sMasterConfig.MasterSlaveMode = TIM_MASTERSLAVEMODE_DISABLE;
  if (HAL_TIMEx_MasterConfigSynchronization(&htim2, &sMasterConfig) != HAL_OK)
  {
    Error_Handler();
  }
  /* USER CODE BEGIN TIM2_Init 2 */

  /* USER CODE END TIM2_Init 2 */

}

/**
  * Enable DMA controller clock
  */
static void MX_DMA_Init(void)
{

  /* DMA controller clock enable */
  __HAL_RCC_DMA1_CLK_ENABLE();

  /* I/O compensation cell: needed for correct slew control on fast outputs */
  __HAL_RCC_SYSCFG_CLK_ENABLE();
  HAL_EnableCompensationCell();

  /* DMA interrupt init */
  /* DMA1_Stream0_IRQn interrupt configuration */
  HAL_NVIC_SetPriority(DMA1_Stream0_IRQn, 0, 0);
  HAL_NVIC_EnableIRQ(DMA1_Stream0_IRQn);

}

/**
  * @brief GPIO Initialization Function
  * @param None
  * @retval None
  */
static void MX_GPIO_Init(void)
{
  GPIO_InitTypeDef GPIO_InitStruct = {0};
  /* USER CODE BEGIN MX_GPIO_Init_1 */

  /* USER CODE END MX_GPIO_Init_1 */

  /* GPIO Ports Clock Enable */
  __HAL_RCC_GPIOC_CLK_ENABLE();
  __HAL_RCC_GPIOH_CLK_ENABLE();
  __HAL_RCC_GPIOA_CLK_ENABLE();
  __HAL_RCC_GPIOD_CLK_ENABLE();

  /*Configure GPIO pin Output Level */
  HAL_GPIO_WritePin(LED_GPIO_Port, LED_Pin, GPIO_PIN_RESET);

  /*Configure GPIO pin : LED_Pin */
  GPIO_InitStruct.Pin = LED_Pin;
  GPIO_InitStruct.Mode = GPIO_MODE_OUTPUT_PP;
  GPIO_InitStruct.Pull = GPIO_NOPULL;
  GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_LOW;
  HAL_GPIO_Init(LED_GPIO_Port, &GPIO_InitStruct);

  /*Configure GPIO pins : PD0 PD1 PD2 PD3
                           PD4 PD5 PD6 PD7 */
  GPIO_InitStruct.Pin = GPIO_PIN_0|GPIO_PIN_1|GPIO_PIN_2|GPIO_PIN_3
                          |GPIO_PIN_4|GPIO_PIN_5|GPIO_PIN_6|GPIO_PIN_7;
  GPIO_InitStruct.Mode = GPIO_MODE_INPUT;
  GPIO_InitStruct.Pull = GPIO_NOPULL;
  HAL_GPIO_Init(GPIOD, &GPIO_InitStruct);

  /* USER CODE BEGIN MX_GPIO_Init_2 */

  /* USER CODE END MX_GPIO_Init_2 */
}

/* USER CODE BEGIN 4 */

/* Test signals: PB3-PB10 each output a 10 MHz PWM so they can be shorted to
 * PD0-PD7. Only a few of these pins have timer outputs (and TIM2 is the
 * sampler), so TIM3 (20 MHz) triggers DMA1_Stream1 on every update, copying
 * one word of a 2-entry table into GPIOB->BSRR (20 MHz / 2 = 10 MHz). No CPU
 * involvement, so no ISR jitter. With only 2 steps per period every channel
 * is a 50% square wave; use more steps (and a slower rate) to vary duty. */
#define TEST_PWM_STEPS 2U
/* GPIOB pin driving capture channel Dn: D0-D7 = PB3-PB10 */
static const uint8_t test_pin[8] = {3, 4, 5, 6, 7, 8, 9, 10};
static uint32_t test_pin_mask;                       /* OR of 1 << test_pin[ch] */

static const uint8_t test_duty_pct[8] = {50, 50, 50, 50, 50, 50, 50, 50};
static uint32_t test_bsrr[TEST_PWM_STEPS]
    __attribute__((section(".dma_buffers")))
    __attribute__((aligned(32)));
static DMA_HandleTypeDef hdma_tim3_up;

/* Drive each candidate pin low then high and read it back. A pin that cannot
 * follow has something else driving it (or a hard short). Pins used by USB
 * (PA11/PA12), SWD (PA13/PA14) and the PD capture inputs are not touched. */
static uint32_t Test_SweepPort(GPIO_TypeDef *port, uint32_t cand)
{
  GPIO_InitTypeDef g = {0};
  g.Pin = cand;
  g.Mode = GPIO_MODE_OUTPUT_PP;
  g.Pull = GPIO_NOPULL;
  g.Speed = GPIO_SPEED_FREQ_LOW;
  HAL_GPIO_Init(port, &g);

  port->BSRR = cand << 16;
  HAL_Delay(2);
  uint32_t stuck_high = port->IDR & cand;
  port->BSRR = cand;
  HAL_Delay(2);
  uint32_t stuck_low = ~port->IDR & cand;

  g.Mode = GPIO_MODE_INPUT;                       /* back to a quiet state */
  HAL_GPIO_Init(port, &g);
  return stuck_high | stuck_low;
}

static void Test_PinSweep(void)
{
  uint32_t fa = Test_SweepPort(GPIOA, 0x07FFU);   /* PA0-PA10 */
  uint32_t fb = Test_SweepPort(GPIOB, 0xFFFFU);   /* PB0-PB15 */
  pin_sweep_result = (fb << 16) | (1U << 11) | (fa & 0x7FFU);
}

static void Test_PWM_Init(void)
{
  GPIO_InitTypeDef gpio = {0};

  __HAL_RCC_GPIOB_CLK_ENABLE();
  __HAL_RCC_GPIOC_CLK_ENABLE();
  __HAL_RCC_TIM3_CLK_ENABLE();
  __HAL_RCC_DMA1_CLK_ENABLE();

  /* PC8: DMA-rate debug pin (toggles every CAPTURE_HALF samples) */
  GPIO_InitTypeDef dbg = {0};
  dbg.Pin = GPIO_PIN_8;
  dbg.Mode = GPIO_MODE_OUTPUT_PP;
  dbg.Speed = GPIO_SPEED_FREQ_LOW;
  HAL_GPIO_Init(GPIOC, &dbg);

  Test_PinSweep();                                /* before the pins become PWM outputs */

  for (uint32_t ch = 0; ch < 8; ch++) test_pin_mask |= 1UL << test_pin[ch];
  gpio.Pin = test_pin_mask;
  gpio.Mode = GPIO_MODE_OUTPUT_PP;
  gpio.Pull = GPIO_NOPULL;
  gpio.Speed = GPIO_SPEED_FREQ_VERY_HIGH;         /* 100 ns pulses need fast edges */
  HAL_GPIO_Init(GPIOB, &gpio);

  /* .dma_buffers is not zeroed at startup: clear the table before OR-ing bits in */
  for (uint32_t i = 0; i < TEST_PWM_STEPS; i++) test_bsrr[i] = 0;
  test_bsrr[0] = test_pin_mask;                         /* all high */
  for (uint32_t ch = 0; ch < 8; ch++) {
    uint32_t on = test_duty_pct[ch] * TEST_PWM_STEPS / 100U;
    if (on > 0 && on < TEST_PWM_STEPS) test_bsrr[on] |= 1UL << (16 + test_pin[ch]);
  }
  SCB_CleanDCache_by_Addr((uint32_t *)test_bsrr, sizeof(test_bsrr));

  hdma_tim3_up.Instance = DMA1_Stream1;
  hdma_tim3_up.Init.Request = DMA_REQUEST_TIM3_UP;
  hdma_tim3_up.Init.Direction = DMA_MEMORY_TO_PERIPH;
  hdma_tim3_up.Init.PeriphInc = DMA_PINC_DISABLE;
  hdma_tim3_up.Init.MemInc = DMA_MINC_ENABLE;
  hdma_tim3_up.Init.PeriphDataAlignment = DMA_PDATAALIGN_WORD;
  hdma_tim3_up.Init.MemDataAlignment = DMA_MDATAALIGN_WORD;
  hdma_tim3_up.Init.Mode = DMA_CIRCULAR;
  hdma_tim3_up.Init.Priority = DMA_PRIORITY_HIGH;
  hdma_tim3_up.Init.FIFOMode = DMA_FIFOMODE_DISABLE;
  if (HAL_DMA_Init(&hdma_tim3_up) != HAL_OK) Error_Handler();
  if (HAL_DMA_Start(&hdma_tim3_up, (uint32_t)test_bsrr,
                    (uint32_t)&GPIOB->BSRR, TEST_PWM_STEPS) != HAL_OK) Error_Handler();

  /* TIM3 = 240 MHz; no prescale; ARR+1 = 12 -> 20 MHz update */
  TIM3->PSC = 0;
  TIM3->ARR = 12 - 1;
  TIM3->EGR = TIM_EGR_UG;
  TIM3->SR = 0;
  TIM3->DIER = TIM_DIER_UDE;
  TIM3->CR1 = TIM_CR1_CEN;
}

/* USER CODE END 4 */

 /* MPU Configuration */

void MPU_Config(void)
{
  MPU_Region_InitTypeDef MPU_InitStruct = {0};

  /* Disables the MPU */
  HAL_MPU_Disable();

  /** Initializes and configures the Region and the memory to be protected
  */
  MPU_InitStruct.Enable = MPU_REGION_ENABLE;
  MPU_InitStruct.Number = MPU_REGION_NUMBER0;
  MPU_InitStruct.BaseAddress = 0x0;
  MPU_InitStruct.Size = MPU_REGION_SIZE_4GB;
  MPU_InitStruct.SubRegionDisable = 0x87;
  MPU_InitStruct.TypeExtField = MPU_TEX_LEVEL0;
  MPU_InitStruct.AccessPermission = MPU_REGION_NO_ACCESS;
  MPU_InitStruct.DisableExec = MPU_INSTRUCTION_ACCESS_DISABLE;
  MPU_InitStruct.IsShareable = MPU_ACCESS_SHAREABLE;
  MPU_InitStruct.IsCacheable = MPU_ACCESS_NOT_CACHEABLE;
  MPU_InitStruct.IsBufferable = MPU_ACCESS_NOT_BUFFERABLE;

  HAL_MPU_ConfigRegion(&MPU_InitStruct);
  /* Enables the MPU */
  HAL_MPU_Enable(MPU_PRIVILEGED_DEFAULT);

}

/**
  * @brief  This function is executed in case of error occurrence.
  * @retval None
  */
void Error_Handler(void)
{
  /* USER CODE BEGIN Error_Handler_Debug */
  /* User can add his own implementation to report the HAL error return state */
  __disable_irq();
  while (1)
  {
  }
  /* USER CODE END Error_Handler_Debug */
}
#ifdef USE_FULL_ASSERT
/**
  * @brief  Reports the name of the source file and the source line number
  *         where the assert_param error has occurred.
  * @param  file: pointer to the source file name
  * @param  line: assert_param error line source number
  * @retval None
  */
void assert_failed(uint8_t *file, uint32_t line)
{
  /* USER CODE BEGIN 6 */
  /* User can add his own implementation to report the file name and line number,
     ex: printf("Wrong parameters value: file %s on line %d\r\n", file, line) */
  /* USER CODE END 6 */
}
#endif /* USE_FULL_ASSERT */
