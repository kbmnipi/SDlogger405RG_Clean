# MILESTONE 1 — First Complete Recording Cycle (Sensor → Timer → Buffer → SD → CRC MATCH)

**Status:** ACCEPTED. `LOG0003.VIB` closes the whole chain end-to-end with a byte-exact
CRC-32 match. This is the first artifact in this project that proves acquisition,
transport, filesystem and integrity verification all work simultaneously.

**Date of artifact:** `LOG0003.VIB`, 246 152 B, host file mtime 2026-11-15 00:44:09 — host clock,
not the board clock: the board's RTC was never set (see §O-2).
**Firmware state:** single-env build (`genericSTM32F405RG`, effective `-Os` — see §4).
**Sources of truth:** `src/` (CubeMX-generated + hand patches), `doc/WEACT_F405_PINOUT.md` (pin SSOT),
`doc/DS_lis3dsh.md` (sensor SSOT). This file records what actually ran, including where audits were wrong.

---

## 1. Overview

| Item | Value | Evidence |
|---|---|---|
| File size | **246 152 B** | `256 + 20 × 12294 + 16 = 246 152` — exact, no slack |
| Layout | 256 B header + 20 chunks + 16 B trailer | `include/sd_writer.h:31` (`VIBL_HEADER_SIZE = 256`), `:30` (`2048` samples), `:96` (`_Static_assert(sizeof(vibl_trailer_t) == 16)`) |
| Chunk size | 12 294 B = 6 (chunk hdr) + 2048 × 6 | `sizeof(vibl_chunk_hdr_t)` = `seq:u32 + count:u16` = 6; record = 3 × int16 |
| Chunks | **20**, seq 0…19, all `count = 2048` | direct binary walk, no gaps, no seq jumps |
| Records | **40 960** | header `record_count = 40960`, trailer `total_records = 40960` — agree |
| Sample rate | 1600 Hz | header `sample_rate = 1600`; `tim.c:44` PSC=124, `:46` Period=419, APB1Tim 84 MHz → 84e6/(125·420) = 1600.0000 Hz |
| Payload duration | **25.600 s** | 40 960 / 1600 |
| Wall duration | **27.000 s ± 1 s** | trailer `end_ts` − header `start_ts` = 946 684 827 000 − 946 684 800 000 ms. Both stamps end in `.000`: `vibl_rtc_ms()` (`sd_writer.c:202-205`) uses Hours/Minutes/Seconds only and never reads `RTC_TimeTypeDef::SubSeconds`, so board timestamp resolution is **1 s**, not 1 ms |
| Acquisition efficiency | 94.8 % | 25.600 / 27.000 → 1.400 s of I/O + open/close overhead absorbed |
| Header CRC32 | `B9025DA1` → **OK** | recomputed over bytes [0..251] with `[51:55]` zeroed (`parser_vibl.py:26`) |
| **File CRC32** | `9C383A17` → **MATCH** | trailer field == chained `zlib.crc32` over all 20 × (hdr + payload) |
| Dropped samples | 0 | `dropped` counter never fired (`main.c:335`), zero-gap seq chain |

`vibl_data_end` accounting is therefore **byte-exact**: the trailer sits exactly at EOF.

---

## 2. Hardware Verified

### 2.1 LIS3DSH is producing real physical data (not HIL, not garbage)

Header reports `sensor = "LIS3DSH"`, `range_g = 16`, `gain = 100.0`
(`main.c:157` → `vibl_open("LOG0003.VIB", VIBL_SENSOR_LIS3DSH, 16u, 100u)`).

Statistics over all 40 960 records (int16 LSB, little-endian, from the file itself):

| Axis | mean | σ | min | max | unique values |
|---|---|---|---|---|---|
| X | +30.93 | 22.49 | −414 | +204 | 165 |
| Y | +104.04 | 22.38 | −64 | +322 | 154 |
| **Z** | **−1403.61** | 27.41 | −3001 | −328 | 183 |

