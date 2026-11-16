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
/* P1-8 (graceful shutdown on SD error): how many times the main loop re-inits
 * the SDMMC bus with HAL_SD_Init() before it gives up and closes the file for
 * good. Each attempt is bracketed by a card-detect gate and HAL_IWDG_Refresh(),
 * because HAL_SD_Init() feeds the watchdog nowhere internally
 * (doc/final_pre_flash_audit.md W-5; the SDMMC busy-waits use SDIO_CMDTIMEOUT,
 * which ll_sdmmc.c:1330 treats as milliseconds). T_IWDG = 4.095 s (iwdg.c P0-4). */
#define SD_RECOVERY_ATTEMPTS 3u

/* V2 LOSSY MODE (V2-1 / Milestone 2b, closes O-11 of doc/final_pre_flash_audit.md)
 * --------------------------------------------------------------------------
 * A failed chunk commit is a CARD-internal garbage-collection pause, not a dead
 * bus: at SDIO_CK = 48/(16+2) = 2.6667 MHz, 1-bit (src/sdio.c), one 12 294 B
 * chunk is on the wire for 12294*8/2.6667e6 = 36.9 ms = 1.85 % of the 2000 ms
 * HAL bound (src/diskio.c:114), so the remaining ~98 % of a timeout is the card
 * being busy internally. Consumer cards do this for > 2 s on purpose.
 * V2 therefore stops treating one stall as fatal: the chunk is DROPPED, the hole
 * is recorded as a sequence-number jump, the bus is re-initialised ONCE and
 * recording CONTINUES. Continuous recording time outranks a lossless stream;
 * lossy-ness is fully observable because parser_vibl.py reports every jump.
 * sd_error_shutdown() is kept as the fallback for sessions that genuinely cannot
 * continue: card pulled, FatFs chain damaged, stream rewind failed. */
#define SD_GAP_BLINK_MS 30u   /* one dropped chunk = one 30 ms ON / 30 ms OFF flash */
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
/* P1-8 - graceful shutdown after an SD write error.
 * V2-1 (2026-11-16, LOSSY MODE): this is now the FALLBACK path only. The main
 * loop first tries sd_error_recover() and keeps recording; everything below
 * stays exactly as specified for the sessions that cannot be recovered.
 *
 * WHY THIS EXISTS: every chunk is committed by f_sync() inside
 * vibl_write_chunk() (sd_writer.c:420), so when the card stalls the samples
 * already on the card are safe. What is at risk is the END of the file: without
 * vibl_close() the session leaves behind the preallocated tail (P1-7 stretch
 * mode: up to 42 MiB; P3-1 expand mode: no tail at all),
 * no trailer (end_ts / total_records / file_crc32), and the few bytes of the
 * chunk that died mid-write still punched into the stream. The old behaviour
 * (a bare hil3_error_loop(5) right at the failing write) abandoned the file in
 * exactly that state, i.e. a recoverable stall looked like a power cut.
 *
 * This path preserves the data instead:
 *   1. re-init the bus (up to SD_RECOVERY_ATTEMPTS x HAL_SD_Init) - NOT a
 *      re-mount: f_mount(&fs,"0:",1) would run fs->id = ++Fsid (ff.c:3192) while
 *      validate() demands obj->id == obj->fs->id (ff.c:3224), which would mark
 *      the already open FIL invalid and make the close impossible. HAL_SD_Init
 *      alone is sufficient: validate() consults only disk_status() (ff.c:3236),
 *      which is the PA8 card-detect pin, and fs->fs_type stays non-zero.
 *   2. vibl_close(): seek back to vibl_data_end (advanced ONLY by successful
 *      chunks, sd_writer.c:407) + f_truncate() -> the abandoned partial chunk
 *      is cut away, then the trailer lands at the true EOF with a CRC that
 *      matches the surviving payload byte-for-byte (parser_vibl.py must then
 *      print "File CRC32: ... MATCH", verified by test/SYNTH_SDERROR_CLOSED.VIB).
 *   3. stop the 1600 Hz sampler and park in the error-blink loop.
 *      SUPERSEDED for the ordinary case by V2-1 (2026-11-16): the main loop now
 *      prefers sd_error_recover() + vibl_recover(), which guarantees the stream
 *      offset (fptr is rewound to vibl_data_end) and makes the hole VISIBLE as a
 *      sequence jump - see the LOSSY MODE note in src/main.c and
 *      doc/MILESTONE_2_ROBUSTNESS.md §10. Steps 1-3 here remain the fallback for
 *      a session that cannot be recovered (card pulled, chain damaged, rewind
 *      failed), where a truncated-but-valid file is still worth more than nothing.
 *
 * Bounded time: 3 x (CD read + HAL_SD_Init + 200 ms) + close. On a REMOVED card
 * the CD gate breaks the loop immediately, so no CMD busy-wait is ever spent.
 * UNCERTAIN (hardware, unchanged from audit U-3): with a card that is physically
 * present but electrically dead, HAL_SD_Init may block longer than T_IWDG =
 * 4.095 s; then the IWDG resets the board instead of blinking code 5. The file
 * is still safe (chunk-granular f_sync), it simply has no trailer - which
 * parser_vibl.py now reports as "No trailer found (power loss/error)". */
