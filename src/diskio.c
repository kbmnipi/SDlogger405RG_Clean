/**
  ******************************************************************************
  * @file    diskio.c
  * @brief   FatFs low-level disk I/O driver (HAL_SD backend).
  ******************************************************************************
  */

#include "ff.h"      /* FatFs public definitions */
#include "diskio.h"  /* FatFs disk I/O definitions */
#include "sdio.h"    /* extern SD_HandleTypeDef hsd;   */
#include "iwdg.h"    /* extern IWDG_HandleTypeDef hiwdg; */
#include "rtc.h"     /* P2-9: extern RTC_HandleTypeDef hrtc; + HAL RTC API */


/*---------------------------------------------------------------------------*/
/* FatFs disk status                                                         */
/*---------------------------------------------------------------------------*/
DSTATUS disk_status(BYTE pdrv)
{
    (void)pdrv; /* Only one volume is supported. */

        /* ACTUAL hardware: card inserted -> PA8 HIGH (NC switch, R11 = 100 k
     * pull-up to VDD33, R12 = 10 k in series). Bench: inserted = 2.5 V,
     * absent = 0.4 V. doc/WEACT_F405_PINOUT.md §1 claimed the opposite —
     * corrected after bench, SSOT updated in the same patch. */
    if ((GPIOA->IDR & GPIO_PIN_8) != 0U) return 0;        /* HIGH = card present (SSOT doc/WEACT_F405_PINOUT.md §1: inserted=PA8 HIGH; matches main.c:130 CD-gate) */
        return STA_NOINIT;                                     /* LOW  = card absent  (NC switch closes PA8 to GND) */
}

/*---------------------------------------------------------------------------*/
/* FatFs disk initialization                                                 */
/*---------------------------------------------------------------------------*/
DSTATUS disk_initialize(BYTE pdrv)
{
    (void)pdrv; /* Only one volume is supported. */

    if (HAL_SD_Init(&hsd) != HAL_OK) {
        return STA_NOINIT;
    }

            /* HIL-1 fix: 4-bit wide bus call REMOVED. sdio.c already configures the
       stable 1-bit mode (SDIO_BUS_WIDE_1B, ClockDiv = 16). Calling
       HAL_SD_ConfigWideBusOperation(&hsd, SDIO_BUS_WIDE_4B) here would
       override it and reintroduce instability. Only HAL_SD_Init is kept. */



    return 0; /* Drive ready and initialized. */
}

/*---------------------------------------------------------------------------*/
/* FatFs sector read                                                         */
/*---------------------------------------------------------------------------*/
DRESULT disk_read(BYTE pdrv, BYTE *buff, DWORD sector, UINT count)
{
    (void)pdrv;

    /* HAL expects uint8_t*, cast the BYTE* */
        /* FIX-B (2026-11-13): 200U -> 1000U, then FIX-B2 (bench, 2026-11-16):
         * 1000U -> 2000U. The 200 ms value from P0-4 was sized for the OLD
         * T_IWDG = 1.023 s; 1000 ms was set when P0-4 bumped IWDG_PRESCALER_32
         * (iwdg.c:45-46: T_IWDG = 4095*32/32000 = 4.095 s). 2000U is now the bound
         * because the TANK-MODE soak at 2.6667 MHz (8 min / 386 chunks, user-reported
         * bench result) showed the failure mode is the CARD's internal garbage
         * collection outliving 1000 ms, not the bus: HAL_SD_ReadBlocks returns on
         * DATAEND/CTIMEOUT/DTIMEOUT, and the SD-spec erase/write stall "can reach
         * ~1 s independently of the bus clock - the stall happens inside the card,
         * not on the wires" (wording of this same comment since FIX-B), so a deep GC
         * pause needs ~2 s for the card to wake up and answer the next command.
         * The 2 s is not bought for the bus: at SDIO_CK = 48/(16+2) = 2.6667 MHz,
         * 1-bit (src/sdio.c) one 512 B sector is on the wire 4096 bit / 2.6667 MHz =
         * 1.54 ms = 0.08 % of the bound, and a whole 12 294 B chunk is 36.9 ms =
         * 1.85 %, so ~98 % of the 2000 ms stays available for card-internal stall.
         * IWDG hierarchy (FIX-E, 2026-11-16): the refresh below now runs on BOTH
         * exit paths - success AND timeout - so a timed-out op can never eat the
         * next op's budget and no chain of FatFs calls above us can span
         * T_IWDG = 4095 ms without a feed. That closes doc/final_pre_flash_audit.md
         * W-11(a) / O-12, which was harmless at 1000 ms but became marginal at
         * 2000 ms (f_sync 2000 ms + f_close -> f_sync (src/ff.c:3821) 2000 ms
         * = 4000 ms, 95 ms of slack).
         * Current arithmetic of the whole write path: 1 attempt x 2000 ms
         * (VIBL_WRITE_RETRY_MAX = 1u, src/sd_writer.c:99) + 10 ms HAL_Delay +
         * ~500 ms of sd_error_shutdown delays (src/main.c:145-151) = ~2.5 s
         * < 4.095 s, and every individual op restarts with a full 4.095 s budget. */
        if (HAL_SD_ReadBlocks(&hsd, (uint8_t *)buff, sector, count, 2000U) != HAL_OK) {
        /* FIX-E (O-12): feed the watchdog on the FAILURE path as well. */
        HAL_IWDG_Refresh(&hiwdg);
        return RES_ERROR;
    }

    HAL_IWDG_Refresh(&hiwdg);
    return RES_OK;
}