**Why this proves a real sensor:**
* At ±16 g the sensitivity is **0.73 mg/LSB** (`doc/DS_lis3dsh.md:94` FSCALE=100 row, rule `:102`,
  `±16 g: 0.73 mg/LSB` `:103`). Z mean -1403.61 LSB × 0.73 = **-1024.6 mg ≈ -1.025 g** → 1 g of
  gravity on Z. Resultant √(X²+Y²+Z²) = 1407.8 LSB = **1.028 g**, tilt off-Z **4.4°**. A static
  board, Z up, canted ~4°: pure noise cannot give a 1.00 g resultant with σ(Z) = 27 LSB.
* First 8 Z samples: -1380, -1407, -1412, -1409, -1404, -1435, -1420, -1423 - a random
  walk around -1404: not a constant (stuck bus), not 0xFFFF (dead SPI, cf. the WHO_AM_I
  retry comment in `lis3dsh.c`).
* sigma(Z) = 27.4 LSB = 20 mg rms, 183 distinct Z values. `CTRL_REG4_VAL = 0x9F` -> ODR
  `1001` = **1600 Hz** (`lis3dsh.c:33`; `doc/DS_lis3dsh.md:404`): the sensor output rate
  EQUALS the TIM2 read rate, so every sample is a fresh conversion, not a re-read.
  Expected noise: 150 ug/sqrt(Hz) at +-2 g (`doc/DS_lis3dsh.md:98`) -> ~1.2 mg/sqrt(Hz) at
  +-16 g over the 800 Hz anti-alias band (`lis3dsh.c:34`, `CTRL_REG5_VAL = 0x20`, DS `:460`)
  -> 1.2*sqrt(800) = 34 mg predicted vs 20 mg measured: a live, mechanically unexcited sensor.

*SPI path confirmed working:* `lis3dsh.c:148` burst command byte `0xA8 = 0x80 | OUT_X_L(0x28)`
  (compile-time guard `lis3dsh.c:43-45`), CS on **PA4** (`lis3dsh.c:8-9`), SPI1 on
PA5/PA6/PA7 (`spi.c:76-77`). Pin assignment per `doc/WEACT_F405_PINOUT.md` §6
("SPI1_SCK **PA5**", "LIS3DSH_CS **PA4**").

### 2.2 SD card works — 1-bit SDIO at `ClockDiv = 16`

Runtime configuration (`src/sdio.c`):
```
hsd.Init.BusWide        = SDIO_BUS_WIDE_1B;   // sdio.c:45
hsd.Init.ClockDiv       = 16;                 // sdio.c:47
// HAL_SD_ConfigWideBusOperation(&hsd, SDIO_BUS_WIDE_4B)  // sdio.c:52-55 — commented out
```
`disk_initialize()` deliberately does **not** re-widen the bus (`diskio.c:41-44`, HIL-1 note):
only `HAL_SD_Init(&hsd)` is called (`diskio.c:37`).

**Clock math.** HAL: `SDIO_CK = SDIOCLK / (ClockDiv + 2)`
(`Drivers/STM32F4xx_HAL_Driver/Src/stm32f4xx_hal_sd.c:82`). With SDIOCLK = 48 MHz
(`SDlogger405RG_Clean.ioc`: `RCC.PLLQCLKFreq_Value=48000000`, `RCC.PLLQ=7`):
SDIO_CK = 48e6 / (16+2) = **2.6667 MHz** → 1-bit ceiling 2.6667 Mbit/s = **0.333 MB/s**.

| Sustained data rate needed | 1600 samples/s × 6 B = **9.6 kB/s** |
| 1-bit ceiling at 2.6667 MHz | 333 kB/s → **34.7× headroom** |
| Pure transfer time, one 12 294 B chunk | 12294 × 8 / 2.6667e6 = **36.9 ms** |

