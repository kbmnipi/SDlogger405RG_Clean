# Final Pre-Flash Audit
Date: 2026-11-14
Mode: AUDIT (read-only — no source file modified)
Files checked (read once, rule 17): `doc/audit_13_11_26_1.md`, `doc/WEACT_F405_PINOUT.md`, `doc/DS_lis3dsh.md`, `src/main.c`, `src/lis3dsh.c`, `src/sd_writer.c`, `include/sd_writer.h`, `src/diskio.c`, `src/iwdg.c`, `src/stm32f4xx_it.c`, `include/stm32f4xx_it.h`, `include/ffconf.h`, `include/ff.h`, `src/ff.c` (targeted greps only), `src/gpio.c`, `src/sdio.c`, `src/spi.c`, `src/tim.c`, `src/rtc.c`, `include/main.h`, `include/lis3dsh.h`, `platformio.ini`, `parser_vibl.py`, `SDlogger405RG_Clean.ioc`, `doc/ANSWER_TO_AUDIT.md`, `test/*` (+ `pio run` / `pio run -v`, `nm`).
Deliverable = this file. Extra artefacts created during verification: `build_verbose.txt`, `build_verbose2.txt`, `clean.log`, `clean.log` in the project root (build logs, not source; safe to delete).

## ADDENDUM 2026-11-15 (rule 15 — the newer artefacts are authoritative)

The audit snapshot below is dated 2026-11-14 and is kept **verbatim** as the historical record; it is corrected by annotation, not by rewriting the findings. Three statements in it have since been superseded by patches applied on 2026-11-15, verified in-tree (builds + synthetic fixtures + grep gates; **no flashing, no bench — rule 18**):

| Audit statement (2026-11-14) | Status now (2026-11-15) | Authoritative source |
|---|---|---|
| Check **C3 = FAIL** / **BLOCKER-1**: `fill_cnt += 3u` burned the slot on the SPI-error path and returned before the wrap check (`main.c:320-325` in that revision) | **CLOSED / RESOLVED** — one shared advance+wrap block serves both paths; the acceptance gate `grep -n "fill_cnt += 3u" src/main.c` now returns **exactly 1 line** (`src/main.c:438`, immediately before `if (fill_cnt >= VIBL_CHUNK_WORDS)` at `:439`) | `src/main.c:398-448`, `doc/MILESTONE_2_ROBUSTNESS.md` §2 |
| Parser's power-loss path recognised only an **all-0x00** tail; an **all-0xFF** tail fell through `count > 2048` and the 16 bytes were mis-probed as a trailer → bogus `File CRC32: FFFFFFFF → ❌ MISMATCH` | **FIXED** — 0x00 tails, 0xFF tails, garbage headers and partial chunks all route to `⚠️ No trailer found (power loss/error)`; chunk statistics and `--plot` still work; 44 MiB fixture parses in **0.49 s** | `parser_vibl.py:46-57, 83-97, 111-131, 148-177`; fixtures `test/SYNTH_FFTAIL.VIB`, `test/SYNTH_TRUNC_PARTIAL.VIB`, generator `test/mk_synth_error_tail.py` |
| A `vibl_write_chunk()` failure called `hil3_error_loop(5)` directly (file abandoned mid-session: no trailer, 42 MiB preallocated tail left allocated, partial chunk still punched into the stream) | **SUPERSEDED** — `sd_error_shutdown()`: up to 3 × `HAL_SD_Init()` → `vibl_close()` (truncate at `vibl_data_end` + trailer) → stop TIM2 → `hil3_error_loop(5)` | `src/main.c:129-168, 317-332`, `src/sd_writer.c:177-198`, `doc/MILESTONE_2_ROBUSTNESS.md` §4 |

Consequences for this audit's own conclusions: **"M4 is gated by BLOCKER-1" no longer applies** (M4 is hardware-pending only), and the flash gate now reads **0 open blockers**. Source files WERE modified on 2026-11-15; this file, written by a read-only audit, is annotated only.

**Line-number drift warning (rule 15 hygiene).** Every `main.c:NNN` citation in the tables below refers to the 2026-11-14 revision and is now offset, because the Milestone-2 patch inserted lines: **+6** from the PV block onward (`FATFS fs;` 53 → 59, `arena[2]` 62 → 68, `fill_idx/fill_cnt/full_mask/dropped` 63-66 → 69-72), **+82** from `int main(void)` (93 → 175) through the flush site (`vibl_mark_gap` 238 → 331), **+91** for the TIM2 callback and the blink loop (`HAL_TIM_PeriodElapsedCallback` 307 → 398, `hil3_error_loop` 345 → 451). Current-state citations are given in the annotations and in `doc/MILESTONE_2_ROBUSTNESS.md`; the original numbers are left untouched so the 2026-11-14 evidence stays auditable. `sd_writer.c` citations drift by **+1** (`vibl_dbg_sd_error` inserted at `:94`, so `vibl_abort` 168 → 169) and by **+24** from that insertion onward (`vibl_close` 442 → 466, `f_truncate` 494 → 518).

## Summary table