static volatile uint8_t sd_recovery_ok = 0u;   /* debugger: 1 = bus came back */

/* V2-1 (LOSSY MODE) error log. This tree has NO UART - include/main.h defines
 * only BUTTON_Pin / CS_LIS_Pin / Led_Pin / SD_DETECT_Pin - so "log the error"
 * means (a) counters for the debugger and the post-mortem, and (b) one visible
 * LED flash per dropped chunk (PB2 Active HIGH, doc/WEACT_F405_PINOUT.md §2):
 *   sd_gap_events   - chunks abandoned whose session CONTINUED (recovered)
 *   sd_fatal_events - sessions that had to be ended by sd_error_shutdown()
 *   sd_last_write_rc- last vibl_status_t (VIBL_ERR_WRITE -4 / VIBL_ERR_SYNC -5)
 * The per-chunk FRESULT stays in vibl_dbg_last_fr and the abandoned-chunk latch
 * in vibl_dbg_sd_error (src/sd_writer.c), the surviving-hole count in
 * vibl_get_dropped_samples() and vibl_get_recover_count(). */
static volatile uint32_t sd_gap_events   = 0u;
static volatile uint32_t sd_fatal_events = 0u;
static volatile int      sd_last_write_rc = 0;

static void sd_error_shutdown(void)
{
    uint8_t attempt;

    for (attempt = 0u; attempt < SD_RECOVERY_ATTEMPTS; attempt++) {
        /* Card detect BEFORE re-init. PA8 HIGH = inserted (SSOT
         * doc/WEACT_F405_PINOUT.md sec.1, same polarity as main.c's startup
         * CD-gate and diskio.c:26). A removed card makes HAL_SD_Init burn the
         * SDIO_CMDTIMEOUT loops with no IWDG feed -> reset, never a blink. */
        if (HAL_GPIO_ReadPin(SD_DETECT_GPIO_Port, SD_DETECT_Pin) == GPIO_PIN_RESET) {
            break;
        }
        HAL_IWDG_Refresh(&hiwdg);
        if (HAL_SD_Init(&hsd) == HAL_OK) {
            sd_recovery_ok = 1u;
            HAL_Delay(100);              /* let the card finish its internal state */
            HAL_IWDG_Refresh(&hiwdg);
            break;
        }
        HAL_IWDG_Refresh(&hiwdg);
        HAL_Delay(200);                  /* GC / erase-busy settle, then retry */
        HAL_IWDG_Refresh(&hiwdg);
    }

    /* Write the trailer while the bus may still answer. If it does not, every
     * FatFs call inside returns fast (validate() -> FR_INVALID_OBJECT, or the
     * bounded 2000 ms disk timeout) and vibl_abort() inside closes the file.
     * O-12 CLOSED (FIX-E, 2026-11-16): disk_read/disk_write now feed the IWDG on
     * the FAILURE path too (src/diskio.c), so the f_sync -> f_close -> f_sync
     * chain (src/ff.c:3821) restarts with a full 4095 ms budget per op. Budget of
     * this path: 1 x 2000 ms failed chunk + <= 500 ms delays = ~2.5 s < 4.095 s. */
    (void)vibl_close();

    /* Stop the sampler last (patch-spec order). The TIM2 callback touches RAM
     * only (arena / full_mask / dropped), so it cannot disturb the FatFs work
     * above; the leftover full_mask is simply never consumed again. */
    HAL_TIM_Base_Stop_IT(&htim2);

    /* R0.12c unmount is f_mount(NULL, path, 0) (ff.c:3277-3293), same call the
     * PC13 stop path uses. */
    (void)f_mount(NULL, "0:", 0);
    HAL_GPIO_WritePin(Led_GPIO_Port, Led_Pin, GPIO_PIN_RESET);
    hil3_error_loop(5);   /* parks forever, refreshing the IWDG every 200 ms */
}