> **NOTE (superseded claim, flagged not merged):** `doc/audit_13_11_26.md:422` quotes “1-bit SDIO
> 1.3125 MHz (SDIO_CK = 42 MHz/(2·16+2))” — that value is internally inconsistent (42e6/34 =
> 1.235 MHz; 1.3125 MHz corresponds to 42e6/32) and uses the APB1 clock. SDIO is clocked from
> PLLQ ≤ 48 MHz (`stm32f4xx_hal_rcc.c:138-139` “The second output is used to generate the clock
> for the USB OTG FS (48 MHz) … and the SDIO (<= 48 MHz)”) and the HAL divisor
> form is `+2`, so the correct figure is **2.6667 MHz**. Newer value wins; the audit keeps the
> old one for history. Net effect: headroom is 2× larger than the audit assumed.

> **NOTE (doc/code contradiction, do not silently merge):** `SDlogger405RG_Clean.ioc:89-107`
> still labels PC8..PC11 `Mode=SD_4_bits_Wide_bus` while the code runs `SDIO_BUS_WIDE_1B`.
> The `.ioc` value is a CubeMX pin-assignment label; the electrical width is set only by
> `Init.BusWide` / `HAL_SD_ConfigWideBusOperation`, and both 4-bit calls are disabled, so
> **runtime truth = 1-bit** and D1/D2/D3 are idle. Regenerating from the `.ioc` will not
> switch the bus back, and will not warn if someone re-enables `sdio.c:52`.

Card detect (PA8) is **Active-HIGH = inserted**: `diskio.c:24-26` (`GPIOA->IDR & GPIO_PIN_8`),
`main.c:130` (`GPIO_PIN_RESET` => absent => `hil3_error_loop(2)`), SSOT
`doc/WEACT_F405_PINOUT.md` §1 note 2 ("карта вставлена → PA8 HIGH"). Bench: 2.5 V inserted /
0.4 V absent (`diskio.c:24`). Opposite to the original rule text ("Active LOW"); the updated
pinout file is authoritative.

---

## 3. Critical Code Fixes — the hard-won knowledge

### 3.1 THE fix: `HAL_Delay(10)` was reintroduced after `HAL_SD_WriteBlocks`

`src/diskio.c:92-123` in `disk_write()`:

```c
    if (HAL_SD_WriteBlocks(&hsd, (uint8_t *)buff, sector, count, 1000U) != HAL_OK) {
        return RES_ERROR;
    }
    /* FIX-D ... REINTRODUCED after P0-3 ... */
    HAL_Delay(10);
    HAL_IWDG_Refresh(&hiwdg);
    return RES_OK;
```

**Audit patch that had to be reversed.** P0-3 (`doc/audit_13_11_26_1.md` BUG-2, section
`:113-117`, patch target `diskio.c:76` at the time) deleted
this delay, arguing: *"HAL_SD_WriteBlocks returns only after SDMMC_FLAG_BTC /
PROGRAMME_READY, i.e. the card is no longer BUSY"*, and quantified the cost as
"~24–25 calls/chunk × 10 ms ≈ 250 ms of dead sleep per 12 KB chunk".

**Why the argument is false for this HAL.** Verified against
`Drivers/STM32F4xx_HAL_Driver/Src/stm32f4xx_hal_sd.c` (framework-stm32cubef4 @ 1.28.3):

* The polling loop of `HAL_SD_WriteBlocks()` exits on **`SDIO_FLAG_DATAEND`** — the SDIO DPSM
  finished draining its FIFO (`:858`, condition `TXUNDERR | DCRCFAIL | DTIMEOUT | DATAEND`).
  DATAEND ≠ the card finished programming.
