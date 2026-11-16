/**
 * @file    sd_writer_fixed.c
 * @brief   VIBL binary logger implementation (see sd_writer.h).
 *          Fixed version: BUG-2 (FA_WRITE), BUG-3 (ff.h include),
 *          BUG-7 (gain semantics comment), chunk count bound, header_crc zeroing.
 */
#include "sd_writer.h"
#include "main.h"      /* stm32f4xx_hal.h -> HAL_IWDG_Refresh, HAL_RTC_* */
#include "iwdg.h"      /* extern IWDG_HandleTypeDef hiwdg;               */
#include "rtc.h"       /* extern RTC_HandleTypeDef hrtc;                 */
#include "ff.h"        /* FatFs R0.12c API (f_open/f_write/f_sync/...)    */

#include <string.h>
#include <stddef.h>

/* -------------------------------------------------------------------------- */
/* Private state (static, no dynamic memory)                                 */
/* -------------------------------------------------------------------------- */
static FIL           vibl_fil;          /* open file object                */
static vibl_header_t vibl_hdr;          /* header kept for close-time update*/
static volatile int  vibl_active = 0;   /* 1 while a VIBL file is open     */

static uint32_t      next_chunk_seq    = 0u;
static uint32_t      total_records     = 0u;
static uint32_t      chunk_count       = 0u;
static uint32_t      total_dropped     = 0u;
static volatile int  overflow_latched  = 0;
/* P1-7 companion: real end of written payload. It is the authority for where
 * the trailer goes, whichever way the session was preallocated:
 *   - VIBL_PREALLOC_STRETCH (legacy f_lseek stretch): f_size() is the
 *     PREALLOCATED length, so only vibl_data_end can locate the trailer;
 *   - VIBL_PREALLOC_EXPAND (P3-1, f_expand opt=0): f_write keeps
 *     objsize == max(fptr, objsize) (ff.c:3636), so the FatFs object size
 *     already equals the bytes written and vibl_data_end == f_size(). */
static uint32_t      vibl_data_end     = 0u;

/* P3-1 (Milestone 3): which preallocation strategy this session ended up with.
 * It decides whether vibl_close() has a ~42 MiB tail to release or nothing. */
#define VIBL_PREALLOC_NONE      0u   /* nothing attempted / no session opened   */
#define VIBL_PREALLOC_EXPAND    1u   /* f_expand(opt=0): contiguous, size==data */
#define VIBL_PREALLOC_STRETCH   2u   /* f_lseek stretch: needs the big truncate */
static uint8_t       vibl_prealloc_mode = VIBL_PREALLOC_NONE;

/* Running CRC-32 state (un-finalized). Starts at 0xFFFFFFFF and is finalized
 * with ^0xFFFFFFFF only when the trailer value is produced.               */
static uint32_t      vibl_file_crc     = 0xFFFFFFFFu;

/* V2-1 (LOSSY MODE, 2026-11-16) ------------------------------------------
 * CRC state as it was BEFORE the chunk that is currently being attempted.
 * vibl_write_chunk() folds the chunk's bytes into vibl_file_crc BEFORE the
 * committing f_sync() (see the tail of vibl_write_chunk: CRC at :498-499,
 * f_sync at :502), so a chunk that dies in f_sync would otherwise leave
 * 12 294 bytes inside the running CRC while those bytes are being abandoned.
 * vibl_stall() restores this snapshot, which is what keeps the trailer CRC
 * correct across an arbitrary number of dropped chunks - the whole point of
 * Lossy Mode (doc/MILESTONE_2_ROBUSTNESS.md §10). */
static uint32_t      vibl_chunk_crc_start = 0xFFFFFFFFu;

/* Sensor name table indexed by sensor_id (written into header.sensor[16]). */
static const char *const vibl_sensor_names[] = {
    "HIL-DAC-ADC",   /* VIBL_SENSOR_HIL   : HIL DAC->ADC loopback           */
    "LIS3DSH",       /* VIBL_SENSOR_LIS3DSH : production LIS3DSH accel      */
};
#define VIBL_SENSOR_COUNT (sizeof(vibl_sensor_names) / sizeof(vibl_sensor_names[0]))

/* -------------------------------------------------------------------------- */
/* CRC-32 (standard zlib/IEEE: poly 0xEDB88320, init 0xFFFFFFFF, final XOR)  */
/* -------------------------------------------------------------------------- */
static uint32_t crc_table[256];
static int      crc_table_ready = 0;

static void crc32_build_table(void)
{
    for (uint32_t i = 0u; i < 256u; i++) {
        uint32_t c = i;
        for (int k = 0; k < 8; k++) {
            c = ((c & 1u) != 0u) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
        }
        crc_table[i] = c;
    }
}

/* Incremental CRC-32: `crc` is the running (un-finalized) state. */
static uint32_t crc32_update(uint32_t crc, const uint8_t *data, uint32_t len)
{
    if (!crc_table_ready) {
        crc32_build_table();
        crc_table_ready = 1;
    }
    for (uint32_t i = 0u; i < len; i++) {
        crc = crc_table[(crc ^ data[i]) & 0xFFu] ^ (crc >> 8);
    }
    return crc;
}

/* -------------------------------------------------------------------------- */
/* Helpers                                                                    */
/* -------------------------------------------------------------------------- */

