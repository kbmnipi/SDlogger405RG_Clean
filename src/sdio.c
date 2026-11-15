/* USER CODE BEGIN Header */
/**
  ******************************************************************************
  * @file    sdio.c
  * @brief   This file provides code for the configuration
  *          of the SDIO instances.
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
#include "sdio.h"

/* USER CODE BEGIN 0 */

/* USER CODE END 0 */

SD_HandleTypeDef hsd;

/* SDIO init function */

void MX_SDIO_SD_Init(void)
{

  /* USER CODE BEGIN SDIO_Init 0 */

  /* USER CODE END SDIO_Init 0 */

  /* USER CODE BEGIN SDIO_Init 1 */

  /* USER CODE END SDIO_Init 1 */
  hsd.Instance = SDIO;
  hsd.Init.ClockEdge = SDIO_CLOCK_EDGE_RISING;
  hsd.Init.ClockBypass = SDIO_CLOCK_BYPASS_DISABLE;
  hsd.Init.ClockPowerSave = SDIO_CLOCK_POWER_SAVE_DISABLE;
  hsd.Init.BusWide = SDIO_BUS_WIDE_1B;
  hsd.Init.HardwareFlowControl = SDIO_HARDWARE_FLOW_CONTROL_DISABLE;
  /* P3-2 ROLLBACK (bench, 2026-11-15): ClockDiv 2 (12 MHz) -> 4 (8 MHz).
   * SDMMC_CK = SDMMCCLK / (ClockDiv + 2) = 48 MHz / 6 = 8 MHz, with SDMMCCLK =
   * PLLQ = 48 MHz (SDlogger405RG_Clean.ioc: RCC.PLLQ = 7,
   * RCC.PLLQCLKFreq_Value = 48000000). The bus STAYS 1-bit: hsd.Init.BusWide =
   * SDIO_BUS_WIDE_1B just above, and disk_initialize() deliberately keeps the
   * HAL_SD_ConfigWideBusOperation(4B) call removed (src/diskio.c).
   *
   * WHY 12 MHz WAS ROLLED BACK - measured on the bench, not theory:
   *  - a ~2 min soak at 12 MHz ended in the graceful-shutdown path and the DATA
   *    was fine: 102 chunks, no seq gaps, size exact (256 + 102*12294 + 16 =
   *    1 254 260 B, the f_expand() preallocation + f_truncate() invariant of
   *    sd_writer.c:595-612 held). Only the LAST 16 B write - the trailer - came
   *    off the bus corrupted, so parser_vibl.py computed a CRC mismatch on an
   *    otherwise complete file. Single-shot writes to a freshly truncated tail
   *    are the most marginal traffic on this bus (first touch of those sectors
   *    after f_expand), which is exactly where a signal-integrity problem shows.
   *  - the board does not help: doc/WEACT_F405_PINOUT.md:9-14 documents an
   *    external pull-up ONLY on SDIO_CMD (PD2, "подтянут к VDD33 через 100k");
   *    D0..D3 (PC8..PC11) and CK (PC12) are listed with no external pull-up /
   *    series termination. What the firmware does provide is just the WEAK
   *    internal pull-up on the AF pins (src/sdio.c:115 and :129,
   *    GPIO_InitStruct.Pull = GPIO_PULLUP) - not a controlled-impedance
   *    termination, so at 12 MHz over uncontrolled traces the sampling window
   *    is not guaranteed. UNCERTAIN: the WeAct schematic is not in doc/, so the
   *    "no external pull-ups on D0..D3" reading comes from
   *    doc/WEACT_F405_PINOUT.md + the bench result above, not from the PCB
   *    schematic; no logic-analyser capture exists for either 12 MHz or 8 MHz.
   *
   * Why 8 MHz is the rung we keep (cited, not assumed):
   *  - the HAL's own ceiling for data transfer is 25 MHz - stm32f4xx_ll_sdmmc.h
   *    :726-727 "SDIO Data Transfer Frequency (25MHz max)", with the default
   *    SDIO_TRANSFER_CLK_DIV = 0x00 = 24 MHz; 8 MHz sits >3x below that and
   *    inside the SD 1.x default-speed window (0-25 MHz).
   *  - card identification is unaffected: HAL_SD_InitCard() first re-inits the
   *    peripheral at 400 kHz (SDIO_INIT_CLK_DIV = 0x76, stm32f4xx_hal_sd.c:414,
   *    applied at :417) and only at the end of SD_InitCard() applies hsd->Init,
   *    i.e. this ClockDiv (stm32f4xx_hal_sd.c:2752).
   *  - the pairing with the .ioc is kept: SDIO.ClockDiv=4 there, otherwise a
   *    CubeMX regeneration would silently write ClockDiv back to the old value
   *    (the Init.* assignments are OUTSIDE the USER CODE blocks).
   *
   * Effect: payload ceiling 8 Mbit/s = 1 MB/s vs 9 600 B/s needed -> 104x
   * headroom (12 MHz gave 156x, the Milestone-1 2.667 MHz gave 34.7x). Pure
   * transfer time of one 12 294 B chunk = 12294*8/8e6 = 12.3 ms (8.2 ms at
   * 12 MHz, 36.9 ms at 2.667 MHz); against the 1280 ms it takes to fill the
   * next chunk at 1600 Hz the consumer still runs ~100x ahead of the producer.
   * Step-forward ladder (doc/final_pre_flash_audit.md O-6): 8 MHz is the base;
   * 12 MHz may only be retried WITH external pull-ups/series termination and a
   * logic analyser, acceptance = 24-h soak with "Gaps Found: No" AND a trailer
   * CRC that MATCHes. If 8 MHz also corrupts, step to ClockDiv 16 (2.667 MHz). */
  hsd.Init.ClockDiv = 4;
  if (HAL_SD_Init(&hsd) != HAL_OK)
  {
    Error_Handler();
  }
  //if (HAL_SD_ConfigWideBusOperation(&hsd, SDIO_BUS_WIDE_4B) != HAL_OK)
  //{
  //  Error_Handler();
  //}
  /* USER CODE BEGIN SDIO_Init 2 */

  /* USER CODE END SDIO_Init 2 */

}