/* V2-1 (LOSSY MODE) recovery: ONE bounded attempt to bring the session back.
 * Returns 0 = stream realigned, recording continues; non-zero = cannot continue,
 * the caller must fall back to sd_error_shutdown() above.
 *
 * Exactly one HAL_SD_Init(&hsd) and NO re-mount, for the reason already
 * documented at main.c:105-110 and doc/MILESTONE_2_ROBUSTNESS.md §4.2a:
 * f_mount(&fs,"0:",1) would run fs->id = ++Fsid (ff.c:3192) while validate()
 * demands obj->id == obj->fs->id (ff.c:3224) - the open FIL would go invalid and
 * every later f_write / f_lseek / f_sync / f_truncate would answer
 * FR_INVALID_OBJECT without attempting any I/O.
 *
 * Card detect FIRST: PA8 LOW = card absent (doc/WEACT_F405_PINOUT.md §1, same
 * polarity as the boot gate at main.c and disk_status() in diskio.c). Letting
 * HAL_SD_Init burn its SDMMC_CMDTIMEOUT loops without a card only wastes the
 * watchdog window (audit U-3: HAL_SD_Init feeds nothing internally) and would
 * end the run in a reset instead of a controlled close.
 *
 * Time budget of this function: 1 x HAL_SD_Init (UNCERTAIN upper bound, audit
 * U-3, unchanged from Milestone 2) + 100 ms settle; every neighbouring bounded
 * op is fed by HAL_IWDG_Refresh here and by disk_read/disk_write on BOTH exit
 * paths (src/diskio.c:87, :116), so the gap between feeds never exceeds one
 * 2000 ms op against T_IWDG = 4095 * 32 / 32000 = 4.095 s (src/iwdg.c:45-46). */
static int sd_error_recover(void)
{
    if (HAL_GPIO_ReadPin(SD_DETECT_GPIO_Port, SD_DETECT_Pin) == GPIO_PIN_RESET) {
        return -1;                       /* card gone: never touch the bus again */
    }

    HAL_IWDG_Refresh(&hiwdg);
    (void)HAL_SD_Init(&hsd);             /* clear SDMMC / card state, once */
    HAL_IWDG_Refresh(&hiwdg);
    HAL_Delay(100);                      /* let the card finish GC / program-busy */
    HAL_IWDG_Refresh(&hiwdg);

    /* TASK-2 half of TASK-1: unlatch FIL::err and rewind the file pointer to
     * vibl_data_end. main.c cannot touch vibl_fil - it is static in
     * src/sd_writer.c - so vibl_recover() is the ONLY legal way to clear the
     * latch. Without it the next vibl_write_chunk() would return the stale
     * FRESULT immediately (ff.c:3627) and every later chunk would fail too. */
    if (vibl_recover() != VIBL_OK) {
        return -2;                       /* chain damaged / size clipped */
    }

    sd_recovery_ok = 1u;
    return 0;
}

/* V2-1 gap log: counters + one short LED flash. 2 x 30 ms = 60 ms is 4.7 % of
 * the 1280 ms the TIM2 ISR needs to mature one arena (2048 samples / 1600 Hz)
 * and 3 % of the 2000 ms stall just paid, so the log cannot realistically cause
 * a second hole; the watchdog is fed on both halves. */