| ID | Check | VERDICT | Evidence (file:line) |
|----|-------|---------|----------------------|
| A1 | TIM2 → 1600.0000 Hz | PASS | `tim.c:44` PSC=124, `tim.c:46` Period=419; `.ioc` `APB1TimFreq_Value=84000000` → 84e6/(125·420)=1600.0000 Hz, 0 ppm (HSE 8 MHz, pinout §3) |
| A2 | SYSCLK 168 MHz | PASS | `main.c:216-222` PLLM=4, PLLN=168, PLLP=2 (8/4=2 MHz ×168 /2 = 168); `.ioc` `SYSCLKFreq_VALUE=168000000` |
| B1 | Burst byte 0xA8, no 0x40 in command | PASS | `lis3dsh.c:30-31` `SPI_RW_ADDR = SPI_READ_BIT` (0x80), `lis3dsh.c:131` `tx[7] = { OUT_X_L_ADDR \| SPI_RW_ADDR …}` = 0xA8; `_Static_assert` at `lis3dsh.c:42-44` pins it; DS §5.2 "There is no MS/auto-increment bit inside the SPI command byte"; DS §10 "SPI read: send (0x80 \| addr)" |
| B2 | Auto-increment via CTRL_REG6.ADD_INC | PASS | `lis3dsh.c:33` CTRL_REG6_VAL=0x10; DS §7.25 default 0x10, ADD_INC = bit 4 |
| B3 | Init order REG6→REG5→REG4 | PASS | `lis3dsh.c:106,107,108` (0x25→0x24→0x20); DS §10 steps 3→4→5 |
| B4 | CTRL_REG4 read-back, `(rb & 0xF8)==0x98`, −2 on mismatch | PASS | `lis3dsh.c:119-121`, read-back placed after all three writes |
| B5 | SPI timeout 1 ms (19× margin) | PASS | `lis3dsh.c:38` `LIS3DSH_SPI_TIMEOUT_MS 1u`; frame = 56 bit / 1.3125 MHz = 42.7 µs + ~10 µs HAL = ~53 µs |
| B6 | SPI mode 3, presc 64 → 1.3125 MHz | PASS | `spi.c:43-44` POLARITY_HIGH + PHASE_2EDGE; `spi.c:47` PRESCALER_64; `.ioc` `SPI1.CalculateBaudRate=1.3125 MBits/s` (SPI1 on APB2 = 84 MHz); DS limit 10 MHz → 7.6× margin |
| C1 | `full_mask`+`dropped`+`arena[2][6144]` | PASS | `main.c:62,64,65,66` (VIBL_CHUNK_WORDS = 2048·3 = 6144 int16 = 12 288 B/arena) |
| C2 | Old names absent | PASS | `powershell Select-String src\main.c 'buf_ready|pingpong_buf|sample_idx|active_buf'` → **0** |
| C3 | ISR error path: burn slot + shared wrap, no OOB | **CLOSED 2026-11-15** (was **FAIL**) | Patch: `main.c:419` reads the sensor once into `rc`, `main.c:420-433` handles streak only, `main.c:438` `fill_cnt += 3u;` is the **single** advance for BOTH paths, `main.c:439-447` is the shared wrap. Gate `grep -n "fill_cnt += 3u" src/main.c` → **1 line**. Original defect evidence (kept for the record): `main.c:320` `fill_cnt += 3u;` + `main.c:325` `return;` — the wrap check was only on the success path. See BLOCKER-1 below. |
| C4 | `dropped` snapshotted in the same critical section | PASS | `main.c:226-229` (`m`,`d` in one `__disable_irq/__enable_irq`); stop path `main.c:192-196` |
| C5 | `vibl_mark_gap()` from main loop | PASS | `main.c:238` (`if (d) vibl_mark_gap(d)`), stop path `main.c:198` |
| D1 | Handler = `EXTI15_10_IRQHandler` | PASS | `stm32f4xx_it.c:206-210`; `include/stm32f4xx_it.h:61` prototype; `stm32f405xx.h` `EXTI15_10_IRQn = 40`; `nm startup_stm32f405xx.o` → `W EXTI15_10_IRQHandler`; `nm firmware.elf` → `08004e84 T EXTI15_10_IRQHandler` (strong override linked) |
| D2 | NVIC EXTI15_10 prio 3 (TIM2=1, SDIO=2) | PASS | `main.c:139-140`; `tim.c:82` prio 1; `sdio.c:106` prio 2; `.ioc` `NVIC_PRIORITYGROUP_4` |
| D3 | `session_stop` set in strong callback | PASS | `main.c:298-301` (`USER CODE BEGIN 4`); `nm` → `08004580 T HAL_GPIO_EXTI_Callback` |
| D4 | Stop order | PASS | `main.c:188` stop TIM2 → `191-196` snapshot mask+dropped → `197-198` drain → `198` mark gap → `201` `vibl_close()` → `207` unmount → `208` LED → `210-211` `HAL_IWDG_Refresh`+`__WFI` |
| D5 | `f_mount(NULL,"0:",0)` not `f_close(&fs)` | PASS | `main.c:207`; `ff.h:243` `f_close(FIL*)`; `ff.h` `f_mount(FATFS* … NULL:unmount)` |
| E1 | Header 256 B, payload at 256 | PASS | `sd_writer.h:29` `VIBL_HEADER_SIZE 256`, `_Static_assert` (`sd_writer.h:78-79`); `parser_vibl.py:39` `pos = 256` |
| E2 | Header CRC over [0..251], `header_crc` zeroed at compute | PASS | `sd_writer.c:154` memset zeros all, `sd_writer.c:172` CRC over `VIBL_HEADER_CRC_LEN` (252) **before** assigning `header_crc` (`:173`); close path zeros it explicitly (`sd_writer.c:366-368`) |
| E3 | Python zeroes `[51:55]` | PASS | `parser_vibl.py:20-28`; offsets: magic 0-3, version 4-5, sample_rate 6-9, axes 10, bps 11, sensor 12-27, range_g 28, gain 29-32, offsets 33-38, start_ts 39-46, record_count 47-50, **header_crc 51-54** |
| E4 | Payload CRC semantics match C | PASS | C: `sd_writer.c:182` seed 0xFFFFFFFF running, `sd_writer.c:413` `^ 0xFFFFFFFF`; Python: `parser_vibl.py:41` `payload_crc = 0`, `:92` `calc_crc = payload_crc & 0xFFFFFFFF` (no XOR). **Empirically confirmed** on `test/SYNTH01.VIB` → `File CRC32: 64B68650 → ✅ MATCH` |
| E5 | Chunk = `seq u32 | count u16 | 2048×3×int16` | PASS | `sd_writer.h:82-90` (`_Static_assert` = 6 B), `sd_writer.c:289-291` |
| E6 | Trailer at `vibl_data_end`, not `f_size()` | PASS | `sd_writer.c:318` `vibl_data_end = (uint32_t)f_tell(&vibl_fil)`, `sd_writer.c:398-401` seek to data end |
| E7 | `f_truncate()` **after** trailer write | WARN | Actual order is seek(`data_end`) → `f_truncate` (`sd_writer.c:405`) → **then** trailer write (`sd_writer.c:415`), i.e. literal E7 order is inverted. The implementation order is **safer** (truncate sets `objsize = data_end`, trailer lands exactly at EOF; power loss between the two leaves size = data end and no trailer — the parser's existing power-loss path). Behaviour required by E7 (tail released, trailer at data end) is achieved. See W-3. |
| F1 | `expected = 256 + 3600·12294 = 44 258 656 B` (42.2 MiB), cap 64 MiB | PASS | `sd_writer.c:242-248`; cap branch (`expected > cap`) never taken at 3600 chunks |
| F2 | Seek to `expected`, seek back to 256, verify `f_tell` | PASS | `sd_writer.c:250,252,254`; mechanism confirmed in `ff.c:4113` (`create_chain` forced stretch in FA_WRITE), `ff.c:4114-4115` (clip on disk full) |
| F3 | `vibl_data_end` tracked in `vibl_write_chunk` | PASS | `sd_writer.c:318` |
| F4 | Disk-full during prealloc → close + `f_unlink` + `VIBL_ERR_OPEN` | PASS | `sd_writer.c:259-265`; `f_unlink` declared `ff.h:255`, releases chain via `remove_chain` (`ff.c`) |
| F5 | Comment arithmetic fixed to 76.8 min | PASS | `sd_writer.c:237-240` ("3600 chunks × 1.28 s = 76.8 min"; 1 h = 2813 chunks) |
| G1 | `IWDG_PRESCALER_32` → T_IWDG = 4.095 s | PASS | `iwdg.c:45` PRESCALER_32, `iwdg.c:46` Reload=4095; `.ioc` `Lsi_value=32000` → 4095·32/32000 = 4.095 s |
| G2 | SD timeouts 200 ms | PASS | `diskio.c:56` `HAL_SD_ReadBlocks(…, 200U)`, `diskio.c:71` `HAL_SD_WriteBlocks(…, 200U)`; hierarchy 200 ms < 4.095 s |
| G3 | CD-gate before `MX_SDIO_SD_Init()` | PASS | `main.c:120-130` inside `USER CODE BEGIN PreSDIO`; pinout §1 note 2 (PA8 Active LOW → `GPIO_PIN_SET` = card absent) |
| G4 | `HAL_IWDG_Refresh` in `disk_read`/`disk_write`/`hil3_error_loop` | PASS | `diskio.c:60`, `diskio.c:80`, `main.c:348,351,355`, `main.c:210`, `sd_writer.c:333` (chunk path) |
| H1 | `_FS_NORTC = 0` | PASS | `ffconf.h:51`; `ff.c` `GET_FATTIME() get_fattime()` branch; declared `ff.h` under `#if !_FS_READONLY && !_FS_NORTC` |
| H2 | `get_fattime()` in `diskio.c`, Time **before** Date | PASS | `diskio.c:132-166` (`HAL_RTC_GetTime` at `:139`, `HAL_RTC_GetDate` at `:142`); separate `RTC_TimeTypeDef`/`RTC_DateTypeDef` — **no** `RTC_DateTimeTypeDef` (historical hallucination avoided) |
| H3 | Year packed `(d.Year + 20) << 25` | PASS | `diskio.c:160` (RTC Year = offset from 2000 → year−1980) |
| H4 | `vibl_get_start_ts()` declared | PASS | `sd_writer.h:143`, defined `sd_writer.c:440`, returns `vibl_hdr.start_ts` |
| H5 | `start_ts == 0` → `hil3_error_loop(6)` | PASS | `main.c:162-164`, after `vibl_open` |
| I1 | `-Ofast -ffast-math` removed | PASS | `platformio.ini:35-45`; `pio run -v` compile lines contain no `-Ofast`/`-ffast-math` |
| I2 | `-O2 -fno-fast-math` present | PASS | `platformio.ini:44-45`; verified in the `src/main.c` compile line |
| I3 | `-DCMSIS_DSP` removed | PASS | `platformio.ini:45-46` comment only; no `-DCMSIS_DSP` in compile lines |
| I4 | `-DHSE_VALUE=8000000` kept | PASS | `platformio.ini:26`, present in every compile line; pinout §3 (HSE = 8 MHz on PH0/PH1) |
| I5 | Framework appends `-Os` after our flags | WARN | `pio run -v`: `… -O2 -fno-fast-math -funroll-loops -mthumb -mcpu=cortex-m4 -Os …` → effective optimization = **`-Os`**; `-fno-fast-math` stays effective (last-wins applies to the -O level only) |
| J1 | `count == 0` → `power_loss_tail = True; break` | PASS | `parser_vibl.py:47-49` |
| J2 | Monotonicity on `seq_number` | PASS | `parser_vibl.py:50-51` — key is `seq_number` (chunks store that key at `:66`), deviation from the spec snippet (`seq`) is correct, not a bug |
| J3 | Trailer branch gated by `power_loss_tail` | PASS | `parser_vibl.py:74-78` → prints "⚠️ No trailer found (power loss)"; **empirically confirmed**: `SYNTH_POWERLOSS.VIB` prints exactly that, no bogus `00000000` CRC line |
| K1 | `.ioc` line 8 = `IWDG.Prescaler=IWDG_PRESCALER_32` | PASS | `SDlogger405RG_Clean.ioc:8`, matches `iwdg.c:45` — **no contradiction** (rule 15) |
| K2 | Stale `IWDG_PRESCALER_8` | PASS (absent) | `Select-String SDlogger405RG_Clean.ioc 'IWDG'` → only `_PRESCALER_32`; `iwdg.c` has no `USER CODE BEGIN IWDG_Init 1` override, so `.ioc` is the single source on regeneration |
| L1 | `USER CODE BEGIN PreSDIO` / `END PreSDIO` | WARN | `main.c:120,130` — non-standard marker name; `.ioc` has `ProjectManager.KeepUserCode=true`, CubeMX preserves **any** named USER CODE block, so regeneration risk is low; flag in the CubeMX workflow note |
| L2 | `USER CODE BEGIN 1` contains `EXTI15_10_IRQHandler` | PASS | `stm32f4xx_it.c:198-211` |
| L3 | `USER CODE BEGIN 4` contains strong `HAL_GPIO_EXTI_Callback` | PASS | `main.c:294-301` |
| M1 | `Header CRC: OK` + `File CRC32: ✅ MATCH` | PASS (static), hardware TBD | `test/SYNTH01.VIB` → `✅ Header CRC: OK` **and** `File CRC32: 64B68650 → ✅ MATCH` (parser + independent C-algorithm port) |
| M2 | `record_count == total_records == Σ sample_count` | PASS (static) | On `SYNTH01.VIB`: 10240 == 10240 == 10240; trailer sits at `pos = 61726`, file size 61742 = 256 + 5·12294 + 16 |
| M3 | `Gaps Found: No` on 60-min clean run | hardware | gap-detection logic verified (`SYNTH_STALE.VIB` reports `Gap … 9 → 1326`) |
| M4 | Card removal ⇒ `Gaps Found: Yes` + `dropped > 0` | **BLOCKED by BLOCKER-1** | mechanism present (`main.c:331-332`, `:238`), but see BLOCKER-1 for the burned-slot/never-marked-full path |
| M5 | Flat ⇒ Z ≈ +16384 LSB, flip ⇒ sign invert | hardware | `CTRL_REG5=0x20` → FSCALE=100 = ±16 g, sensitivity 0.73 mg/LSB (DS §7.24 table); read-back B4 is the static proxy |
| M6 | Read-back `CTRL_REG4 ∈ 0x98..0x9F` | PASS (static) | `lis3dsh.c:119-121` |
| M7 | PA8 HIGH at boot ⇒ `err=2` ≥ 5 s, no reset | PASS (static) | gate `main.c:128-130` runs **before** `MX_SDIO_SD_Init()` (`main.c:131`); `hil3_error_loop` refreshes IWDG every 200/100 ms (`main.c:348-355`) vs T_IWDG 4.095 s |
| M8 | PC13 ⇒ file contains trailer | PASS (static) | stop branch reaches `vibl_close()` (`main.c:201`) |
| M9 | 24-h soak, slowed `disk_write`, `dropped == 0` | hardware | `dropped` accounting is static-PASS; P99 flash duration not measured (U-2 open) |

## Blockers (must close before flash)

> **Status 2026-11-15: 0 open blockers.** BLOCKER-1 below is CLOSED (patch applied, gate re-measured). No other blocker in this audit was ever open.

### BLOCKER-1 — OOB write in the ISR error path (check C3: FAIL → **CLOSED / RESOLVED 2026-11-15**)
`src/main.c:320-325`: the SPI-failure branch increments `fill_cnt` and **returns before** the wrap check, which exists only on the success path (`main.c:330-338`). Sequence that breaks: an SPI failure at `fill_cnt = 6141` → `fill_cnt = 6144`, `return`. On the **next** tick the success path executes
`int16_t *b = arena[fill_idx];` and `lis3dsh_read(&b[6144], &b[6145], &b[6146])` (`main.c:310`) — `arena[i]` has indices 0..6143, so this is an **out-of-bounds write into the adjacent `.bss` object** (`.bss` = 27 392 B per `pio run` map). Secondary consequence of the same asymmetry: an arena whose last slots were burned **is never marked full** (`main.c:331-334` skipped), so those samples are silently lost with no `dropped` count, no seq jump, and `fill_idx` stays on the full arena (all subsequent chunks land in the same arena).
Fix (5-line restructure, `fill_cnt` invariant `∈ {0,3,…,6141}` guaranteed at entry; ~3 instructions, zero effect on the 625 µs ISR budget):

```c src/main.c
    int16_t *b = arena[fill_idx];

    int rc = lis3dsh_read(&b[fill_cnt], &b[fill_cnt + 1], &b[fill_cnt + 2]);
    if (rc != 0) {
        /* P2-8: escalate only after 10 consecutive failures (~6.3 ms dead bus) */
        if (++spi_err_streak >= 10u) { lis3dsh_err = 1u; }
    } else {
        spi_err_streak = 0u;
    }
    /* Shared wrap for BOTH paths (fixes C3): the slot is always burned and the
     * arena is always closed at VIBL_CHUNK_WORDS, so fill_cnt < 6144 at entry. */
    fill_cnt += 3u;
    if (fill_cnt >= VIBL_CHUNK_WORDS) {
        if (full_mask & (uint8_t)(1u << fill_idx)) {
            dropped += VIBL_SAMPLES_PER_CHUNK;
        } else {
            full_mask |= (uint8_t)(1u << fill_idx);
        }
        fill_idx ^= 1u;
        fill_cnt = 0u;
    }
```
Cost/behaviour note: burned slots are written as stale/zero samples inside the chunk that closes. This is the deliberate documented tradeoff (P2-8, `main.c:315-319`) and is **better** than the current state (never-marked-full arena = invisible loss + arena misalignment). Per-SPI-failure gap reporting needs a record-level invalid flag — open item, not a blocker.
Acceptance re-check after the patch: `grep -n "fill_cnt += 3u" src/main.c` must return **exactly one** line, located before the `if (fill_cnt >= VIBL_CHUNK_WORDS)` block.

**CLOSED 2026-11-15 — patch applied exactly as specified above.** Current state: `src/main.c:419` `int rc = lis3dsh_read(&b[fill_cnt], &b[fill_cnt + 1], &b[fill_cnt + 2]);`, `:420-433` streak accounting (`if (++spi_err_streak >= 10u) lis3dsh_err = 1u;` / `else spi_err_streak = 0u;`), then the single shared block `:438 fill_cnt += 3u;` → `:439 if (fill_cnt >= VIBL_CHUNK_WORDS) { … full_mask |= … / dropped += … ; fill_idx ^= 1u; fill_cnt = 0u; }` (`:440-447`). Gate executed on 2026-11-15: `Select-String src\main.c -Pattern 'fill_cnt \+= 3u'` → **1 match** (`main.c:438`), before the wrap block. Build after this patch alone: `pio run -e release` SUCCESS, RAM 27 428 B / Flash 25 840 B (the current tree, after all Milestone-2 patches, reports RAM 27 436 B / Flash 25 976 B). Invariant now enforced by construction: `fill_cnt ∈ {0,3,…,6141}` at every ISR entry, so `&b[6144..6146]` is unreachable and a saturated arena is always either published (`full_mask`) or counted (`dropped`). Details: `doc/MILESTONE_2_ROBUSTNESS.md` §2.

## Warnings (can flash, note them)

- **W-1 (I5, build level).** Effective optimization is `-Os`, not `-O2` (framework appends `-Os` after the project flags). `-fno-fast-math` remains effective. Not a blocker. If the exact `-O2` level is required, add `build_unflags = -Os` to `platformio.ini` — untested on ststm32 20.0.0, so it is not recommended in this gate.
- **W-2 (E7, order).** `f_truncate()` runs **before** the trailer write (`sd_writer.c:401-408` then `:415`), not after. The chosen order is the safe one: truncate at `data_end` sets `objsize` = real data end, trailer lands at EOF; power loss between the two leaves `size == data_end` and no trailer = the parser's power-loss path. The literal E7 wording in the spec is inverted; do **not** change the code.
- **W-3 (L1, regeneration).** `USER CODE BEGIN PreSDIO` is a non-standard marker name. `.ioc` has `KeepUserCode=true` and CubeMX preserves named USER CODE blocks, so the CD-gate survives regeneration — but the workflow (open `.ioc` → save → rebuild) has not been exercised on the user's CubeMX 6.18.1. Verify after any regeneration that `main.c:120-130` is present.
- **W-4 (`.ioc` SD mode vs hardware).** `.ioc` pins PC8..PC11 as `SD_4_bits_Wide_bus` (pinout §1 wires all four lines), while `sdio.c:45` runs `SDIO_BUS_WIDE_1B` with `ClockDiv = 16` → SDMMC_CK = 48/(16+2) = 2.667 MHz, ~130-200 KB/s vs 9 600 B/s needed (≥13× margin). Intentional (WARN-3 of audit #2 deferred). Note the coupling: `diskio.c:29-31` explicitly keeps the 4-bit call removed, and `sdio.c:52-54` keeps it commented. Any future 4-bit experiment must lower `ClockDiv` simultaneously.
- **W-5 (no IWDG feed inside `HAL_SD_Init`).** `disk_initialize()` (`diskio.c:38`) calls `HAL_SD_Init` with no refresh, and the HAL has no refresh in its init path; `SDMMC_DATATIMEOUT = 0xFFFFFFFF` (`stm32f4xx_ll_sdmmc.h:303-304`) is the long-blocking candidate (U-3 unchanged). Mitigated at boot by the CD gate (G3) — during recording, hot removal is handled by `disk_status()` via `validate()` (`ff.c:3236`), which does **not** re-run `HAL_SD_Init`. Residual exposure: a card present at boot that dies mid-session and forces a `disk_initialize()` re-entry path.
- **W-6 (RTC never set).** `MX_RTC_Init()` (`rtc.c`) never calls `HAL_RTC_SetTime/SetDate` (not in the tree). If the RTC is never written, `vibl_rtc_ms() == 0` ⇒ `vibl_get_start_ts() == 0` ⇒ `hil3_error_loop(6)` at boot, i.e. the logger **refuses to run** until the RTC is set. This is the intended loud failure (P2-9), and must be part of the hardware procedure below, not treated as a bug.
- **W-7 (fixed filename).** `vibl_open("LOG0003.VIB", …)` (`main.c:154`) with `FA_CREATE_ALWAYS` (`sd_writer.c:184`) — every reboot **overwrites** the previous session. Rollover naming (LOG0001…LOGNNNN) is a product decision.
- **W-8 (single-volume/8.3 names).** `_USE_LFN = 0` (`ffconf.h:24`) — 8.3 names only; `LOG0003.VIB` is legal. `_USE_MKFS = 0` (`ffconf.h:17`) ⇒ the card must be FAT32-formatted on a PC; `GET_BLOCK_SIZE` (`diskio.c:88`) is inert (read only in `f_mkfs`).
- **W-9 (RAM map vs board def).** `pio run` reports RAM 20.9 % (27 408 B / 131 072 B) and `.ccmram` = 0. F405RG has 128 KB main SRAM + 64 KB CCM; CCM stays unused, which is correct for SDIO (separate AHB master). Headroom for a third arena (+12 KB) exists.
- **W-10 (build warning).** Clean rebuild: only `src/ff.c:5032:2: warning: this 'for' clause does not guard... [-Wmisleading-indentation]` (pre-existing FatFs cosmetic issue, not in project files). Re-verified 2026-11-15 after the Milestone-2 patches (`pio run -e release -t clean` + `pio run -e release -v`): 109 `arm-none-eabi-gcc` invocations, **107 with `-O2`, 0 with `-Os`**, 0 `-ffast-math` / `-Ofast`, and the single `ff.c:5032` warning is still the only one — i.e. **W-1 is closed as well** (`build_unflags = -Os` in `platformio.ini` works on ststm32 20.0.0).
- **W-11 (added 2026-11-15, SD-error shutdown).** The new `sd_error_shutdown()` (`main.c:129-168`) ends the session on a chunk-write error instead of continuing; that is a deliberate policy, not a bug — see `doc/MILESTONE_2_ROBUSTNESS.md` §4. Two exposures it does NOT remove: (a) `disk_read`/`disk_write` refresh the IWDG only on the **success** path (`diskio.c:71`, `:124`) and return `RES_ERROR` before any refresh on failure, so a long chain of failing FatFs ops inside `vibl_close()` is bounded only by the 1000 ms per-op timeout, not by an explicit feed; (b) `HAL_SD_Init()` has no internal feed (`W-5`), and with a card that is physically present but electrically dead it can outlive `T_IWDG = 4.095 s` (U-3 unchanged).

## Acceptance readiness

Statically verified now (no hardware):
- **M1, M2** — `python parser_vibl.py test/SYNTH01.VIB` → `Header CRC: OK`, `File CRC32: 64B68650 → ✅ MATCH`, and `record_count == trailer.total_records == Σ sample_count` = 10240. The C CRC path (seed 0xFFFFFFFF, poly 0xEDB88320, final XOR) and the Python path (seed 0, mask-only) agree, verified by an independent port of `crc32_update()` (`test/mk_synth_vibl.py`).
- **M3 (mechanism)** — gap detection proven: `SYNTH01.VIB` reports `Gap … 2 → 10` (the `vibl_mark_gap(7)` seq jump), `SYNTH_STALE.VIB` reports `Gaps Found: Yes`; power-loss file reports `Gaps Found: No` + "No trailer found (power loss)". **Updated 2026-11-15:** the parser message is now `⚠️ No trailer found (power loss/error)` and it also covers the **all-0xFF** tail shape (`parser_vibl.py:115-117`, `:85-86`, `:89-97`); verified on `test/SYNTH_FFTAIL.VIB` (44 258 656 B, 10 chunks, 0.49 s, no bogus `File CRC32` line) and on `test/SYNTH_TRUNC_PARTIAL.VIB` (partial chunk at EOF).
- **M6, M7, M8** — read-back predicate (`lis3dsh.c:120`), pre-SDIO CD-gate + IWDG hierarchy (`main.c:128-130`, `iwdg.c:45`, `diskio.c:56,71`), stop branch reaching `vibl_close()` (`main.c:201`) — all present and provable from source.
- **M4, M9** — accounting logic present (`main.c:331-332`, `:238`), **but M4 is gated by BLOCKER-1**. **Updated 2026-11-15:** BLOCKER-1 is CLOSED, so M4 is hardware-pending only. Call-site line numbers moved with the patch: gap accounting is now `main.c:311-315` (snapshot) / `main.c:331` (`vibl_mark_gap`), arena accounting `main.c:438-447` (shared wrap).
- **I1..I4** — verified by `pio run -v` compile lines; **I5** = WARN (`-Os` wins).
- **K1/K2** — `.ioc:8` and `iwdg.c:45` agree (no rule-15 contradiction).
- Build integrity: `pio run` succeeds; `nm firmware.elf` shows `EXTI15_10_IRQHandler` (T, 08004e84), `HAL_GPIO_EXTI_Callback` (T, 08004580), `get_fattime` (T, 08002d64), `vibl_get_start_ts` (T, 08004c58) — all linked, not stripped; startup `.o` has the weak `EXTI15_10_IRQHandler` slot (vector slot 56 populated), confirming the D1 deviation is real and the spec's `EXTI15_13_IRQHandler` would have been a silent no-op.

Requires hardware (cannot be closed statically): **M3 (60-min clean run), M4, M5, M7 (timed blink), M8 (real trailer), M9 (24-h soak)**. M5/M6 close U-1 (the 0x40-bit question is now answered in code by DS §5.2 + the `_Static_assert`, and the read-back is the on-board proof).

## Hardware verification steps (for the user)

Order matters: 1-3 close U-1 and prove the sensor path **before** any long recording.
1. **Read-back proof (M6).** Flash, set the RTC (see step 2), observe that boot does **not** enter `err=6`. Add a temporary one-shot print/log of `ctrl4_rb` (`lis3dsh.c:119`); accept `0x98..0x9F`. If −2 → check CS (PA4), SPI mode 3, wiring (pinout §6).
2. **RTC set.** `MX_RTC_Init()` never sets date/time; set the RTC once (SWD/`HAL_RTC_SetDate`+`SetTime`, Time before Date). Without this, `vibl_get_start_ts() == 0` ⇒ `hil3_error_loop(6)` at boot (W-6).
3. **Orientation check (M5, closes U-1).** Sensor flat ⇒ Z ≈ +16384 LSB at ±16 g (0.73 mg/LSB), X,Y ≈ 0; rotate 180° around X ⇒ Z sign inverts. Run `parser_vibl.py --plot` on the captured file. If Z stays ~0 / axes are garbage ⇒ burst address decoding assumption is wrong → re-open with LA on PA5(SCK)/PA7(MOSI)/PA6(MISO) (pinout §6).
4. **CD-gate (M7).** Empty slot, PA8 HIGH ⇒ LED blinks code 2 for ≥ 5 s **without** reset (`.ioc` IWDG=4.095 s; `hil3_error_loop` refreshes every 200/100 ms).
5. **Clean session + graceful stop (M1, M2, M8).** Record ≥ 5 min, press PC13, dump the card, `python parser_vibl.py LOG0003.VIB` ⇒ `Header CRC: OK`, `File CRC32: ✅ MATCH`, `record_count == total_records == Σ sample_count`, `Gaps Found: No`, trailer present. Note `vibl_close()` runs `remove_chain()` over the preallocated tail (~10 300 clusters at 4 KiB, ~0.3-0.5 s) — TIM2 is stopped first (`main.c:188`), IWDG fed inside `disk_write` (`diskio.c:80`), so no reset is expected.
6. **Hot removal (M4, requires BLOCKER-1 patch).** Record, pull the card for ~3 s, re-insert, stop with PC13, parse ⇒ `Gaps Found: Yes` **and** `dropped > 0`. **Updated 2026-11-15:** the BLOCKER-1 prerequisite is CLOSED, so this step can be executed as written; note that with the new shutdown path a card pull that fails a chunk write now ends the session (`sd_error_shutdown()` → trailer + blink code 5) rather than continuing, so expect a *valid, truncated* file, not a long one.
7. **Soak (M9, U-2, U-5).** 24 h with `disk_write` artificially slowed (e.g. `HAL_Delay(1500)` in `diskio.c:71` for the test build only) ⇒ `dropped == 0`, no watchdog reset, P99 `vibl_write_chunk` < 200 ms (instrument with DWT_CYCCNT around `vibl_write_chunk`). Also check `dir` on PC shows real mtimes (P2-9).
8. **Power loss mid-session (U-4).** Cut power mid-run, parse ⇒ chunk count = last `f_sync`'d chunk, "No trailer found (power loss)", parser finishes < 1 s. Do **not** claim "≤ 1 chunk lost" — the card cache depth is undefined (U-4). **Updated 2026-11-15:** the parser now prints `⚠️ No trailer found (power loss/error)` and finishes in **0.49 s** on the 44 MiB fixture (`test/SYNTH_FFTAIL.VIB`), for both blank-tail shapes (all-0x00 and all-0xFF).
9. Flashing is a manual step: `pio run -t upload` requires explicit user instruction (rule 18). This audit did not flash.

## Open items (deferred to next iteration)

- **O-1 (U-1, sensor burst).** Code is now consistent with DS §5.2 + `_Static_assert` (`lis3dsh.c:42-44`), and DS §10 gives "SPI read: CS low, send (0x80 | addr)". The DS does **not** provide an explicit bit map of the 7-bit sub-address, so the final proof of `0xA8` is step 3 + step 1 on hardware. **UNCERTAIN: DS gives no explicit sub-address bit map.**
- **O-2 (per-SPI-failure gap accounting).** Deliberately not implemented (deviation D4 in `doc/report.md`): a partially invalid arena cannot be expressed as a gap without a new VIBL record flag. Needs a format decision from the owner.
- **O-3 (U-2 / U-5).** Realistic 1-bit SDIO throughput and P99 flash duration unmeasured; FTL behaviour on mid-sector writeback (`FIL` 512 B window, `ff.c:3664-3667`, `3749-3751`) is empirical only. Needs DWT + LA + 24-h soak.
- **O-4 (U-4).** Card write-cache energy-loss depth: `disk_ioctl(CTRL_SYNC)` is a no-op (`diskio.c:90-92`) and `HAL_SD_ConfigCacheSize`/CMD6 is not implemented in HAL F4. The comment "power loss corrupts at most this in-flight chunk" (`sd_writer.c:233-234`) overstates the guarantee; reword to "≤ N chunks, N = card cache depth per datasheet".
- **O-5 (real power-loss tail).** Synthetic power-loss tails are all zeros. A real post-power-loss tail is stale FAT/card data that can contain plausible `count = 2048` chunk headers at positions with no real data; the monotonicity guard (`parser_vibl.py:50-51`) bounds the damage (no infinite loop, no OOM) but cannot prove those bytes are not data. Needs a real captured file. **Updated 2026-11-15:** one more shape is now covered — an **all-0xFF** tail (erase-state-1 medium / sectors left unwritten by a failed write) is detected both per-header (`parser_vibl.py:115-117`) and as a whole-region scan (`tail_is_blank()`, `parser_vibl.py:89-97`), and can never be reported as a trailer. The "stale plausible chunk headers" case remains open, exactly as written above.
- **O-6 (WARN-3, 4-bit SDIO).** Deferred by design. `HAL_SD_ConfigWideBusOperation(&hsd, SDIO_BUS_WIDE_4B)` (`stm32f4xx_hal_sd.h`/`stm32f4xx_hal_sd.c:2240`) after successful `HAL_SD_Init`, plus lowering `ClockDiv` to 2 (12 MHz) or 4 (8 MHz), only with a logic analyser; acceptance = 24-h soak with `Gaps Found: No` on both configs.
- **O-7 (CubeMX regeneration).** Untested on CubeMX 6.18.1 with `PreSDIO` (W-3) and with the `USER CODE BEGIN 1` handler in `stm32f4xx_it.c`. After any regeneration, re-verify: `main.c:120-130`, `stm32f4xx_it.c:206`, `main.c:298-301`, `iwdg.c:45` vs `.ioc:8`.
- **O-8 (`.pio` build artefacts).** `startup_stm32f405xx.s` is not in the project tree (framework-provided `.o` only). D1 was confirmed via `nm` on the framework object + `stm32f405xx.h:117` (`EXTI15_10_IRQn = 40`) + `report.md` line 188 citation. **UNCERTAIN: the `.s` source line number cannot be cited from the project tree.**
- **O-9 (RTC set policy).** No mechanism in the tree to set the RTC; the product needs a policy (host-set over SWD, or a "RTC not set" user prompt). Tied to W-6.
- **O-10 (cleanup).** Delete `build_verbose.txt`, `build_verbose2.txt`, `clean.log` created during this audit's verification.
- **O-11 (added 2026-11-15, recovery policy).** `sd_error_shutdown()` terminates the session after 3 failed `HAL_SD_Init()` attempts and does not attempt to resume logging after a *successful* recovery. Resuming would require a "discard the abandoned chunk and stay aligned" primitive (`f_lseek(vibl_data_end)` + `f_truncate()` + `fp->err` unlatch, i.e. a `vibl_recover_chunk()` in `sd_writer.c`) plus a decision about the samples of the abandoned arena (they are consumed and gone, and `vibl_mark_gap()` produces a seq jump that has no following chunk to be seen by the parser). Owner decision required; currently documented in `doc/MILESTONE_2_ROBUSTNESS.md` §4 and §7.
- **O-12 (added 2026-11-15, watchdog feed on failing disk ops).** See W-11(a). Candidate fix (NOT applied — documentation-only round): `HAL_IWDG_Refresh(&hiwdg);` before `return RES_ERROR;` in `disk_read` (`diskio.c:68`) and `disk_write`. Arithmetic: `VIBL_WRITE_RETRY_MAX = 3` (`sd_writer.c:88`) × 1000 ms per op = 3.000 s already fits `T_IWDG = 4.095 s`, but `vibl_close()` in the shutdown path runs many FatFs ops (header update, `f_sync`, `f_lseek`, `f_truncate` over ~10 300 clusters) and each *failing* op costs up to 1000 ms with no feed ⇒ on a dying card a reset can beat `hil3_error_loop(5)`. Needs hardware measurement before the patch is justified.

*No source file was modified by the 2026-11-14 audit; `doc/final_pre_flash_audit.md` was its single deliverable. The 2026-11-15 addendum above is an annotation only — the source patches it refers to are described in `doc/MILESTONE_2_ROBUSTNESS.md`.*