* `HAL_SD_WriteBlocks()` spans `:784-961` and makes **no `HAL_SD_GetCardState()` call**; the only
  calls in the file are in the abort path (`:2391, :2454, :2572, :2603, :2642`). HAL pushes the
  duty onto the caller instead: “This API should be followed by a check on the card state through
  HAL_SD_GetCardState()” (`:776`). That getter (`:2322-2337`) sends CMD13 (`SD_SendStatus`) and
  decodes `resp1 >> 9 & 0xF`; programming = `HAL_SD_CARD_PROGRAMMING = 0x07`
  (`Inc/stm32f4xx_hal_sd.h:75`). Nothing in this project reads it.

**Observed failure mode without the delay.** The next command in line (next sector of the same
chunk, or FatFs FAT/dir writeback) is issued while the 32 GB card is still internally busy; it
gets no response, `HAL_SD_WriteBlocks()` returns `HAL_ERROR`, `disk_write()` returns `RES_ERROR`,
FatFs surfaces `FR_DISK_ERR` — the mapping documented at `sd_writer.c:267-268`.
*UNCERTAIN: the first SDIO flag (CTIMEOUT vs DATA_TIMEOUT) was not captured; recovery
signature was identical for all of them.*

**Why a blind delay instead of a bounded `GetCardState` poll.** The audit fix
(`audit_13_11_26_1.md:117`: bounded `HAL_SD_GetCardState(&hsd)` poll at `:2322` +
`HAL_IWDG_Refresh`) was **never applied** (see §3.2). A blind 10 ms wait is free:
nothing that the acquisition timeline does not already have:

| Per-chunk I/O budget | Value |
|---|---|
| `disk_write` calls per 12 294 B chunk | 24 full sectors (12 288 B) + 6 B remainder = **25**; unaligned head/tail plus FatFs dirty-window writeback (`_FS_TINY = 0`, `include/ffconf.h:49`) keeps it at 25 calls |
| Delay component | 25 × 10 ms = **250 ms** |
| Transfer component | **36.9 ms** |
| Total | **≈ 287 ms** |
| Time to fill the *next* chunk at 1600 Hz | 2048 / 1600 = **1280 ms** |
| Writer duty | 287 / 1280 = **22.4 %** → consumer is 4.5× faster than producer |

So the double-buffered arenas (`main.c:62-65`, `full_mask`, `VIBL_CHUNK_WORDS` at `main.c:61`) never
overflow: `dropped` stayed 0 → 20/20 chunks, zero gaps. **P0-3’s arithmetic was right; only its
conclusion was wrong** — that 250 ms/chunk of “dead” sleep is exactly what keeps a 32 GB card’s
internal program/erase stall from colliding with the next command.
with the next command.

**Documentation debt closed:** the comment above `HAL_Delay(10)` still said "REMOVED" while
the statement was present; it is now FIX-D and matches the code. Comment-only change, verified
by rebuild: `pio run -e debug` -> Flash 34 876 B / RAM 27 440 B, identical to before.

### 3.2 Rejected hypothesis: "SDIO state lock (`HAL_SD_STATE_ERROR`)"

An intermediate theory claimed `HAL_SD_WriteBlocks` leaves `hsd.State = HAL_SD_STATE_ERROR` on
failure, so retries fail instantly with `hsd.ErrorCode = 0`, and that `HAL_SD_Init(&hsd)` inside
`disk_write()` was needed to clear it. **Refuted for this HAL version** — three independent checks:

1. `HAL_SD_STATE_ERROR = 0x0000000FU` is *defined* (`Inc/stm32f4xx_hal_sd.h:58`) but
   **never assigned anywhere** in `stm32f4xx_hal_sd.c` (0 grep hits). Every error path of
   `HAL_SD_WriteBlocks` sets `hsd->State = HAL_SD_STATE_READY`
   (`:848, :887, :905, :922, :931, :940, :952`).