/*---------------------------------------------------------------------------*/
/* FatFs sector write                                                        */
/*---------------------------------------------------------------------------*/
DRESULT disk_write(BYTE pdrv, const BYTE *buff, DWORD sector, UINT count)
{
    (void)pdrv;

    /* FIX-B -> FIX-B2: 200U -> 1000U -> 2000U, same reason as disk_read above
     * (T_IWDG = 4.095 s, iwdg.c:45-46, so 2 s per op is the safe ceiling).
     * HAL_SD_WriteBlocks returns only after DATAEND, and its Timeout only guards
     * the FIFO-drain loop; the card-internal program/erase / GC stall is covered
     * by SDMMC_DATATIMEOUT inside the SDMMC peripheral, but the HAL 2 s bound is
     * what protects us from a wedged bus - and 2 s is what the 2.6667 MHz soak
     * showed a deep card-GC pause needs before the card answers the next command.
     * Hierarchy (FIX-E): the refresh below runs after EVERY bounded call - success
     * and timeout - so one op can never starve the next one's budget.
     * 1 attempt x 2000 ms (VIBL_WRITE_RETRY_MAX = 1u, src/sd_writer.c:99)
     * + 10 ms HAL_Delay + ~500 ms of sd_error_shutdown delays (src/main.c:145-151)
     * = ~2.5 s < T_IWDG = 4095 ms (iwdg.c:45-46). */
    if (HAL_SD_WriteBlocks(&hsd, (uint8_t *)buff, sector, count, 2000U) != HAL_OK) {
        /* FIX-E (O-12): feed the watchdog on the FAILURE path as well. */
        HAL_IWDG_Refresh(&hiwdg);
        return RES_ERROR;
    }

    /* FIX-D (milestone 1, LOG0003.VIB): HAL_Delay(10) here is INTENTIONAL and was
     * REINTRODUCED after P0-3 (audit_13_11_26_1 BUG-2) had removed it.
     *
     * Why P0-3 was wrong: it assumed HAL_SD_WriteBlocks() returns only once the
     * card has finished programming. It does not. The polling loop at
     * stm32f4xx_hal_sd.c:858 exits on SDIO_FLAG_DATAEND, i.e. the SDIO DPSM has
     * drained its FIFO - the card's internal program/erase cycle is still
     * running, and HAL never looks at it: HAL_SD_WriteBlocks() (lines 784-961)
     * contains NO call to HAL_SD_GetCardState(); the only calls in that file are
     * in the Abort* functions (2391, 2454, 2572, 2603, 2642). HAL documents the
     * obligation on the caller instead - "This API should be followed by a check
     * on the card state through HAL_SD_GetCardState()" (stm32f4xx_hal_sd.c:776).
     * We deliberately do NOT poll, so the wait is supplied as a blind delay.
     *
     * Observed failure mode on the 32 GB card without the delay: the next command
     * in line (the following sector of the same chunk, or FatFs' FAT/dir write)
     * is issued while the card is still internally BUSY, that command never gets
     * its response, HAL_SD_WriteBlocks() returns HAL_ERROR, and FatFs surfaces
     * FR_DISK_ERR (sd_writer.c:268). UNCERTAIN: the exact first flag (CTIMEOUT vs
     * DATA_TIMEOUT) was not captured on the bench; the recovery signature was
     * identical for all of them.
     *
     * Cost is bounded and hidden behind acquisition, so it is affordable:
     * 10 ms per disk_write call, and the transfer term. UNCERTAIN (needs DWT):
     * this comment's original figure counted "25 disk_write calls per 12294 B
     * chunk"; the real call count depends on fs->csize (4 KiB clusters = 8
     * sectors per cluster on a 32 GB card) and on cluster alignment, because
     * ff.c clips a direct multi-sector write at the cluster boundary. Verify
     * with DWT_CYCCNT before quoting the duty again.
     * Transfer term at the CURRENT clock (TANK MODE, src/sdio.c: SDIO_CK =
     * 48/(16+2) = 2.6667 MHz, 1-bit) = 12294*8/2.6667e6 = 36.9 ms per chunk; the
     * retired rungs measured 12.3 ms (8 MHz) and 8.2 ms (12 MHz). Against 1280 ms
     * needed to fill a chunk at 1600 Hz the consumer still runs ~35x ahead of the
     * producer => no arena overrun, no dropped samples.
     * P0-3's duty arithmetic (7 % -> 27 %) was correct; only its conclusion was.
     * FIX-E: HAL_IWDG_Refresh() now feeds the dog after EVERY bounded call -
     * success (below, after the 10 ms delay) and timeout (before RES_ERROR) - so
     * whatever chain FatFs builds above us (f_write / f_sync / f_lseek /
     * f_truncate / f_close), the gap between feeds never exceeds one 2000 ms op
     * (+ 10 ms), against T_IWDG = 4.095 s (iwdg.c:45-46).
     * doc/final_pre_flash_audit.md W-11(a) / O-12: CLOSED by this patch. */
    HAL_Delay(10);
    HAL_IWDG_Refresh(&hiwdg);
    return RES_OK;
}

