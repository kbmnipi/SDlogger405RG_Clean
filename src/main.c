/* USER CODE BEGIN Header */
/**
  ******************************************************************************
  * @file           : main.c
  * @brief          : Main program body
  ******************************************************************************
  * @attention
  *
  * Copyright (c) 2026 STMicroelectronics.
  * All rights reserved.
  *
  * This software is licensed under terms that can be found in the LICENSE file
  * in the root directory of this software component.
  * If no LICENSE file comes with this software, it is provided AS-IS.
  *
  ******************************************************************************
  */
/* USER CODE END Header */
/* Includes ------------------------------------------------------------------*/
#include "main.h"
#include "iwdg.h"
#include "rtc.h"
#include "sdio.h"
#include "spi.h"
#include "tim.h"
#include "gpio.h"

/* Private includes ----------------------------------------------------------*/
/* USER CODE BEGIN Includes */
#include "lis3dsh.h"
#include "sd_writer.h"
#include "ff.h"
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

/* USER CODE BEGIN PV */
FATFS fs;

/* P1-6 (audit_13_11_26_1 WARN-1): mask-based ping-pong instead of the coded
 * ready-flag (old "ready = active buffer + 1" scheme). With that scheme a late
 * consumer lost a pending flag silently (and read-then-clear after the flash
 * could drop an arena that matured during the flush), so lost chunks left no
 * trace in the file. A bitmask keeps BOTH matured arenas pending until
 * consumed, and a collision is counted in `dropped` -> vibl_mark_gap(). */
#define VIBL_CHUNK_WORDS  (VIBL_SAMPLES_PER_CHUNK * 3u)
static int16_t arena[2][VIBL_CHUNK_WORDS];
static volatile uint8_t  fill_idx  = 0u;   /* 0 or 1 — which arena ISR writes into */
static volatile uint32_t fill_cnt  = 0u;   /* words filled in current arena        */
static volatile uint8_t  full_mask = 0u;   /* bit0 = arena[0] full, bit1 = arena[1] */
static volatile uint32_t dropped   = 0u;   /* accumulated dropped samples          */
static volatile uint8_t lis3dsh_err = 0u;
/* P2-8: consecutive SPI failures (see the ISR error branch). */
static volatile uint32_t spi_err_streak = 0u;
/* P1-5 (audit_13_11_26_1 BUG-5): set by HAL_GPIO_EXTI_Callback() on PC13.
 * PC13 is Active HIGH (doc/WEACT_F405_PINOUT.md §1 note 1 / §2), configured in
 * gpio.c as GPIO_MODE_IT_RISING + GPIO_PULLDOWN, so a press is a rising edge. */
static volatile uint8_t session_stop = 0u;

static void hil3_error_loop(uint8_t err_code);
/* USER CODE END PV */

