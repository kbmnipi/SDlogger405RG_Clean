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
  /* P3-2 (Milestone 3): ClockDiv 16 -> 2.
   * SDMMC_CK = SDMMCCLK / (ClockDiv + 2) = 48 MHz / 4 = 12 MHz, with SDMMCCLK =
   * PLLQ = 48 MHz (SDlogger405RG_Clean.ioc: RCC.PLLQ = 7,
   * RCC.PLLQCLKFreq_Value = 48000000). The bus STAYS 1-bit: hsd.Init.BusWide =
   * SDIO_BUS_WIDE_1B just above, and disk_initialize() deliberately keeps the
   * HAL_SD_ConfigWideBusOperation(4B) call removed (src/diskio.c).
   *
   * Why 12 MHz is safe here (cited, not assumed):
   *  - the HAL's own ceiling for data transfer is 25 MHz - stm32f4xx_ll_sdmmc.h
   *    :726-727 "SDIO Data Transfer Frequency (25MHz max)", with the default
   *    SDIO_TRANSFER_CLK_DIV = 0x00 = 24 MHz; 12 MHz is a 2x margin to that, and
   *    12 MHz is also the SD 1.x default-speed timing limit (0-25 MHz).
   *  - card identification is unaffected: HAL_SD_InitCard() first re-inits the
   *    peripheral at 400 kHz (SDIO_INIT_CLK_DIV = 0x76, stm32f4xx_hal_sd.c:414,
   *    applied at :417) and only at the end of SD_InitCard() applies hsd->Init,
   *    i.e. this ClockDiv (stm32f4xx_hal_sd.c:2752).
   *  - the pairing with the .ioc is kept: SDIO.ClockDiv=2 there, otherwise a
   *    CubeMX regeneration would silently write ClockDiv back to the old value
   *    (the Init.* assignments are OUTSIDE the USER CODE blocks).
   *
   * Effect: payload ceiling 12 Mbit/s = 1.5 MB/s vs 9 600 B/s needed -> 156x
   * headroom (was 34.7x). Pure transfer time of one 12 294 B chunk drops from
   * 12294*8/2.6667e6 = 36.9 ms to 12294*8/12e6 = 8.2 ms (4.5x). Shorter bus
   * occupancy also shrinks the window in which the card's internal
   * program/erase stall (GC) can collide with the next command.
   * UNCERTAIN (hardware): 12 MHz on this board's soldered 4-bit routing
   * (doc/WEACT_F405_PINOUT.md sec.1: PC8..PC11, PC12=CK, PD2=CMD) has not been
   * probed with a logic analyser. Step-back ladder if CRC/data errors appear:
   * ClockDiv 4 (8 MHz), then 16 (2.667 MHz, the Milestone-1 value). */
  hsd.Init.ClockDiv = 2;
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

