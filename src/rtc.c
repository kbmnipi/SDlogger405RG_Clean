/* USER CODE BEGIN Header */
/**
  ******************************************************************************
  * @file    rtc.c
  * @brief   This file provides code for the configuration
  *          of the RTC instances.
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
#include "rtc.h"

/* USER CODE BEGIN 0 */
/*---------------------------------------------------------------------------*/
/* O-2 FIX (doc/MILESTONE_1_SUCCESS.md §5, also final_pre_flash_audit.md W-6): */
/* MX_RTC_Init() only programs the prescalers (AsynchPrediv=127,               */
/* SynchPrediv=255, LSE via HAL_RTC_MspInit) and NEVER writes the calendar,    */
/* so RTC_DR/RTC_TR keep their power-on values and vibl_rtc_ms() produces a    */
/* bogus epoch (sd_writer.c: year = 2000u + sDate.Year). This helper stamps a  */
/* fixed default so a fresh board logs with a plausible UTC timestamp.         */
/*                                                                            */
/* TEMPORARY HARDCODE — to be replaced by a host-side time sync (SWD write to */
/* RTC_TR/RTC_DR, or a `--set-rtc` UART command) per open item O-9. Until then */
/* every power-on re-stamps the SAME constant: with VBAT/super-cap retention   */
/* the RTC keeps running between boots, so the re-stamp DISCARDS the elapsed   */
/* time. Deterministic (a wrong date is always exactly this one) but NOT a     */
/* substitute for a real time source.                                         */
/*                                                                            */
/* Calendar values are plain binary here; HAL_RTC_SetTime/SetDate convert them */
/* to BCD internally (RTC_ByteToBcd2, stm32f4xx_hal_rtc.c). Note that RTC     */
/* stores the YEAR AS AN OFFSET FROM 2000 -> Year = 26 means 2026, which is    */
/* what sd_writer.c:189 and diskio.c:189 ((Year + 20) << 25 == year - 1980)    */
/* expect. F4 HAL has SEPARATE RTC_TimeTypeDef / RTC_DateTypeDef structs —     */
/* RTC_DateTimeTypeDef DOES NOT exist in STM32F4 HAL (RM: do not invent it).   */
/*                                                                            */
/* Backup-domain write access (PWR_CR_DBP) is already granted by               */
/* HAL_RCCEx_PeriphCLKConfig() inside HAL_RTC_MspInit (stm32f4xx_hal_rcc_ex.c  */
/* sets __HAL_RCC_PWR_CLK_ENABLE() then PWR->CR |= PWR_CR_DBP), which runs     */
/* inside HAL_RTC_Init() -> so it is in place before this helper is called.    */
/*---------------------------------------------------------------------------*/
#define RTC_DEFAULT_YEAR   26u   /* 2000 + 26 = 2026 (RTC Year is an offset from 2000) */
#define RTC_DEFAULT_MONTH  11u   /* November; RTC_MONTH_NOVEMBER is the BCD 0x11 alias */
#define RTC_DEFAULT_DATE   15u
#define RTC_DEFAULT_HOUR   12u
#define RTC_DEFAULT_MIN    0u
#define RTC_DEFAULT_SEC    0u

/* Debug observability (same style as vibl_dbg_* in sd_writer.c): read this in
 * the debugger watch window. 1 = calendar stamped OK, 0 = SetTime/SetDate
 * failed (typical cause: LSE not running -> RTC_EnterInitMode() times out after
 * RTC_TIMEOUT_VALUE = 1000 ms, stm32f4xx_hal_rtc.h:792). */
static volatile int rtc_stamp_ok = 0;