/* FIX-C (this session) -----------------------------------------------
 * Debug-only observability for the write path. Read these three symbols in the
 * debugger watch window (they are volatile => not optimised away, not cached):
 *   vibl_dbg_last_fr  FRESULT returned by the LAST f_write attempt
 *   vibl_dbg_last_bw  UINT  bytes FatFs reported as written in that attempt
 *   vibl_dbg_attempts attempt number of that last chunk (1..VIBL_WRITE_RETRY_MAX)
 * Rationale: VIBL_ERR_WRITE (-4) alone cannot distinguish FR_DISK_ERR(1),
 * FR_INVALID_OBJECT(9) and FR_DENIED(7) - the three failures this project has
 * actually seen. Cost: 3 words + 1 word in .bss, no extra flash in hot code.
 */
#define VIBL_WRITE_RETRY_MAX 1u   /* FIX-F (2026-11-16): 3u -> 1u. 3 attempts x 2000 ms = 6 s of blocking is worse than going straight to sd_error_shutdown(). */
static volatile FRESULT vibl_dbg_last_fr  = FR_OK;
static volatile UINT    vibl_dbg_last_bw  = 0u;
static volatile int     vibl_dbg_attempts = 0;
static volatile FRESULT vibl_dbg_open_fr  = FR_OK;  /* FIX-D: last f_open() result */
static volatile int     vibl_dbg_cd_pa8   = -1;     /* FIX-D: 1 = PA8 HIGH = card inserted */
static volatile int     vibl_dbg_sd_error = 0;      /* P1-8: 1 = a chunk was abandoned */
static volatile FRESULT vibl_dbg_expand_fr = FR_OK; /* P3-1: f_expand() result; 7 = FR_DENIED -> stretch fallback */
/* V2-1 (LOSSY MODE): number of chunks abandoned AND successfully recovered in
 * this session. 0 = lossless run; > 0 = the file contains that many seq holes.
 * Read it in the watch window together with vibl_dbg_sd_error and with
 * vibl_get_dropped_samples() / vibl_get_chunk_count(). */
static volatile uint32_t vibl_dbg_recover_count = 0u;

/* Write `len` bytes to the open file, looping in <=64 KiB f_write calls.
 *
 * FIX-C: bounded retry (VIBL_WRITE_RETRY_MAX attempts) per chunk, resuming
 * exactly at the point where FatFs stopped. Two R0.12c facts drive the design:
 *
 * 1. ff.c:3635-3637 - the for-loop increments (wbuff, fp->fptr, fp->obj.objsize,
 *    *bw, btw) only execute for iterations that did NOT ABORT. ff.c:455 ABORT()
 *    returns from inside the body. Therefore on a failure *bw is exactly the
 *    number of bytes already committed to the card AND fp->fptr advanced by the
 *    same amount. Retrying must continue at (buf + bw); re-sending the whole
 *    chunk would duplicate bw bytes at the wrong file offset.
 *
 * 2. ff.c:3627 - after an ABORT, fp->err is latched and EVERY later f_write()
 *    returns that stale code immediately, without attempting any disk I/O.
 *    FIL::err is a public member (ff.h:158), so it is cleared here to make a
 *    genuine retry possible. FR_INVALID_OBJECT raised by validate() (ff.c:3221)
 *    does NOT set fp->err - it is a drive-state rejection, not an abort.
 *
 * The retry is defense-in-depth against transient FR_DISK_ERR (card GC stall).
 * It deliberately does NOT mask a persistent state error: after VIBL_WRITE_RETRY_MAX
 * attempts (1 since FIX-F) it still fails with VIBL_ERR_WRITE - real FRESULT in
 * vibl_dbg_last_fr - and the caller routes straight to sd_error_shutdown(). */
static int vibl_write_all(const void *buf, uint32_t len)
{
    const uint8_t *p = (const uint8_t *)buf;

    while (len > 0u) {
        UINT chunk = (len > 0xFFFFu) ? (UINT)0xFFFFu : (UINT)len;
        UINT done  = 0u;                 /* bytes of THIS chunk on the card      */
        int  attempt = 0;

        while (done != chunk) {
            UINT bw = 0u;

            if (++attempt > (int)VIBL_WRITE_RETRY_MAX) {
                vibl_dbg_attempts = attempt;
                return VIBL_ERR_WRITE;   /* fr of the last attempt is in _dbg_last_fr */
            }

            FRESULT fr = f_write(&vibl_fil, p + done, chunk - done, &bw);

            vibl_dbg_last_fr  = fr;
            vibl_dbg_last_bw  = bw;
            vibl_dbg_attempts = attempt;

            /* Count real progress only (see R0.12c fact 1 above). */
            if (bw > 0u) {
                done += bw;
            }

            if (fr != FR_OK) {
                vibl_fil.err = 0u;       /* unlatch, otherwise attempt N+1 is a no-op */
            }

            /* FR_OK with zero progress cannot happen for btw > 0 in R0.12c; if it
             * ever does, retrying is pointless - bail out instead of spinning. */
            if ((fr == FR_OK) && (bw == 0u)) {
                return VIBL_ERR_WRITE;
            }

            /* FIX-E made disk_read/disk_write feed the dog on BOTH exit paths, so
             * no bounded op can starve the next one; this feed is the second belt.
             * With VIBL_WRITE_RETRY_MAX = 1u (FIX-F) one chunk costs at most
             * 1 x 2000 ms, i.e. the gap between feeds stays ~2.0 s + 10 ms, well
             * under T_IWDG = 4.095 s (iwdg.c:45-46, PRESCALER_32 / Reload 4095). */
            HAL_IWDG_Refresh(&hiwdg);
        }

        p   += chunk;
        len -= chunk;
    }
    return VIBL_OK;
}