/*---------------------------------------------------------------------------*/
/* FatFs disk control (ioctl)                                                */
/*---------------------------------------------------------------------------*/
DRESULT disk_ioctl(BYTE pdrv, BYTE cmd, void *buff)
{
    (void)pdrv;

    switch (cmd) {
        case CTRL_SYNC:
            /* HAL_SD transfers are synchronous; nothing to flush. */
            return RES_OK;

        case GET_SECTOR_COUNT:
            /* hsd.SdCard.LogBlockNbr is defined in HAL for F4 */
            *(DWORD *)buff = hsd.SdCard.LogBlockNbr;
            return RES_OK;

        case GET_SECTOR_SIZE:
            *(WORD *)buff = 512U;
            return RES_OK;

        case GET_BLOCK_SIZE:
            *(DWORD *)buff = 1U;
            return RES_OK;

                default:
            return RES_PARERR;
    }
}

/*---------------------------------------------------------------------------*/
/* FatFs timestamp callback (P2-9, audit_13_11_26_1 WARN-4)                  */
/*                                                                           */
/* Declared by ff.h under `#if !_FS_READONLY && !_FS_NORTC` and called by    */
/* ff.c through GET_FATTIME() on create/truncate/dir-register.               */
/*                                                                           */
/* RTC_TimeTypeDef / RTC_DateTypeDef are SEPARATE structs in STM32F4 HAL     */
/* (there is no RTC_DateTimeTypeDef); RM009 requires reading the time before  */
/* the date so the shadow registers are captured consistently — same order   */
/* used by vibl_rtc_ms() in sd_writer.c.                                     */
/*                                                                           */
/* FatFs packs the stamp as: bit31..25 = year-1980, 24..21 = month,          */
/* 20..16 = day, 15..11 = hour, 10..5 = minute, 4..0 = second/2.             */
/* RTC stores Year as an offset from 2000, hence (Year + 20) == (year - 1980).*/
/*                                                                           */
/* Returns 0 on a HAL failure. NOTE: 0 is a LEGAL FatFs stamp (1980-01-01    */
/* 00:00:00) and must not be treated as "unknown" here — the RTC-not-ready   */
/* condition is detected separately via vibl_get_start_ts() in main.c.       */
/*---------------------------------------------------------------------------*/
DWORD get_fattime(void)
{
    RTC_TimeTypeDef  t;
    RTC_DateTypeDef  d;

    if (HAL_RTC_GetTime(&hrtc, &t, RTC_FORMAT_BIN) != HAL_OK) {
        return 0u;
    }
    if (HAL_RTC_GetDate(&hrtc, &d, RTC_FORMAT_BIN) != HAL_OK) {
        return 0u;
    }

    return ((DWORD)(d.Year + 20u) << 25)
         | ((DWORD)d.Month       << 21)
         | ((DWORD)d.Date        << 16)
         | ((DWORD)t.Hours       << 11)
         | ((DWORD)t.Minutes     <<  5)
         | ((DWORD)(t.Seconds / 2u));
}