static void Set_RTC_Time(void)
{
  RTC_TimeTypeDef sTime;
  RTC_DateTypeDef sDate;

  /* --- Time: RTC_FORMAT_BIN, 24-hour mode (HourFormat = RTC_HOURFORMAT_24) ---
   * With 24h format HAL_RTC_SetTime() forces sTime.TimeFormat = 0x00 itself,
   * and DayLightSaving/StoreOperation are deprecated: NONE/RESET (== 0) keeps
   * RTC_CR BKP/SUB1H/ADD1H clear. */
  sTime.Hours          = (uint8_t)RTC_DEFAULT_HOUR;
  sTime.Minutes        = (uint8_t)RTC_DEFAULT_MIN;
  sTime.Seconds        = (uint8_t)RTC_DEFAULT_SEC;
  sTime.TimeFormat     = 0x00u;                    /* unused in 24h mode */
  sTime.DayLightSaving = RTC_DAYLIGHTSAVING_NONE;  /* 0x00000000U */
  sTime.StoreOperation = RTC_STOREOPERATION_RESET; /* 0x00000000U */

  /* --- Date: RTC_FORMAT_BIN ---
   * HAL_RTC_SetDate() re-bases the month when BIN and (Month & 0x10) is set
   * (stm32f4xx_hal_rtc.c:878), so RTC_MONTH_NOVEMBER (0x11) and plain 11 both
   * end up as BCD 0x11 in RTC_DR. 2026-11-15 is a SUNDAY (verified on host). */
  sDate.WeekDay = RTC_WEEKDAY_SUNDAY;
  sDate.Month   = RTC_MONTH_NOVEMBER;   /* 11 */
  sDate.Date    = (uint8_t)RTC_DEFAULT_DATE;
  sDate.Year    = (uint8_t)RTC_DEFAULT_YEAR;

  /* HAL order matters: HAL_RTC_SetTime() before HAL_RTC_SetDate() (the alarm
   * APIs document "SetTime() must be called before enabling the Alarm", and
   * both enter RTC init mode independently). */
  if (HAL_RTC_SetTime(&hrtc, &sTime, RTC_FORMAT_BIN) != HAL_OK)
  {
    rtc_stamp_ok = 0;
    return;
  }
  if (HAL_RTC_SetDate(&hrtc, &sDate, RTC_FORMAT_BIN) != HAL_OK)
  {
    rtc_stamp_ok = 0;
    return;
  }
  rtc_stamp_ok = 1;

  /* NOT Error_Handler(): a failed calendar stamp must not hang the boot in
   * while(1) with IRQs disabled (Error_Handler, main.c:368) while the IWDG
   * (already started by MX_IWDG_Init, main.c:118) resets the board forever.
   * The logger's existing guard (main.c:165 `vibl_get_start_ts() == 0u` ->
   * hil3_error_loop(6)) stays the single place that reports a clock fault.
   * KNOWN RESIDUAL GAP (documented in O-2): if the LSE is dead, TR/DR keep
   * their reset values, vibl_rtc_ms() returns a NON-ZERO epoch-2000 stamp, and
   * that guard cannot detect it. Sentinel check (`Year == 0 && time == 0`) is
   * the follow-up; deliberately not added here (scope: stamp the clock only). */
}
/* USER CODE END 0 */

RTC_HandleTypeDef hrtc;

/* RTC init function */
void MX_RTC_Init(void)
{

  /* USER CODE BEGIN RTC_Init 0 */

  /* USER CODE END RTC_Init 0 */

  /* USER CODE BEGIN RTC_Init 1 */

  /* USER CODE END RTC_Init 1 */

  /** Initialize RTC Only
  */
  hrtc.Instance = RTC;
  hrtc.Init.HourFormat = RTC_HOURFORMAT_24;
  hrtc.Init.AsynchPrediv = 127;
  hrtc.Init.SynchPrediv = 255;
  hrtc.Init.OutPut = RTC_OUTPUT_DISABLE;
  hrtc.Init.OutPutPolarity = RTC_OUTPUT_POLARITY_HIGH;
  hrtc.Init.OutPutType = RTC_OUTPUT_TYPE_OPENDRAIN;
  if (HAL_RTC_Init(&hrtc) != HAL_OK)
  {
    Error_Handler();
  }
    /* USER CODE BEGIN RTC_Init 2 */
  /* O-2 FIX: stamp the calendar (prescalers alone leave TR/DR at their reset
   * values -> header.start_ts reads as 2000-01-01). TEMPORARY hardcode until a
   * host-side time sync over SWD/UART is implemented (open item O-9).
   * Cost: 2 RTC init-mode entries; each waits for INITS via RTC_ExitInitMode()
   * -> 2 x <= RTC_TIMEOUT_VALUE = 1000 ms in the worst case (LSE dead),
   * 0 ms steady state once INITS is set (HAL_RTC_Init skips re-init).
   * Boot budget: MX_IWDG_Init already ran (main.c:118), T_IWDG = 4.095 s
   * (iwdg.c P0-4) -> worst case 2.000 s still fits inside the watchdog window,
   * but a real failure needs the LSE, so this is not a place to add retries. */
  Set_RTC_Time();
  /* USER CODE END RTC_Init 2 */

}

void HAL_RTC_MspInit(RTC_HandleTypeDef* rtcHandle)
{

  RCC_PeriphCLKInitTypeDef PeriphClkInitStruct = {0};
  if(rtcHandle->Instance==RTC)
  {
  /* USER CODE BEGIN RTC_MspInit 0 */

  /* USER CODE END RTC_MspInit 0 */

  /** Initializes the peripherals clock
  */
    PeriphClkInitStruct.PeriphClockSelection = RCC_PERIPHCLK_RTC;
    PeriphClkInitStruct.RTCClockSelection = RCC_RTCCLKSOURCE_LSE;
    if (HAL_RCCEx_PeriphCLKConfig(&PeriphClkInitStruct) != HAL_OK)
    {
      Error_Handler();
    }

    /* RTC clock enable */
    __HAL_RCC_RTC_ENABLE();
  /* USER CODE BEGIN RTC_MspInit 1 */

  /* USER CODE END RTC_MspInit 1 */
  }
}

void HAL_RTC_MspDeInit(RTC_HandleTypeDef* rtcHandle)
{

  if(rtcHandle->Instance==RTC)
  {
  /* USER CODE BEGIN RTC_MspDeInit 0 */

  /* USER CODE END RTC_MspDeInit 0 */
    /* Peripheral clock disable */
    __HAL_RCC_RTC_DISABLE();
  /* USER CODE BEGIN RTC_MspDeInit 1 */

  /* USER CODE END RTC_MspDeInit 1 */
  }
}

/* USER CODE BEGIN 1 */

/* USER CODE END 1 */