/* Best-effort cleanup on a mid-stream failure. */
static void vibl_abort(void)
{
    if (vibl_active != 0) {
        f_close(&vibl_fil);   /* ignore result: file already damaged/aborted */
        vibl_active = 0;
    }
}

/* P1-8 companion (main.c sd_error_shutdown) / V2-1 (main.c Lossy Mode):
 * abandoning one CHUNK must NOT close the file. The failure paths of
 * vibl_write_chunk() call this instead of vibl_abort() so that the session can
 * EITHER be finished cleanly (old shutdown path) OR continued with the next
 * chunk (Lossy Mode):
 *   - vibl_data_end advances ONLY on successful chunks (see the tail of
 *     vibl_write_chunk), so it is the authority for "last committed byte";
 *   - vibl_file_crc is rolled back to vibl_chunk_crc_start here, because it was
 *     already folded with the abandoned chunk's bytes before f_sync() failed;
 *     without the rollback the trailer CRC would not match the surviving
 *     payload and the parser would report a mismatch on a perfectly good file;
 *   - FIL::err is cleared on purpose: after a disk error FatFs latches it
 *     (ABORT, ff.c:455) and f_write (ff.c:3627), f_lseek (ff.c:4015) and
 *     f_truncate (ff.c:4485) all return the stale code WITHOUT attempting any
 *     I/O, so without the unlatch neither the next chunk nor the close
 *     sequence would do anything at all;
 *   - with vibl_abort() here, vibl_close() could only answer
 *     VIBL_ERR_NOT_OPEN and no trailer would ever reach the card.
 * This function performs NO disk I/O - the stream realignment is done by
 * vibl_rewind_to_data_end() below, so the shutdown path (vibl_close() seeks on
 * its own) stays exactly as cheap and as safe as before. */
static void vibl_stall(void)
{
    vibl_file_crc    = vibl_chunk_crc_start;  /* V2-1: drop the dead chunk from the CRC */
    vibl_fil.err     = 0u;                    /* unlatch, see above                     */
    vibl_dbg_sd_error = 1;                    /* debugger: a chunk was abandoned        */
}

/* V2-1 (LOSSY MODE) stream realignment. Closes O-11 of
 * doc/final_pre_flash_audit.md, which asked for exactly this primitive
 * ("f_lseek(vibl_data_end) + FIL.err unlatch, i.e. a future vibl_recover_chunk()").
 *
 * WHY IT IS MANDATORY BEFORE CONTINUING: f_write() advances fp->fptr by exactly
 * the bytes it managed to commit (ff.c:3635-3637 - the loop increments run only
 * for iterations that did not ABORT; ABORT returns from inside the body). A dead
 * chunk therefore leaves 0..12 294 bytes punched between vibl_data_end and fptr.
 * Appending the next chunk at that fptr would put a chunk header in the middle
 * of the previous chunk's sample data and parser_vibl.py would read int16
 * samples as a 6-byte header (seq/count garbage) - an UNDETECTABLE hole, which
 * is precisely why Milestone 2 chose to end the session. Rewinding makes the hole
 * explicit instead: the next chunk overwrites the dead range byte-for-byte
 * (both are 6 + 2048*6 = 12 294 B), and the gap is reported by the sequence jump.
 *
 * Cost: free on the healthy path (f_tell is the macro (fp)->fptr, ff.h:278 - no
 * I/O; the compare fails out immediately because fptr == vibl_data_end between
 * chunks). On a recovery it costs one FatFs backward seek: f_lseek walks the
 * cluster chain from the first cluster (ff.c:4092-4103) using create_chain(),
 * which returns the EXISTING next cluster without put_fat (ff.c:1369), plus at
 * most one sector-cache write-back and one sector read (ff.c:4141-4148) - both
 * bounded by the 2000 ms HAL timeout and both feeding the IWDG on success AND on
 * failure (src/diskio.c:87, :116). */
static int vibl_rewind_to_data_end(void)
{
    if (vibl_data_end < (uint32_t)VIBL_HEADER_SIZE) {
        return 0;      /* no chunk committed yet: payload starts at 256 by design */
    }
    if (vibl_fil.err != 0u) {
        vibl_fil.err = 0u;   /* else f_lseek answers the stale code, ff.c:4015 */
    }
    if ((uint32_t)f_tell(&vibl_fil) != vibl_data_end) {
        if (f_lseek(&vibl_fil, (FSIZE_t)vibl_data_end) != FR_OK) {
            vibl_fil.err = 0u;   /* leave vibl_close() working: no silent no-op */
            return -1;
        }
        if ((uint32_t)f_tell(&vibl_fil) != vibl_data_end) {
            return -1;           /* size clipped (disk full) or chain damaged */
        }
    }
    return 0;
}