2. The entry guard is `if(hsd->State == HAL_SD_STATE_READY)` (`:799`); the else-branch
   (`:956-959`) adds `HAL_SD_ERROR_BUSY` — but it can only be reached from `BUSY`,
   which the same function always clears. On an accepted call `ErrorCode` is reset to
   `HAL_SD_ERROR_NONE` at `:801`, so "retries see ErrorCode = 0" is not a HAL behaviour.
3. `arm-none-eabi-nm -u` on `diskio.o` (both `debug` and `release`) lists exactly:
   `HAL_SD_Init`, `HAL_SD_ReadBlocks`, `HAL_SD_WriteBlocks`, `HAL_Delay`, `HAL_IWDG_Refresh`,
   `HAL_RTC_Get{Time,Date}`, `hsd`, `hiwdg`, `hrtc`. **No `HAL_SD_GetCardState`, no
   `HAL_SD_DeInit`**; `HAL_SD_Init` appears only in `disk_initialize()` (`diskio.c:37`).

**Verdict:** do not add a `HAL_SD_Init()` or `GetCardState()` call inside `disk_write()`.
A full `HAL_SD_Init()` per failed sector would re-run the whole card enumeration:
`SD_PowerON` (`stm32f4xx_hal_sd.c:432`) -> `SD_InitCard` (`:441`) -> `SDMMC_CmdBlockLength`
(`:450`) - hundreds of ms of CMD0/CMD8/ACMD41/CMD2 traffic per failed sector, and it
would also re-enter the card idle sequence while the card is mid-programming: a bigger
risk than the 10 ms it saves. Current code is the minimal correct fix.

### 3.3 Timeouts raised 200 ms → 1000 ms (both directions)

`diskio.c:67` (`HAL_SD_ReadBlocks`, `1000U`) and `diskio.c:88` (`HAL_SD_WriteBlocks`,
`1000U`); previously 200U under the old watchdog budget.

The bound is legal only because P0-4 lengthened the watchdog:
`iwdg.c:45-46` → `IWDG_PRESCALER_32`, `Reload = 4095` →
**T_IWDG = 4095 × 32 / 32000 = 4.095 s** (`iwdg.c:42`).

Worst-case accounting, with the bounded chunk retry (`VIBL_WRITE_RETRY_MAX = 3u`,
`sd_writer.c:88`, loop at `:129`, watchdog re-feed at `:155-157`):

```
3 attempts × 1000 ms  = 3.000 s  <  T_IWDG 4.095 s          (safe, 1.095 s margin)
1 op + 10 ms delay + HAL_IWDG_Refresh after every op          (no cross-op span can starve)
```
Note the HAL `Timeout` argument only bounds the FIFO-drain loop (`:882`); card-internal
program/erase stalls are bounded by `SDMMC_DATATIMEOUT` inside the peripheral
(`config.DataTimeOut`, `:820`). The 1 s bound protects against a wedged bus, not the card stall.

### 3.4 Preallocation (P1-7) — works, and the tail is reclaimed at close

`vibl_open()` pre-links the cluster chain once (`sd_writer.c:322-355`):

```
expected = 256 + 3600 × 12294 = 44 258 656 B  (~42.21 MiB)   sd_writer.c:331-332
cap      = 64 MiB clamp                                       sd_writer.c:333-338
f_lseek(expected) → f_lseek(256) → f_tell() == 256            sd_writer.c:339-344
```
All three succeeded (`prealloc_ok` stayed 1; the failure branch `sd_writer.c:349-354`, which closes
+ `f_unlink()` + returns `VIBL_ERR_OPEN`, was not taken — the file exists, `hil3_error_loop(3)` did not fire).

**Why the seek back to 256 matters:** the tree has no 512-byte header sector; the header is
exactly `VIBL_HEADER_SIZE = 256` (`sd_writer.h:31`, `_Static_assert` at `:76`), so seeking to 512
would have punched a 256-byte zero hole (`sd_writer.c:325-330`). The artifact proves it: seq 0 is at byte 256.