/* Private function prototypes -----------------------------------------------*/
void SystemClock_Config(void);
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

  /* MCU Configuration--------------------------------------------------------*/

  /* Reset of all peripherals, Initializes the Flash interface and the Systick. */
  HAL_Init();

  /* USER CODE BEGIN Init */

  /* USER CODE END Init */

  /* Configure the system clock */
  SystemClock_Config();

  /* USER CODE BEGIN SysInit */

  /* USER CODE END SysInit */

  /* Initialize all configured peripherals */
  MX_GPIO_Init();
  MX_IWDG_Init();
  MX_RTC_Init();
  /* USER CODE BEGIN PreSDIO */
    /* P0-4 (audit_13_11_26_1 BUG-4): CD gate BEFORE the blocking HAL_SD_Init inside
   * MX_SDIO_SD_Init(). Without this gate a dead/absent card makes HAL_SD_Init
   * outlive T_IWDG and the error-blink loop is unreachable.
   * T_IWDG is now 4.095 s (iwdg.c P0-4), so hil3_error_loop(2) can blink freely.
   *
   * ACTUAL hardware on this board: card inserted -> PA8 HIGH (NC switch,
   * R11 = 100 k pull-up to VDD33, R12 = 10 k in series with PA8). Bench:
   * inserted = 2.5 V, absent = 0.4 V. doc/WEACT_F405_PINOUT.md §1 claimed the
   * opposite (active LOW) — corrected after bench, SSOT updated in the same patch. */
  if (HAL_GPIO_ReadPin(SD_DETECT_GPIO_Port, SD_DETECT_Pin) == GPIO_PIN_RESET) {
    hil3_error_loop(2);  /* PA8 LOW = card NOT inserted (NC switch closes to GND) */
  }
  /* USER CODE END PreSDIO */
  MX_SDIO_SD_Init();
  MX_SPI1_Init();
  MX_TIM2_Init();
    /* USER CODE BEGIN 2 */
  /* P1-5: NVIC enable for the shared EXTI15_10 line (PC13 = EXTI13).
   * CubeMX left this out (SDlogger405RG_Clean.ioc has SH.GPXTI13.0=GPIO_EXTI13
   * but no NVIC.EXTI15_10_IRQn entry, and gpio.c issues no HAL_NVIC_EnableIRQ),
   * so without this the new EXTI15_10_IRQHandler never fires. */
  HAL_NVIC_SetPriority(EXTI15_10_IRQn, 3u, 0u);  /* lowest: must not delay TIM2 (1) */
  HAL_NVIC_EnableIRQ(EXTI15_10_IRQn);
  __HAL_GPIO_EXTI_CLEAR_IT(BUTTON_Pin);          /* drop any spurious pending edge */

  /* 1. Init Sensor */
  if (lis3dsh_init() != 0) {
    hil3_error_loop(1);
  }

  /* 2. Mount SD */
  if (f_mount(&fs, "0:", 1) != FR_OK) {
    hil3_error_loop(2);
  }

    /* 3. Open VIBL file */
  if (vibl_open("LOG0003.VIB", VIBL_SENSOR_LIS3DSH, 16u, 100u) != VIBL_OK) {
    hil3_error_loop(3);
  }

  /* P2-9 (audit WARN-4): the file was created successfully, but if the RTC was
   * unreadable at open time both header.start_ts and every FAT timestamp are
   * meaningless (the parser can only print "Unknown"). Fail loudly instead of
   * logging untimed data. Error code 6 = RTC fault. */
  if (vibl_get_start_ts() == 0u) {
    hil3_error_loop(6);   /* new error code: RTC fault (LSE not running?) */
  }


  /* 4. Start Timer */
  HAL_TIM_Base_Start_IT(&htim2);
  /* USER CODE END 2 */

    /* Infinite loop */
  /* USER CODE BEGIN WHILE */
  while (1)
  {
    /* USER CODE END WHILE */

    /* USER CODE BEGIN 3 */
    /* P1-5 (audit_13_11_26_1 BUG-5): graceful stop requested by PC13.
     * Drain whatever is pending, commit header/trailer via vibl_close(),
     * unmount, stop the sampling timer and park with the LED steady ON
     * (PB2 Active HIGH per doc/WEACT_F405_PINOUT.md sec.2 note 3). */
    if (session_stop) {
      /* DEVIATION (order only): the spec stops TIM2 after vibl_close(). With
       * P1-7 preallocation, vibl_close() now runs remove_chain() over the
       * never-written tail (~10k FAT entries at 4k clusters = 0.3-0.5 s), so
       * stopping the 1600 Hz sampler FIRST removes ISR jitter from that window
       * and prevents new full_mask/dropped updates after the snapshot below. */
      HAL_TIM_Base_Stop_IT(&htim2);

      /* Drain both arenas if any are full (P1-5 + P1-6). */
      __disable_irq();
      uint8_t  sm = full_mask;
      uint32_t sd = dropped;
      full_mask = 0u;
      dropped   = 0u;
      __enable_irq();
      if (sm & 1u) (void)vibl_write_chunk(arena[0], VIBL_SAMPLES_PER_CHUNK);
      if (sm & 2u) (void)vibl_write_chunk(arena[1], VIBL_SAMPLES_PER_CHUNK);
      if (sd) { (void)vibl_mark_gap(sd); }

      if (vibl_close() != VIBL_OK) hil3_error_loop(5);

      /* DEVIATION from the patch spec ("f_close(&fs)"): in FatFs R0.12c
       * f_close() takes FIL* (ff.h:243); passing FATFS* would make f_sync()
       * write through a wrong object layout. The R0.12c unmount call is
       * f_mount(NULL, path, 0) - see ff.c:3277-3293 (FatFs[vol] = 0). */
      (void)f_mount(NULL, "0:", 0);
      HAL_GPIO_WritePin(Led_GPIO_Port, Led_Pin, GPIO_PIN_SET);   /* clean stop */
      while (1) {
        HAL_IWDG_Refresh(&hiwdg);
        __WFI();
      }
    }

    if (lis3dsh_err != 0u) hil3_error_loop(4);

    /* P1-6 (audit WARN-1): atomic snapshot+clear of the arena mask and of the
     * dropped counter. Critical section = 2 loads + 2 stores + cpsr writes
     * = ~12 instructions < 100 ns at 168 MHz, so jitter of the 1600 Hz ISR
     * (period 625 us) stays negligible. Both matured arenas are flushed
     * back-to-back. Because `d` is snapshotted in the SAME critical section,
     * an increment landing during the flush below is carried to the next
     * iteration instead of being erased (closes the audit read-then-clear
     * window). */
    __disable_irq();
    uint8_t  m = full_mask;
    uint32_t d = dropped;
    full_mask = 0u;
    dropped   = 0u;
    __enable_irq();

    if (m & 1u) {
      if (vibl_write_chunk(arena[0], VIBL_SAMPLES_PER_CHUNK) != VIBL_OK) hil3_error_loop(5);
    }
    if (m & 2u) {
      if (vibl_write_chunk(arena[1], VIBL_SAMPLES_PER_CHUNK) != VIBL_OK) hil3_error_loop(5);
    }
    if (d) { (void)vibl_mark_gap(d); }

        HAL_IWDG_Refresh(&hiwdg);
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

  /** Configure the main internal regulator output voltage
  */
  __HAL_RCC_PWR_CLK_ENABLE();
  __HAL_PWR_VOLTAGESCALING_CONFIG(PWR_REGULATOR_VOLTAGE_SCALE1);

  /** Initializes the RCC Oscillators according to the specified parameters
  * in the RCC_OscInitTypeDef structure.
  */
  RCC_OscInitStruct.OscillatorType = RCC_OSCILLATORTYPE_LSI|RCC_OSCILLATORTYPE_HSE
                              |RCC_OSCILLATORTYPE_LSE;
  RCC_OscInitStruct.HSEState = RCC_HSE_ON;
  RCC_OscInitStruct.LSEState = RCC_LSE_ON;
  RCC_OscInitStruct.LSIState = RCC_LSI_ON;
  RCC_OscInitStruct.PLL.PLLState = RCC_PLL_ON;
  RCC_OscInitStruct.PLL.PLLSource = RCC_PLLSOURCE_HSE;
  RCC_OscInitStruct.PLL.PLLM = 4;
  RCC_OscInitStruct.PLL.PLLN = 168;
  RCC_OscInitStruct.PLL.PLLP = RCC_PLLP_DIV2;
  RCC_OscInitStruct.PLL.PLLQ = 7;
  if (HAL_RCC_OscConfig(&RCC_OscInitStruct) != HAL_OK)
  {
    Error_Handler();
  }

  /** Initializes the CPU, AHB and APB buses clocks
  */
  RCC_ClkInitStruct.ClockType = RCC_CLOCKTYPE_HCLK|RCC_CLOCKTYPE_SYSCLK
                              |RCC_CLOCKTYPE_PCLK1|RCC_CLOCKTYPE_PCLK2;
  RCC_ClkInitStruct.SYSCLKSource = RCC_SYSCLKSOURCE_PLLCLK;
  RCC_ClkInitStruct.AHBCLKDivider = RCC_SYSCLK_DIV1;
  RCC_ClkInitStruct.APB1CLKDivider = RCC_HCLK_DIV4;
  RCC_ClkInitStruct.APB2CLKDivider = RCC_HCLK_DIV2;

  if (HAL_RCC_ClockConfig(&RCC_ClkInitStruct, FLASH_LATENCY_5) != HAL_OK)
  {
    Error_Handler();
  }
}