/* Convert the RTC (LSE, BIN format) to milliseconds since Unix epoch. */
static uint64_t vibl_rtc_ms(void)
{
    RTC_TimeTypeDef  sTime;   /* F4 HAL: time and date are separate structs */
    RTC_DateTypeDef  sDate;

    if (HAL_RTC_GetTime(&hrtc, &sTime, RTC_FORMAT_BIN) != HAL_OK) {
        return 0u;
    }
    if (HAL_RTC_GetDate(&hrtc, &sDate, RTC_FORMAT_BIN) != HAL_OK) {
        return 0u;
    }

    uint32_t year = 2000u + sDate.Year;   /* F4 RTC stores year as offset from 2000 */
    int y = (int)year;
    int m = (int)sDate.Month;
    int d = (int)sDate.Date;              /* field is named Date, not Day */

    /* Days since 1970-01-01 (Howard Hinnant civil algorithm). */
    y -= (m <= 2) ? 1 : 0;
    int era = (y >= 0) ? (y / 400) : ((y - 399) / 400);
    unsigned int yoe = (unsigned int)(y - era * 400);                 /* [0,399] */
    unsigned int doy = (153u * ((m + ((m > 2) ? -3 : 9)) % 12) + 2) / 5 + (unsigned int)d - 1u;
    unsigned int doe = yoe * 365u + yoe / 4u - yoe / 100u + doy;       /* [0,146096] */
    uint64_t days = (uint64_t)era * 146097ull + doe - 719468ull;

    uint32_t secs_of_day = ((uint32_t)sTime.Hours * 3600u)
                         + ((uint32_t)sTime.Minutes * 60u)
                         + (uint32_t)sTime.Seconds;
    return days * 86400000ull + secs_of_day * 1000ull;
}

/* -------------------------------------------------------------------------- */
/* Public API                                                                 */
/* -------------------------------------------------------------------------- */

/* One session's preallocation target, single source of truth for BOTH the
 * f_expand() attempt and the f_lseek() fallback:
 *   256 B header + 3600 chunks x (chunk_hdr 6 + 2048 x 6) = 256 + 44 258 400
 * = 44 258 656 B (42.2 MiB) = 3600 x 1.28 s = 4608 s = 76.8 min of payload
 * (a real hour needs 2813 chunks). Clamped to 64 MiB so a small card is never
 * stretched past its capacity. On a 32 GB FAT32 card with 4 KiB clusters this
 * span is 44 258 656 / 4096 = 10 806 clusters. */
static FSIZE_t vibl_prealloc_target(void)
{
    const FSIZE_t chunk_bytes = (FSIZE_t)(sizeof(vibl_chunk_hdr_t)
                                + (VIBL_SAMPLES_PER_CHUNK * VIBL_BYTES_PER_SAMPLE));
    FSIZE_t expected = (FSIZE_t)VIBL_HEADER_SIZE + ((FSIZE_t)3600u * chunk_bytes);
    const FSIZE_t cap = (FSIZE_t)64u * 1024u * 1024u;   /* cap for small cards */

    if (expected > cap) {
        expected = cap;
    }
    return expected;
}