static void sd_gap_log(int rc)
{
    sd_gap_events++;
    sd_last_write_rc = rc;
    HAL_GPIO_WritePin(Led_GPIO_Port, Led_Pin, GPIO_PIN_SET);
    HAL_Delay(SD_GAP_BLINK_MS);
    HAL_IWDG_Refresh(&hiwdg);
    HAL_GPIO_WritePin(Led_GPIO_Port, Led_Pin, GPIO_PIN_RESET);
    HAL_Delay(SD_GAP_BLINK_MS);
    HAL_IWDG_Refresh(&hiwdg);
}
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

    /* ---- V2 LOSSY MODE (V2-1, Milestone 2b) -------------------------------
     * TASK-1: a failed vibl_write_chunk() NO LONGER ends the session.
     * The failure is a card-internal GC pause outliving the 2000 ms HAL bound
     * (src/diskio.c:102-113), so V2 drops exactly that chunk, makes the hole
     * explicit in the sequence chain, re-inits the bus once and KEEPS RECORDING.
     * Continuous recording time now outranks a lossless stream - and the loss is
     * auditable, because parser_vibl.py reports every jump it sees.
     * sd_error_shutdown() remains only as the fallback for a session that truly
     * cannot continue (card pulled, FatFs chain damaged, stream rewind failed). */
    int wr_err = 0;
    uint32_t lost_chunks = 0u;

    if (m & 1u) {
      int rc = vibl_write_chunk(arena[0], VIBL_SAMPLES_PER_CHUNK);
      if (rc != VIBL_OK) { wr_err = rc; lost_chunks++; }  /* this chunk never landed */
    }
    /* If arena[0] already burned its 1 x 2000 ms timeout (src/diskio.c, FIX-F:
     * VIBL_WRITE_RETRY_MAX = 1u), spending another 2000 ms on arena[1] cannot
     * help and only widens the stall. Its samples are gone as well, so they MUST
     * be gap-counted too - an uncounted skip would let the surviving seq chain
     * claim 1.28 s of data that was never written. */
    if (m & 2u) {
      if (wr_err == 0) {
        int rc = vibl_write_chunk(arena[1], VIBL_SAMPLES_PER_CHUNK);
        if (rc != VIBL_OK) { wr_err = rc; lost_chunks++; }
      } else {
        lost_chunks++;
      }
    }

    /* Holes the producer already counted itself: while the consumer was stalled
     * both arenas were full, so TIM2 dropped whole arenas into `dropped`. */
    if (d) { (void)vibl_mark_gap(d); }

    if (wr_err != 0) {
      /* TASK-1 step 2 - explicit sequence jump, one VIBL_SAMPLES_PER_CHUNK per
       * lost chunk. next_chunk_seq is NOT advanced by a failed chunk
       * (sd_writer.c:591-593 run only after a successful f_sync), so this adds
       * precisely (lost_chunks) missing chunk indices: the previously written
       * chunk k and the next one k+1+lost_chunks bracket the hole. */
      (void)vibl_mark_gap(lost_chunks * VIBL_SAMPLES_PER_CHUNK);
      sd_gap_log(wr_err);                        /* TASK-1 step 3a: log it */
      if (sd_error_recover() != 0) {             /* 3b: 1 x HAL_SD_Init + unlatch */
        sd_fatal_events++;
        sd_error_shutdown();                     /* never returns */
      }
      /* 3c: fall through. full_mask/arena state is untouched by the stall, so
       * the next iteration simply writes the next arena. */
    }

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

    /* BLOCKER-1 fix (doc/final_pre_flash_audit.md "Blockers/BLOCKER-1", check
     * C3): the advance+wrap block used to live ONLY on the success path, while
     * the SPI-error path burned the slot and returned. Sequence that broke: an
     * SPI failure at fill_cnt = 6141 -> fill_cnt = 6144 -> return; on the NEXT
     * tick the success path ran lis3dsh_read(&b[6144], &b[6145], &b[6146]) with
     * arena[i] indexed 0..6143 (VIBL_CHUNK_WORDS = 2048 * 3) - an out-of-bounds
     * write into the adjacent .bss object. Secondary defect of the same
     * asymmetry: an arena whose last slots were burned was NEVER marked full,
     * so those samples vanished with no `dropped` count and no seq jump, and
     * fill_idx stayed stuck on the saturated arena.
     *
     * Fix: read first, then fall through ONE shared advance/wrap block, so the
     * entry invariant fill_cnt <= 6141 holds by construction for both paths.
     * Cost: ~3 instructions inside a 625 us (1600 Hz) ISR window - no timing
     * impact at -O2. */
    int rc = lis3dsh_read(&b[fill_cnt], &b[fill_cnt + 1], &b[fill_cnt + 2]);
    if (rc != 0) {
        /* P2-8: escalate to the error loop only after 10 consecutive failures
         * (~6.3 ms of dead bus). A one-off glitch costs a single sample and is
         * NOT counted into `dropped`, because a partially invalid arena cannot
         * be reported as a clean gap without a new VIBL record type; closing
         * that properly needs an invalid-sample flag in the record format.
         * The burned slot is written as a stale/zero sample inside the chunk
         * that closes below - documented tradeoff (audit WARN-2). */
        if (++spi_err_streak >= 10u) {
            lis3dsh_err = 1u;   /* -> hil3_error_loop(4) in the main loop */
        }
    } else {
        spi_err_streak = 0u;    /* P2-8: one good frame clears the streak */
    }

    /* Shared wrap for BOTH paths (this is what closes C3): the slot is always
     * burned and the arena is always closed at VIBL_CHUNK_WORDS, so
     * fill_cnt < VIBL_CHUNK_WORDS at the next entry. */
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