/* USER CODE BEGIN 4 */
/* P1-5: strong override of the HAL weak callback (stm32f4xx_hal_gpio.c).
 * Called from EXTI15_10_IRQHandler -> HAL_GPIO_EXTI_IRQHandler(BUTTON_Pin).
 * Only sets a flag; all SD/file work happens in the main loop. */
void HAL_GPIO_EXTI_Callback(uint16_t GPIO_Pin)
{
    if (GPIO_Pin == BUTTON_Pin) {
        session_stop = 1u;
    }
}

void HAL_TIM_PeriodElapsedCallback(TIM_HandleTypeDef *htim)
{
    if (htim->Instance != TIM2) return;

    int16_t *b = arena[fill_idx];

    if (lis3dsh_read(&b[fill_cnt], &b[fill_cnt + 1], &b[fill_cnt + 2]) != 0) {
        /* P1-6: burn the slot so the arena stays aligned. Returning without
         * advancing would re-read into the SAME slot, i.e. the last good
         * sample repeats 1600x/s while seq / total_records keep advancing -
         * an invisible corruption (audit WARN-2).
         * P2-8: escalate to the error loop only after 10 consecutive failures
         * (~6.3 ms of dead bus). A one-off glitch costs a single sample and is
         * NOT counted into `dropped`, because a partially invalid arena cannot
         * be reported as a clean gap without a new VIBL record type; closing
         * that properly needs an invalid-sample flag in the record format. */
        fill_cnt += 3u;
        spi_err_streak++;
        if (spi_err_streak >= 10u) {
            lis3dsh_err = 1u;   /* -> hil3_error_loop(4) in the main loop */
        }
        return;
    }
    spi_err_streak = 0u;        /* P2-8: one good frame clears the streak */

    fill_cnt += 3u;
    if (fill_cnt >= VIBL_CHUNK_WORDS) {
        if (full_mask & (uint8_t)(1u << fill_idx)) {
            dropped += VIBL_SAMPLES_PER_CHUNK;  /* consumer too slow (SD GC) */
        } else {
            full_mask |= (uint8_t)(1u << fill_idx);
        }
        fill_idx ^= 1u;
        fill_cnt = 0u;
    }
}


static void hil3_error_loop(uint8_t err_code)
{
    while (1) {
        for (uint8_t i = 0; i < err_code; i++) {
            HAL_GPIO_WritePin(GPIOB, GPIO_PIN_2, GPIO_PIN_SET);
            HAL_Delay(200);
            HAL_IWDG_Refresh(&hiwdg);
            HAL_GPIO_WritePin(GPIOB, GPIO_PIN_2, GPIO_PIN_RESET);
            HAL_Delay(200);
            HAL_IWDG_Refresh(&hiwdg);
        }
        for (int i = 0; i < 10; i++) {
            HAL_Delay(100);
            HAL_IWDG_Refresh(&hiwdg);
        }
    }
}
/* USER CODE END 4 */

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