int vibl_open(const char *filename, uint8_t sensor_id, uint8_t range_g, uint16_t gain)
{
    if (filename == NULL) {
        return VIBL_ERR_PARAM;
    }

    /* Build the fixed 256-byte header in a static buffer (no malloc). */
    memset(&vibl_hdr, 0, sizeof(vibl_hdr));   /* zero reserved[] + all fields */

    memcpy(vibl_hdr.magic, VIBL_MAGIC, 4u);
    vibl_hdr.version          = VIBL_VERSION;
    vibl_hdr.sample_rate      = VIBL_SAMPLE_RATE;
    vibl_hdr.axes             = (uint8_t)VIBL_AXES;
    vibl_hdr.bytes_per_sample = (uint8_t)VIBL_BYTES_PER_SAMPLE;

    const char *sname = ((sensor_id < VIBL_SENSOR_COUNT) ? vibl_sensor_names[sensor_id] : "UNKNOWN");
    size_t slen = strlen(sname);
    if (slen >= sizeof(vibl_hdr.sensor)) {
        slen = sizeof(vibl_hdr.sensor) - 1u;   /* keep room for '\0' */
    }
    memcpy(vibl_hdr.sensor, sname, slen);
    vibl_hdr.sensor[slen] = '\0';

    vibl_hdr.range_g = (int8_t)range_g;        /* stored as i8 per spec       */
    vibl_hdr.gain    = (float)gain;            /* gain is LSB/g              */
    vibl_hdr.offset_x = 0;                     /* calibration not part of this API */
    vibl_hdr.offset_y = 0;
    vibl_hdr.offset_z = 0;
    vibl_hdr.start_ts   = vibl_rtc_ms();
    vibl_hdr.record_count = 0u;                /* finalized in vibl_close()   */

    /* header_crc covers bytes [0..251] (everything except the crc field). */
    uint32_t hcrc = crc32_update(0xFFFFFFFFu, (const uint8_t *)&vibl_hdr, VIBL_HEADER_CRC_LEN);
    vibl_hdr.header_crc = hcrc ^ 0xFFFFFFFFu;

    /* Reset per-session state. */
    next_chunk_seq   = 0u;
    total_records    = 0u;
    chunk_count      = 0u;
    total_dropped    = 0u;
    overflow_latched = 0;
    /* V2-1 (LOSSY MODE): seeded at the END of the header instead of 0, because
     * vibl_rewind_to_data_end() (called by every vibl_write_chunk) needs a valid
     * anchor from the first chunk on: at this point fptr == 256 == vibl_data_end,
     * so the guard is a no-op and never seeks to 0. vibl_close()'s zero-chunk
     * branch stays as a belt for the (now unreachable) 0 case. */
    vibl_data_end    = (uint32_t)VIBL_HEADER_SIZE;
    vibl_prealloc_mode = VIBL_PREALLOC_NONE;
    vibl_file_crc    = 0xFFFFFFFFu;            /* running (un-finalized) state */
    vibl_chunk_crc_start = 0xFFFFFFFFu;        /* V2-1 rollback anchor         */
    vibl_dbg_recover_count = 0u;               /* V2-1 new session => fresh log */

    FRESULT fr = f_open(&vibl_fil, filename, FA_CREATE_ALWAYS | FA_WRITE);
    if (fr != FR_OK) {
        vibl_dbg_open_fr = fr;           /* FIX-D: remember the real f_open code */
        return VIBL_ERR_OPEN;
    }
    vibl_dbg_open_fr = fr;               /* FIX-D: FR_OK(0) => file really created */

    /* FIX-D: latch the CD pin state at open time. Together with
     * vibl_dbg_last_fr this identifies the failure class in a single watch read:
     *   last_fr == 9  (FR_INVALID_OBJECT) => disk_status() rejected the drive:
     *                 check PA8 / card detect polarity (diskio.c disk_status()).
     *   last_fr == 1  (FR_DISK_ERR)       => physical SD I/O failure: look at
     *                 hsd.ErrorCode (HAL_SD_ERROR_TIMEOUT / _DATA_TIMEOUT / _TX_UNDERRUN).
     *   last_fr == 7  (FR_DENIED)         => FA_WRITE missing or volume full.
     * Pin semantics per doc/WEACT_F405_PINOUT.md §1: card inserted => PA8 HIGH. */
    vibl_dbg_cd_pa8 = (HAL_GPIO_ReadPin(SD_DETECT_GPIO_Port, SD_DETECT_Pin)
                       == GPIO_PIN_SET) ? 1 : 0;

    /* ---- P3-1: contiguous preallocation attempt, BEFORE any byte is written.
     * The position is not cosmetic: f_expand() rejects a file whose objsize is
     * not zero (ff.c:5153 `if (fsz == 0 || fp->obj.objsize != 0 || !(fp->flag &
     * FA_WRITE)) LEAVE_FF(fs, FR_DENIED);`), and the header write below makes
     * objsize 256 - called after it, f_expand could never succeed.
     *
     * opt = 0 ("Find and prepare") is used deliberately. opt = 1 ("Find and
     * allocate") links the whole run into the FAT immediately AND sets
     * fp->obj.objsize = fsz (ff.c:5208), which puts the 42 MiB tail right back
     * and re-requires the ~10 800-cluster remove_chain() at close - exactly what
     * this patch exists to remove. opt = 0 only proves that a run of `tcl` free
     * clusters exists and parks fs->last_clst just before it (ff.c:5199, :5205);
     * create_chain() then allocates forward from that bias (ff.c:1362,
     * :1400-1414, :1419), so every cluster of the session is the next one in the
     * verified run while objsize stays equal to the bytes actually written.
     *
     * FR_DENIED (no contiguous run of this size, ff.c:5189) is NOT fatal: the
     * session falls back to the legacy f_lseek stretch below. Inspect
     * vibl_dbg_expand_fr and vibl_prealloc_mode in the watch window to see which
     * mode a real card produced. */
    {
        FRESULT fer = f_expand(&vibl_fil, vibl_prealloc_target(), 0);

        vibl_dbg_expand_fr = fer;
        vibl_prealloc_mode = (fer == FR_OK) ? VIBL_PREALLOC_EXPAND
                                            : VIBL_PREALLOC_STRETCH;
    }

    if (vibl_write_all((const uint8_t *)&vibl_hdr, sizeof(vibl_hdr)) != VIBL_OK) {
        /* STOP HERE: inspect vibl_dbg_last_fr / _last_bw / _attempts /
         * _open_fr / _cd_pa8 and hsd.ErrorCode before the file is closed -
         * f_close() below invalidates vibl_fil (ff.c:3832 fp->obj.fs = 0), which
         * is why a later read of the FIL shows a dead object, not the cause. */
        f_close(&vibl_fil);
        vibl_active = 0;
        return VIBL_ERR_WRITE;
    }

    fr = f_sync(&vibl_fil);   /* commit header immediately (power-loss safety) */
    if (fr != FR_OK) {
        f_close(&vibl_fil);
        vibl_active = 0;
        return VIBL_ERR_SYNC;
    }

    /* ---- P3-1 (replaces the P1-7 stretch as the PRIMARY path) ------------
     * Two preallocation modes exist now; whichever one was chosen is recorded
     * in vibl_prealloc_mode by the attempt above.
     *
     * EXPAND mode - f_expand(&vibl_fil, target, opt = 0). objsize is left alone,
     * so the file is always exactly as long as its data: a power cut or an SD
     * error leaves no 42 MiB tail allocated on the card, and close has nothing
     * to release. Clusters are allocated lazily but contiguously, because
     * f_expand parked fs->last_clst at the start of a run it verified to be free
     * (ff.c:5179-5190, :5199, :5205) and create_chain allocates forward from
     * there (ff.c:1362, :1400-1414, :1419).
     * Cost per cluster boundary (4 KiB clusters, ~3 boundaries per 12 294 B
     * chunk): get_fat + 2x put_fat inside the cached FAT window - one 512 B FAT
     * sector flush per ~64 allocations (128 FAT32 entries per sector) - plus
     * FSINFO dirtied, so one extra 512 B write per f_sync (ff.c:954-966):
     * ~4 % metadata per chunk. That is the price for removing the close-time
     * remove_chain() storm. Contiguity is best-effort (the run is not reserved);
     * fragmentation would cost nothing here anyway, because parser_vibl.py walks
     * chunks, not clusters.
     *
     * STRETCH mode - the legacy P1-7 path, kept as the FALLBACK for the case
     * where no contiguous run of this size exists (f_expand -> FR_DENIED,
     * ff.c:5189) or the card/FS refuses it. A single forced-stretch seek pre-links
     * the chain (ff.c:4113) and steady-state recording only follows it (ff.c:1369
     * returns the existing next cluster, no put_fat), but the tail has to be
     * released at close: with 4 KiB clusters on a 32 GB card that is ~10 800
     * put_fat calls, most hitting the same FAT sector, i.e. ~0.3-0.5 s.
     *
     * Target (both modes, from vibl_prealloc_target()): chunk = chunk_hdr(6) +
     * 2048*6 = 12 294 B, 1.28 s per chunk; 256 + 3600*12294 = 44 258 656 B
     * (~42.2 MiB) = 4608 s = 76.8 min of payload. NOTE: the audit labels this
     * "1 hour"; 3600 *chunks* is 76.8 min (a real hour needs 2813 chunks).
     *
     * Geometry note kept from P1-7: the payload starts at VIBL_HEADER_SIZE = 256
     * (sd_writer.h:29 _Static_assert), NOT 512 - the old patch text assumed a
     * 512-byte header sector that this tree does not have, and seeking to 512
     * would punch a 256-byte zero hole between header and payload
     * (parser_vibl.py parses from pos = 256).
     *
     * DEVIATION kept from P1-7: on failure the file is closed AND unlinked and
     * VIBL_ERR_OPEN is returned (instead of the spec's "res = VIBL_ERR_WRITE;
     * goto fail;"), because a half-failed stretch survives f_close() (ff.c:4115
     * only clips the size on a full disk) and a bare close would strand up to
     * 42 MiB per retry; f_unlink() (ff.h:255) releases the chain via
     * remove_chain(). The tail is never zero-filled, so preallocation costs no
     * data write storm at open either way. */
    {
        int prealloc_ok = 1;

        if (vibl_prealloc_mode == VIBL_PREALLOC_EXPAND) {
            /* Do NOT seek past EOF in expand mode: f_lseek() in FA_WRITE mode
             * stretches the chain and sets objsize = fptr (ff.c:4113,
             * :4134-4136), which would re-create the very tail this mode exists
             * to avoid. Verify the geometry only - the header must have landed
             * at 256 and nothing else has been written yet. */
            if (f_tell(&vibl_fil) != (FSIZE_t)VIBL_HEADER_SIZE) {
                prealloc_ok = 0;
            } else {
                /* Expand accepted: payload starts at 256, clusters follow the
                 * verified contiguous run as they are written. */
            }
        } else {
            FSIZE_t expected = vibl_prealloc_target();

            vibl_prealloc_mode = VIBL_PREALLOC_STRETCH;
            if (f_lseek(&vibl_fil, expected) != FR_OK) {   /* stretch the chain once */
                prealloc_ok = 0;
            } else if (f_lseek(&vibl_fil, (FSIZE_t)VIBL_HEADER_SIZE) != FR_OK) {
                prealloc_ok = 0;                           /* back to payload start */
            } else if (f_tell(&vibl_fil) != (FSIZE_t)VIBL_HEADER_SIZE) {
                prealloc_ok = 0;                           /* size clipped (disk full) */
            } else {
                /* Stretch accepted: payload starts at 256, chain pre-linked. */
            }
        }

        if (prealloc_ok == 0) {
            f_close(&vibl_fil);
            vibl_active = 0;
            vibl_prealloc_mode = VIBL_PREALLOC_NONE;
            (void)f_unlink(filename);
            return VIBL_ERR_OPEN;
        }
    }

    vibl_active = 1;
    HAL_IWDG_Refresh(&hiwdg);   /* keep the watchdog fed across open/sync */
    return VIBL_OK;
}