void HAL_SD_MspInit(SD_HandleTypeDef* sdHandle)
{

  GPIO_InitTypeDef GPIO_InitStruct = {0};
  if(sdHandle->Instance==SDIO)
  {
  /* USER CODE BEGIN SDIO_MspInit 0 */

  /* USER CODE END SDIO_MspInit 0 */
    /* SDIO clock enable */
    __HAL_RCC_SDIO_CLK_ENABLE();

    __HAL_RCC_GPIOC_CLK_ENABLE();
    __HAL_RCC_GPIOD_CLK_ENABLE();
    /**SDIO GPIO Configuration
    PC8     ------> SDIO_D0
    PC9     ------> SDIO_D1
    PC10     ------> SDIO_D2
    PC11     ------> SDIO_D3
    PC12     ------> SDIO_CK
    PD2     ------> SDIO_CMD
    */
    GPIO_InitStruct.Pin = GPIO_PIN_8|GPIO_PIN_9|GPIO_PIN_10|GPIO_PIN_11;
    GPIO_InitStruct.Mode = GPIO_MODE_AF_PP;
    GPIO_InitStruct.Pull = GPIO_PULLUP;
    GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_VERY_HIGH;
    GPIO_InitStruct.Alternate = GPIO_AF12_SDIO;
    HAL_GPIO_Init(GPIOC, &GPIO_InitStruct);

    GPIO_InitStruct.Pin = GPIO_PIN_12;
    GPIO_InitStruct.Mode = GPIO_MODE_AF_PP;
    GPIO_InitStruct.Pull = GPIO_NOPULL;
    GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_VERY_HIGH;
    GPIO_InitStruct.Alternate = GPIO_AF12_SDIO;
    HAL_GPIO_Init(GPIOC, &GPIO_InitStruct);

    GPIO_InitStruct.Pin = GPIO_PIN_2;
    GPIO_InitStruct.Mode = GPIO_MODE_AF_PP;
    GPIO_InitStruct.Pull = GPIO_PULLUP;
    GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_VERY_HIGH;
    GPIO_InitStruct.Alternate = GPIO_AF12_SDIO;
    HAL_GPIO_Init(GPIOD, &GPIO_InitStruct);

    /* SDIO interrupt Init */
    HAL_NVIC_SetPriority(SDIO_IRQn, 2, 0);
    HAL_NVIC_EnableIRQ(SDIO_IRQn);
  /* USER CODE BEGIN SDIO_MspInit 1 */

  /* USER CODE END SDIO_MspInit 1 */
  }
}

void HAL_SD_MspDeInit(SD_HandleTypeDef* sdHandle)
{

  if(sdHandle->Instance==SDIO)
  {
  /* USER CODE BEGIN SDIO_MspDeInit 0 */

  /* USER CODE END SDIO_MspDeInit 0 */
    /* Peripheral clock disable */
    __HAL_RCC_SDIO_CLK_DISABLE();

    /**SDIO GPIO Configuration
    PC8     ------> SDIO_D0
    PC9     ------> SDIO_D1
    PC10     ------> SDIO_D2
    PC11     ------> SDIO_D3
    PC12     ------> SDIO_CK
    PD2     ------> SDIO_CMD
    */
    HAL_GPIO_DeInit(GPIOC, GPIO_PIN_8|GPIO_PIN_9|GPIO_PIN_10|GPIO_PIN_11
                          |GPIO_PIN_12);

    HAL_GPIO_DeInit(GPIOD, GPIO_PIN_2);

    /* SDIO interrupt Deinit */
    HAL_NVIC_DisableIRQ(SDIO_IRQn);
  /* USER CODE BEGIN SDIO_MspDeInit 1 */

  /* USER CODE END SDIO_MspDeInit 1 */
  }
}

/* USER CODE BEGIN 1 */

/* USER CODE END 1 */

