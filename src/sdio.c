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
  /* TANK MODE (final clock choice, 2026-11-16): ClockDiv 2 (12 MHz) -> 16 (2.667 MHz).
   * SDMMC_CK = SDMMCCLK / (ClockDiv + 2) = 48 MHz / 18 = 2.6667 MHz, with SDMMCCLK =
   * PLLQ = 48 MHz (SDlogger405RG_Clean.ioc: RCC.PLLQ = 7,
   * RCC.PLLQCLKFreq_Value = 48000000; same math + headroom table already written up
   * in doc/MILESTONE_1_SUCCESS.md:82-89). The bus STAYS 1-bit: hsd.Init.BusWide =
   * SDIO_BUS_WIDE_1B just above, and disk_initialize() deliberately keeps the
   * HAL_SD_ConfigWideBusOperation(4B) call removed (src/diskio.c:41-44).
   *
   * WHY 2.667 MHz IS THE FINAL "TANK MODE" RUNG - the bottleneck is the card, not
   * the wires (cited, not assumed):
   *  - src/diskio.c:63-65 already states it for this project: the card-internal
   *    GC / erase-write stall "can reach ~1 s independently of the bus clock - the
   *    stall happens inside the card, not on the wires". doc/Patch_f_write.md:66
   *    quantifies it AT THIS CLOCK: one 512 B sector is on the wire for
   *    4096 bit / 2.667 MHz = 1.54 ms, so ~99.85 % of the 1 000 ms HAL bound is
   *    spent waiting on card GC/program-erase, not transferring.
   *    doc/STM32F405 WeAct SDIO FatFs Analysis.md:90 says the same in general:
   *    "bus width does not govern flash garbage collection" - bad-block management,
   *    wear leveling, ECC and GC run autonomously in the card's FTL and stall block
   *    writes through busy signaling. Defense already in the tree: 3 retries on
   *    transient FR_DISK_ERR from a GC stall (src/sd_writer.c:126-128).
   *  - so a faster bus buys throughput this logger cannot use (9 600 B/s needed vs
   *    333 kB/s available = 34.7x headroom, doc/MILESTONE_1_SUCCESS.md:87-88) and
   *    only shortens the part of the write that never fails, while raising edge
   *    rate on traces with no controlled impedance and no termination on CK (PC12).
   *    Net: added signal-integrity risk for zero usable margin.
   *  - the only rung with a clean long run on this hardware is 2.667 MHz
   *    (Milestone 1: doc/MILESTONE_1_SUCCESS.md:71-89, first artifact LOG0003.VIB).
   *    UNCERTAIN: the "~25 min without failure" duration of that first hardware
   *    test and the "50-200 ms GC latency" are user-reported bench readings;
   *    neither number appears in any file of this tree, and no DWT_CYCCNT
   *    histogram of vibl_write_chunk exists yet (that measurement is still an open
   *    item in doc/MILESTONE_2_ROBUSTNESS.md:199 - 24 h soak, P99 < 200 ms).
   *
   * ClockDiv ladder: 16 (2.667 MHz, Milestone 1) -> 2 (12 MHz, Milestone 3) -> 4
   * (8 MHz rollback, 2026-11-15) -> 2 (12 MHz re-arm after the pull-up mod,
   * 2026-11-16) -> 16 (2.667 MHz, 2026-11-16, FINAL tank-mode choice).
   *
   * WHY 12 MHz WAS ROLLED BACK ON 2026-11-15 - measured on the bench, not theory:
   *  - a ~2 min soak at 12 MHz ended in the graceful-shutdown path and the DATA
   *    was fine: 102 chunks, no seq gaps, size exact (256 + 102*12294 + 16 =
   *    1 254 260 B, the f_expand() preallocation + f_truncate() invariant of
   *    sd_writer.c:595-612 held). Only the LAST 16 B write - the trailer - came
   *    off the bus corrupted, so parser_vibl.py computed a CRC mismatch on an
   *    otherwise complete file. Single-shot writes to a freshly truncated tail
   *    are the most marginal traffic on this bus (first touch of those sectors
   *    after f_expand), which is exactly where a signal-integrity problem shows.
   *  - the board did not help back then: doc/WEACT_F405_PINOUT.md:9-14 documented an
   *    external pull-up ONLY on SDIO_CMD (PD2, "подтянут к VDD33 через 100k");
   *    D0..D3 (PC8..PC11) and CK (PC12) are listed with no external pull-up / series
   *    termination; all the firmware had was the WEAK internal GPIO_PULLUP in
   *    HAL_SD_MspInit (PC8..PC11 and PD2) - not a controlled-impedance
   *    termination, so the sampling window at 12 MHz over uncontrolled traces
   *    was not guaranteed.
   *  - WHAT CHANGED (bench mod, 2026-11-16, user-reported): 33.2 kOhm external
   *    pull-ups were soldered on SDIO_CMD (PD2) and SDIO_D0 (PC8). In 1-bit
   *    mode those are the only two lines that carry traffic, so this was expected
   *    to fix the weak-edge weakness that corrupted the 16 B trailer. THE MOD DID
   *    NOT RESCUE 12 MHz: the post-mod 12 MHz run and the 8 MHz rung both still
   *    failed (user-reported, attributed to card GC stalls plus residual
   *    signal-integrity problems), which is why the ladder now bottoms out at
   *    2.667 MHz instead of stepping back up through 8 MHz.
   *    NOTE (rule 15, file authority): this mod supersedes the "external
   *    pull-up on SDIO_CMD only, nothing on SDIO_D0" reading of
   *    doc/WEACT_F405_PINOUT.md:9-14 for PD2 and PC8; the pinout doc itself was
   *    NOT re-measured or updated by this patch and still lists the original
   *    100k on CMD and no pull-up on D0..D3/CK.
   *    UNCERTAIN: the WeAct schematic is not in doc/, PC12 (CK) and PC9..PC11
   *    (D1..D3) are still unmodded, 33.2 k is a pull-up (idle-state / bus-free
   *    help), not series termination, and no logic-analyser capture exists for
   *    either 12 MHz or 8 MHz.
   *
   * Why 2.667 MHz is the rung we keep (cited, not assumed):
   *  - it sits 9.4x below the HAL's own transfer ceiling - stm32f4xx_ll_sdmmc.h
   *    :726-727 "SDIO Data Transfer Frequency (25MHz max)", default
   *    SDIO_TRANSFER_CLK_DIV = 0x00 = 24 MHz - and deep inside the SD 1.x
   *    default-speed window (0-25 MHz). Bit period = 1/2.6667 MHz = 375 ns vs
   *    83 ns at 12 MHz: a 4.5x wider setup/hold sampling window on traces that
   *    have no controlled impedance and no termination on CK (PC12).
   *  - card identification is unaffected: HAL_SD_InitCard() first re-inits the
   *    peripheral at 400 kHz (SDIO_INIT_CLK_DIV = 0x76, stm32f4xx_hal_sd.c:414,
   *    applied at :417) and only at the end of SD_InitCard() applies hsd->Init,
   *    i.e. this ClockDiv (stm32f4xx_hal_sd.c:2752).
   *  - the pairing with the .ioc is kept: SDIO.ClockDiv=16 there, otherwise a
   *    CubeMX regeneration would silently write ClockDiv back to the old value
   *    (the Init.* assignments are OUTSIDE the USER CODE blocks).
   *
   * Effect: payload ceiling 2.6667 Mbit/s = 333 kB/s vs 9 600 B/s needed -> 34.7x
   * headroom (12 MHz gave 156x, 8 MHz 104x - throughput this logger never asks
   * for; doc/MILESTONE_1_SUCCESS.md:87-88). Pure transfer time of one 12 294 B
   * chunk = 12294*8/2.6667e6 = 36.9 ms (12.3 ms at 8 MHz, 8.2 ms at 12 MHz);
   * against the 1280 ms it takes to fill the next chunk at 1600 Hz the consumer
   * still runs ~35x ahead of the producer, and the chunk eats only 3.7 % of the
   * 1 000 ms per-op HAL bound (src/diskio.c:71, :92), itself 4.1x under
   * T_IWDG = 4.095 s (src/iwdg.c) - GC pauses stay absorbable.
   * ACCEPTANCE GATE (doc/final_pre_flash_audit.md O-6; doc/MILESTONE_2_ROBUSTNESS.md:199):
   * a 24-h soak at this clock must end with "Gaps Found: No" AND a trailer CRC
   * that MATCHes, with DWT_CYCCNT around vibl_write_chunk to measure the real GC
   * stall distribution (target P99 < 200 ms). If 2.667 MHz corrupts too, the bus
   * is NOT the cause: stop tuning ClockDiv and attack the medium instead
   * (industrial SLC/pSLC card, doc/STM32F405 WeAct SDIO FatFs Analysis.md:90, 98-102). */
  hsd.Init.ClockDiv = 16;
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