int vibl_write_chunk(const int16_t *samples, uint32_t count)
{
    if ((vibl_active == 0) || (samples == NULL)) {
        return VIBL_ERR_PARAM;
    }
    if ((count == 0u) || (count > VIBL_SAMPLES_PER_CHUNK)) {   /* chunk bound per doc §6 */
        return VIBL_ERR_PARAM;
    }

    /* chunk_hdr per doc §4: seq_number u32 | sample_count u16 (6 bytes).
     * Per-chunk integrity is carried by the running file CRC below (the exact
     * bytes of this chunk are folded into vibl_file_crc), which the trailer
     * validates. This keeps the on-disk layout identical to the PC parser spec. */
    vibl_chunk_hdr_t chdr;
    chdr.seq_number  = next_chunk_seq;
    chdr.sample_count = (uint16_t)count;

    const uint8_t *hdr_bytes = (const uint8_t *)&chdr;
    uint32_t data_len = count * VIBL_BYTES_PER_SAMPLE;   /* count * 6 */

    /* V2-1 (LOSSY MODE) self-aligning guard: a chunk is ALWAYS appended at
     * vibl_data_end, so an abandoned chunk's partial bytes are overwritten by
     * the next chunk instead of leaving an undetectable hole mid-stream.
     * Healthy path cost = one integer compare (f_tell is the macro (fp)->fptr,
     * ff.h:278 - no disk access). If the rewind itself fails the FIL is damaged
     * beyond this chunk: report it and let main.c fall back to the shutdown. */
    if (vibl_rewind_to_data_end() != 0) {
        vibl_stall();          /* unlatch + CRC rollback, no I/O */
        return VIBL_ERR_WRITE;
    }
    vibl_chunk_crc_start = vibl_file_crc;   /* rollback anchor for vibl_stall() */

    if (vibl_write_all(hdr_bytes, sizeof(chdr)) != VIBL_OK) {
        vibl_stall();          /* P1-8: keep the file open for vibl_close() */
        return VIBL_ERR_WRITE;
    }
    if (vibl_write_all(samples, data_len) != VIBL_OK) {
        vibl_stall();          /* P1-8: keep the file open for vibl_close() */
        return VIBL_ERR_WRITE;
    }

    /* Update the running file CRC over exactly what was written. */
    vibl_file_crc = crc32_update(vibl_file_crc, hdr_bytes, sizeof(chdr));
    vibl_file_crc = crc32_update(vibl_file_crc, (const uint8_t *)samples, data_len);

    /* Commit the chunk to SD: power loss corrupts at most this in-flight chunk. */
    FRESULT fr = f_sync(&vibl_fil);
    if (fr != FR_OK) {
        vibl_stall();          /* P1-8: keep the file open for vibl_close() */
        return VIBL_ERR_SYNC;
    }

    next_chunk_seq++;
    total_records += count;
    chunk_count++;
    /* P1-7 companion: remember the true payload end (preallocation made f_size
     * useless for this). Equals VIBL_HEADER_SIZE + written payload bytes. */
    vibl_data_end = (uint32_t)f_tell(&vibl_fil);

    /* Required: feed the IWDG inside the chunk write path. f_sync can stall up
     * to ~200 ms, well under the ~1-1.5 s IWDG timeout, but this guarantees it. */
    HAL_IWDG_Refresh(&hiwdg);

    return VIBL_OK;
}