`vibl_close()` then cuts the never-written tail (`sd_writer.c:487-497`):
`f_lseek(vibl_data_end)` → **`f_truncate()`** → trailer written → lands at EOF →
`f_sync()`. Order is power-loss-atomic (audit W-2): a crash between truncate and trailer
leaves `size == data end` and **no** trailer = the parser's power-loss path, never a
stranded trailer 42 MB deep.

| Quantity | Value |
|---|---|
| Preallocated length | 44 258 656 B (42.21 MiB) |
| Final length | 246 152 B |
| Tail reclaimed at close | **44 012 504 B (41.97 MiB)** |
| FAT clusters released (4 KiB clusters) | 10 806 → 61 ⇒ **10 745** (recomputed from the real sizes; the code comment at `sd_writer.c:307` says “~10 300”) |
| Truncate cost | ~0.3–0.5 s (`sd_writer.c:305-309`: most updates hit the same FAT sector) |

Because of that cost, `HAL_TIM_Base_Stop_IT(&htim2)` runs **before** `vibl_close()` (`main.c:191`,
deviation noted at `main.c:186-190`): the 1600 Hz ISR stops before the 0.3–0.5 s FAT drain, so no
new `full_mask`/`dropped` update races the snapshot at `main.c:194-198`.

Preallocation label: 3600 **chunks** = 76.8 min of payload, not 1 h (`sd_writer.c:302-303`);
a real hour needs 2813 chunks.

---

## 4. Build System — split into `release` / `debug`

`platformio.ini` no longer has a single `[env:genericSTM32F405RG]`: a base `[env]` (platform/board/
framework/upload/debug/defines/`build_unflags`) plus children `[env:release]` (`-O2 -g -fno-fast-math
-funroll-loops`) and `[env:debug]` (`-O0 -g3 -fno-fast-math -funroll-loops`); `default_envs = debug`.

**The critical fix: `build_unflags = -Os`.** The ststm32 build script appended its own
`-Os` **after** `build_flags` on every compile line, and GCC's "last `-O` wins" made the
framework silently override the project's level. This was `final_pre_flash_audit.md`
item **I5 / W-1**, left as WARN because it was "untested on ststm32 20.0.0". It is now
tested and fixed:

| Profile (clean build, PIO Core 6.2.0, ststm32 20.0.0, gcc-arm-none-eabi 1.70201.0) | result |
|---|---|
| `pio run -e release -v` | 107 `-c` compile lines: `-O2` in **107/107**, ` -Os ` in **0**, `-DHSE_VALUE=8000000` in **107/107**, `-ffast-math`/`-Ofast` **0**. RAM 27 424 B (20.9 %), Flash **24 584 B** |
| `pio run -e debug -v` | 107 lines: `-O0` in **107/107**, ` -Os ` in **0**, HSE define 107/107, no fast-math. RAM 27 440 B (20.9 %), Flash **34 876 B** |
| Old single env (produced `LOG0003.VIB`) | Flash 20 340 B / `firmware.bin` 20 748 B — **effective `-Os`**, not `-O2` |

**Consequence for Milestone 1:** the CRC/functional result is optimization-independent (byte-exact
format; the only float in the path is the stored calibration gain, `sd_writer.c:236`), but **no
timing number measured on that build may be quoted as acceptance data** — it was measured at `-Os`.
The 22.4 % writer duty in §3.1 is a transfer-cost budget, not a CPU measurement, so it survives.

**Inheritance trap (measured, not assumed):** a child `build_flags` **replaces** the parent's
instead of appending. Deleting `-D...` from the children removed `-DHSE_VALUE=8000000` from
107/107 compile lines, so the defines are duplicated in both children (re-verified with
`pio project config`). `${env.build_flags} ...` works in PIO 6.2 (probe-verified) but is not
used, for readability.