/* -------------------------------------------------------------------------- */
/* V2-1 (LOSSY MODE, 2026-11-16): recover the writer after an abandoned chunk. */
/* Call order required by main.c: vibl_write_chunk() fails -> main.c marks the */
/* gap -> main.c re-inits the bus with ONE HAL_SD_Init() -> vibl_recover() ->  */
/* next vibl_write_chunk(). No FatFs re-mount anywhere in this sequence:       */
/* re-mounting would bump fs->id (ff.c:3192) and invalidate the open FIL       */
/* (validate(), ff.c:3224) - doc/MILESTONE_2_ROBUSTNESS.md §4.2a.              */
/* -------------------------------------------------------------------------- */
int vibl_recover(void)
{
    if (vibl_active == 0) {
        return VIBL_ERR_NOT_OPEN;
    }
    if (vibl_rewind_to_data_end() != 0) {
        return VIBL_ERR_SEEK;   /* chain damaged / size clipped: not recoverable */
    }
    /* The abandoned chunk's bytes stay inside objsize until either the next
     * chunk overwrites them (normal case) or vibl_close()'s f_truncate() cuts
     * the tail (session ended on a failed chunk). Deliberately NO f_truncate()
     * here: in STRETCH mode it would release the whole preallocated tail
     * (~10 800 put_fat calls, 0.3-0.5 s) and force re-allocation for every
     * following chunk. Only the <= 3 clusters of the dead chunk stay allocated. */
    vibl_dbg_recover_count++;   /* debugger: how many chunks this session survived */
    HAL_IWDG_Refresh(&hiwdg);
    return VIBL_OK;
}

int vibl_mark_gap(uint32_t dropped_count)
{
    if (vibl_active == 0) {
        return VIBL_ERR_NOT_OPEN;
    }
    if (dropped_count == 0u) {
        return VIBL_OK;
    }

    /* Gaps are represented by sequence-number jumps in chunk headers (doc §4/§6):
     * the dropped buffers' seq_numbers are simply skipped, so the next written
     * chunk header carries a higher seq_number and the PC parser reports
     * "seq_number jump > 1". The exact sample count is latched for reporting. */
    uint32_t dropped_chunks = dropped_count / VIBL_SAMPLES_PER_CHUNK;
    if (dropped_chunks == 0u) {
        dropped_chunks = 1u;   /* sub-chunk drop: still make the jump visible */
    }

    next_chunk_seq += dropped_chunks;
    total_dropped  += dropped_count;
    overflow_latched = 1;       /* -> FSM error code 4 (buffer overflow latched) */

    HAL_IWDG_Refresh(&hiwdg);   /* no SD I/O here, but keep the watchdog fed */
    return VIBL_OK;
}

int vibl_close(void)
{
    if (vibl_active == 0) {
        return VIBL_ERR_NOT_OPEN;
    }

    /* 1) Finalize the header in place: record_count + recomputed header_crc.
     *    The two fields are adjacent (offsets 47..50 and 51..54), so a single
     *    8-byte write keeps the update atomic w.r.t. power loss. */
    vibl_hdr.record_count = total_records;
    /* Zero the CRC field before recomputing: header_crc (offset 51..54) is NOT
     * inside [0..251], so this cannot change the result, but it removes any
     * ambiguity about which bytes feed the CRC. */
    vibl_hdr.header_crc = 0;
    uint32_t hcrc = crc32_update(0xFFFFFFFFu, (const uint8_t *)&vibl_hdr, VIBL_HEADER_CRC_LEN);
    vibl_hdr.header_crc = hcrc ^ 0xFFFFFFFFu;

    if (f_lseek(&vibl_fil, offsetof(vibl_header_t, record_count)) != FR_OK) {
        vibl_abort();
        return VIBL_ERR_SEEK;
    }
    /* &record_count points at [record_count(4)][header_crc(4)] (packed, adjacent). */
    if (vibl_write_all((const uint8_t *)&vibl_hdr.record_count, 2u * sizeof(uint32_t)) != VIBL_OK) {
        vibl_abort();
        return VIBL_ERR_WRITE;
    }
    FRESULT fr = f_sync(&vibl_fil);
    if (fr != FR_OK) {
        vibl_abort();
        return VIBL_ERR_SYNC;
    }

    /* 2) Append the trailer at the end of the payload.
     *
     * The seek is required in BOTH preallocation modes: step 1 left the file
     * pointer at 55 (record_count + header_crc), so without
     * f_lseek(vibl_data_end) the trailer would land on top of the header.
     *
     * f_truncate() is kept in BOTH modes on purpose - but in the primary (P3-1
     * EXPAND) mode it is no longer the expensive operation it used to be:
     *   - EXPAND: objsize already equals vibl_data_end for a clean session, so
     *     f_truncate() does nothing at all - its entire body is guarded by
     *     `if (fp->fptr < fp->obj.objsize)` (ff.c:4488), i.e. zero I/O.
     *   - EXPAND with an abandoned chunk (the P1-8 SD-error path): fptr <
     *     objsize there, so it releases only the dead chunk's clusters
     *     (<= 12 294 B = <= 3 clusters at 4 KiB) instead of ~10 800.
     *   - STRETCH (fallback): this is the ~10 800-cluster remove_chain()
     *     (ff.c:4488-4513) of ~0.3-0.5 s, which is why main.c stops TIM2 before
     *     its own vibl_close().
     * Either way objsize ends up == vibl_data_end, so the trailer written next
     * lands exactly at EOF. That invariant is what parser_vibl.py's
     * trailer_candidate() needs (it requires len(data) - pos == 16): any stale
     * bytes after the trailer make the trailer invisible to the parser.
     * A power loss between the truncate and the trailer write leaves
     * size == data end and no trailer, i.e. the parser's
     * "No trailer found (power loss/error)" path - never a stranded trailer. */
    if (vibl_data_end == 0u) {
        vibl_data_end = VIBL_HEADER_SIZE;    /* zero-chunk session */
    }
    if (f_lseek(&vibl_fil, (FSIZE_t)vibl_data_end) != FR_OK) {
        vibl_abort();
        return VIBL_ERR_SEEK;
    }
    if (f_truncate(&vibl_fil) != FR_OK) {   /* cut the preallocated tail */
        vibl_abort();
        return VIBL_ERR_SEEK;
    }

    vibl_trailer_t tr;
    tr.end_ts        = vibl_rtc_ms();
    tr.total_records = total_records;   /* per spec: total record count (parser validates this) */
    tr.file_crc32    = vibl_file_crc ^ 0xFFFFFFFFu;   /* finalize running CRC */

    if (vibl_write_all((const uint8_t *)&tr, sizeof(tr)) != VIBL_OK) {
        vibl_abort();
        return VIBL_ERR_WRITE;
    }
    fr = f_sync(&vibl_fil);
    if (fr != FR_OK) {
        vibl_abort();
        return VIBL_ERR_SYNC;
    }

    HAL_IWDG_Refresh(&hiwdg);

    FRESULT cfr = f_close(&vibl_fil);
    vibl_active = 0;
    return (cfr == FR_OK) ? VIBL_OK : VIBL_ERR_CLOSE;
}

/* -------------------------------------------------------------------------- */
/* State accessors                                                            */
/* -------------------------------------------------------------------------- */
int         vibl_is_open(void)            { return vibl_active; }
uint32_t    vibl_get_total_records(void)  { return total_records; }
uint32_t    vibl_get_chunk_count(void)    { return chunk_count; }
uint32_t    vibl_get_dropped_samples(void){ return total_dropped; }
int         vibl_overflow_latched(void)   { return overflow_latched; }
uint32_t    vibl_get_recover_count(void)  { return vibl_dbg_recover_count; }
/* P2-9 (audit WARN-4): 0 => the RTC was unreadable when the session opened
 * (LSE not running), so header.start_ts and every FAT mtime are meaningless. */
uint64_t    vibl_get_start_ts(void)       { return vibl_hdr.start_ts; }