**`.vscode/launch.json`** was regenerated with `pio init --ide vscode -d .` and now points
at `projectEnvName: "debug"` / `.pio/build/debug/firmware.elf`. Backups:
`doc/launch_json_backup_before_pio_init.json`,
`doc/c_cpp_properties_backup_before_pio_init.json`,
`doc/platformio_ini_backup_20250113.txt` (pre-split), `doc/platformio_ini_split_backup.txt`.

*Gotcha:* the `PIO Debug` configuration has `preLaunchTask: PlatformIO / Pre-Debug` —
F5 will **rebuild and re-flash** the `-O0 -g3` image over whatever is on the board.
Use `PIO Debug (without uploading)` for a passive attach.

---

## 5. Open Issues (next sprint)

### O-1 — Parser emits a cosmetic "File truncated" warning on a *complete* file

Actual output on a valid file:
```
✅ Header CRC: OK
⚠️ Warning: File truncated during chunk 1792021880. Ignoring incomplete tail.
...
Number of Chunks: 20 | Total Records: 40960 | Gaps Found: No
File CRC32: 9C383A17 -> ✅ MATCH
```
Root cause (`parser_vibl.py:50-70`): the chunk loop reads the **16-byte trailer as a chunk
header** — `struct.unpack('<IH', trailer[0:6])` reinterprets `end_ts` (uint64) as
`seq:u32 = end_ts & 0xFFFFFFFF = 1 792 021 880`, `count:u16 = (end_ts >> 32) & 0xFFFF = 220`.
No guard fires (`count != 0`, `count ≤ 2048`, `seq` monotonically > 19), so it falls through to
the length check. The loop breaks with `pos` unchanged, so the trailer is still parsed and CRC
matches — **cosmetic**, but it reads like data loss in logs.

Fix direction (parser only, no C change): treat a candidate whose `seq != len(chunks)` as a
trailer, or probe the trailing 16 bytes before entering the loop. The monotonicity guard
(MISS-1 companion) cannot help: the low 32 bits of a ms epoch are always >> chunk index.

### O-2 — RTC is never set: file timestamps read as year 2000

`MX_RTC_Init()` (`rtc.c:30-56`) configures only prescalers
(`AsynchPrediv = 127`, `SynchPrediv = 255`, LSE source per `rtc.c:73`); there is **no
`HAL_RTC_SetDate` / `HAL_RTC_SetTime` call anywhere in `src/`** (HAL prototypes exist at
`stm32f4xx_hal_rtc.c:865` and `:706`). Consequence: `RTC_DateTypeDef.Year = 0` = offset
from 2000 (`sd_writer.c:189` `year = 2000u + sDate.Year`) ⇒
`start_ts = 946 684 800 000` (2000-01-01T00:00:00Z) and `end_ts` = start + 27 s, and FAT
directory stamps are equally bogus (`diskio.c:177-194` `get_fattime`, `(d.Year + 20) << 25`).

**The existing guard does not catch this.** `main.c:165` fails only when
`vibl_get_start_ts() == 0u`, but an unset RTC yields a *valid non-zero* epoch-2000 stamp.
So "RTC fault → `hil3_error_loop(6)`" protects against a dead LSE, not against an
unset clock. Gap to close: either set date/time at startup (hard-coded default + host
`--set-rtc` path), or gate on a sentinel date (e.g. `Year == 0 && (Hours|Minutes|Seconds) == 0`).

Note: the 27 s wall duration in §1 stays **internally consistent** (both stamps come from the
same running RTC) — only the absolute date is wrong. `end_ts − start_ts` is the number to trust.

### O-3 — Timing acceptance must be re-run at `-O2`

See §4: session length, writer duty, `vibl_close()` drain time and ISR jitter all come
from a `-Os` build and a 27 s session. Required after `pio run -e release -t upload`: (a) ≥ 5 min
session + PC13 graceful stop (`final_pre_flash_audit.md` M1/M2/M8); (b) power-cut mid-session →
parser must report “No trailer found (power loss)” in < 1 s; (c) a 76.8-min session to exercise
the `expected` preallocation clamp.

### O-4 — SPI1 speed margin must be re-verified before claiming vibration bandwidth

Sensor and sampler are already rate-matched: `CTRL_REG4_VAL = 0x9F` -> ODR = 1600 Hz
(`lis3dsh.c:33`, DS table `doc/DS_lis3dsh.md:404`) and TIM2 = 1600 Hz. NOT yet proven: that a
7-byte burst read fits the 625 us ISR at 1600 Hz on this SPI config. `spi.c:47` ->
`SPI_BAUDRATEPRESCALER_64`, APB2 = 84 MHz (.ioc `RCC.APB2Freq_Value=84000000`) -> SCLK
1.3125 MHz -> 56 bit = 42.7 us + ~10 us HAL = ~53 us = **11.8 %** of the ISR window (same
figure as the P2-8 comment at `lis3dsh.c:37-40`). Never measured with DWT, and PC13 EXTI15_10
shares the ISR family at priority 3 (`main.c:142`) while TIM2 is priority 1 (.ioc:63). TBD:
instrument with DWT before making bandwidth claims; do NOT lower the prescaler without a
pinout revision (audit WARN-7, `doc/audit_13_11_26.md:486-492`, proposes SPI2/APB1 = a pin move).

---

## 6. Evidence / reproduction commands

```text
python parser_vibl.py LOG0003.VIB
pio project config                                   # env inheritance + build_unflags
pio run -e release -v | Select-String "arm-none-eabi-gcc -o.* -c "   # -O2, no -Os, -DHSE_VALUE
pio run -e debug   -v | Select-String "arm-none-eabi-gcc -o.* -c "   # -O0,  no -Os, -DHSE_VALUE
arm-none-eabi-nm -u .pio\build\debug\src\diskio.o   # HAL API surface actually referenced
arm-none-eabi-readelf -S .pio\build\debug\firmware.elf   # .debug_info/.debug_macro => -g3
Select-String -Path Drivers\STM32F4xx_HAL_Driver\Src\stm32f4xx_hal_sd.c -Pattern "HAL_SD_STATE_ERROR"   # 0 hits
```

**Deliberately NOT used in either profile:** `-Ofast` / `-ffast-math` (implies `-ffast-math` → the
float gain at `sd_writer.c:236` / `include/sd_writer.h:65` stops being bit-reproducible; audit
WARN-5), `-flto` (inlines the ISR path, invalidating measured cycle budgets), `-DCMSIS_DSP` (P2-10).

---

## 7. What changed in the docs/rules after this milestone

| Claim | Old position | Now | Basis |
|---|---|---|---|
| `HAL_Delay(10)` in `disk_write` | Remove (P0-3, BUG-2) | **Keep, documented as FIX-D** | §3.1, HAL `:858` / `:776` |
| SD retry failure cause | `hsd.State` stuck in `HAL_SD_STATE_ERROR` | **Refuted** | §3.2, enum never assigned; `nm` evidence |
| Write/read timeout 200 ms | sized for T_IWDG = 1.023 s | **1000 ms** | §3.3, `iwdg.c:42,45-46` |
| SDIO bus width | 4-bit (rule text + `.ioc` labels) | **1-bit, `ClockDiv = 16`** | `sdio.c:45,47,52-55`; `diskio.c:41-44` |
| SDIO_CK | 1.3125 MHz | **2.6667 MHz** | HAL `:82` + `.ioc PLLQ=7 → 48 MHz` |
| Card detect PA8 | Active LOW | **Active HIGH = inserted** | `diskio.c:24-26`; pinout §1 note 2 |
| Effective `-O` level | `-O2` (spec) | **was `-Os`; now `-O2`/`-O0` via `build_unflags`** | §4 |
| `TIM2` stop vs `vibl_close()` | close first | **stop timer first** | `main.c:186-191` |
