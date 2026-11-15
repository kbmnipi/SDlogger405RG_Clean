# IMPLEMENT report — P1-5 → P1-6 → P1-7 → P2-8 → P2-9 → P2-10

**Build (clean rebuild):** `RAM 27 408 B / 131 072 B (20.9 %)`, `Flash 20 340 B`, **SUCCESS**
Only warning in the whole build: `src/ff.c:5032:2: [-Wmisleading-indentation]` — pre-existing, explicitly allowed.
**Board was NOT flashed. No hardware tests run.**

---

## Per-patch results

### P1-5 — graceful stop via PC13 ✅
| File:line | Change |
| :-- | :-- |
| `src/stm32f4xx_it.c:230-247` | `EXTI15_10_IRQHandler()` → `HAL_GPIO_EXTI_IRQHandler(BUTTON_Pin)` inside `USER CODE BEGIN/END 1` |
| `include/stm32f4xx_it.h:61` | handler prototype in `USER CODE BEGIN EFP` |
| `src/main.c:70-73` | `static volatile uint8_t session_stop` in `USER CODE BEGIN PV` |
| `src/main.c:135-141` | `HAL_NVIC_SetPriority(EXTI15_10_IRQn,3,0)` + `HAL_NVIC_EnableIRQ` + `__HAL_GPIO_EXTI_CLEAR_IT` |
| `src/main.c:178-213` | stop branch at the top of `while(1)` (drain → gap → `vibl_close()` → unmount → TIM2 stop → LED steady → `IWDG_Refresh`+`WFI`) |
| `src/main.c:294-301` | strong `HAL_GPIO_EXTI_Callback()` in `USER CODE BEGIN 4` |

**Disassembly evidence**
`08004e84 <EXTI15_10_IRQHandler>: mov.w r0,#8192 (0x2000 = BUTTON_Pin); b.w 8000aac <HAL_GPIO_EXTI_IRQHandler>` ✔
`08004580 <HAL_GPIO_EXTI_Callback>: cmp.w r0,#8192; strbeq r2,[r3]` — strong override beat the HAL `__weak` version ✔
**Vector table (`.isr_vector`):** slot 56 = **0x08004e84** (our handler), vs. neighbours still `Default_Handler` → the handler is really wired, not dead code. TIM2 (slot 44) = 0x08004e6d, SDIO (slot 64) = 0x08004e79 ✔

### P1-6 — `full_mask` + `dropped` + `vibl_mark_gap()` ✅
`src/main.c:55-73` (PV), `src/main.c:303-338` (ISR), `src/main.c:217-240` (main drain).
Acceptance greps: `grep -n "buf_ready" src/main.c` → **0 matches**; `pingpong_buf|sample_idx|active_buf` → **0 matches**.
`dropped` is snapshotted **inside the same** critical section as `full_mask` (see Deviation D3 — closes the audit's own read-then-clear window). Critical section = 2 loads + 2 stores + CPSR ≈ 12 instr < 100 ns @168 MHz vs. 625 µs ISR period.

### P1-7 — preallocation via `f_lseek` ✅ (with 2 mandatory fixes)
`src/sd_writer.c:202-262` (open), `src/sd_writer.c:34-37, 244, 310-312` (`vibl_data_end`), `src/sd_writer.c:379-402` (close: seek→truncate→trailer).
* target `256 + 3600·12294 = 44 258 656 B` (42.2 MiB), 64 MiB clamp never triggers.
* Mechanism confirmed in this exact `ff.c`: `f_lseek` in `FA_WRITE` forces `create_chain` per cluster (`ff.c:4113`); `f_write` then only follows a linked chain (`ff.c:3652` → `create_chain` returns the existing next cluster at `ff.c:1369`).
* **Bug in the patch text, corrected:** offsets were **256, not 512** (this tree's header is `VIBL_HEADER_SIZE 256`, `parser_vibl.py` parses from 256; there is no `f_lseek(512u)` and no `vibl_samples_written` line in this `sd_writer.c`).
* **Latent bug found and closed:** `vibl_close()` appended the trailer at `f_size()`. With preallocation `f_size()` = 42 MB ⇒ the trailer would land 42 MB deep. Verified empirically: synthesising "preallocated but not truncated" yields `File CRC32: ❌ MISMATCH` + 7 366 160 garbage "chunks"; with the truncate path the same data parses as `Header CRC: OK`, `File CRC32: ✅ MATCH`, `Gaps Found: Yes`.

### P2-8 — SPI error hardening ✅
`src/lis3dsh.c:29`→`LIS3DSH_SPI_TIMEOUT_MS 1u` (53 µs legit frame ⇒ 19× margin).
`src/main.c:69` `spi_err_streak`; ISR error branch burns the slot (`fill_cnt += 3u`), `>= 10` consecutive failures latches `lis3dsh_err` → `hil3_error_loop(4)`; one good frame clears the streak.

### P2-9 — real FAT timestamps ✅
`include/ffconf.h:51-59` `_FS_NORTC 0`; `src/diskio.c:11` `#include "rtc.h"`, `src/diskio.c:132-166` `get_fattime()` (Time **before** Date, separate F4 structs — no `RTC_DateTimeTypeDef`); `include/sd_writer.h:143` + `src/sd_writer.c:440` `vibl_get_start_ts()`; `src/main.c:157-164` → `hil3_error_loop(6)`.
ELM: `08002d64 <get_fattime>` (0x5C) and `08004c58 <vibl_get_start_ts>` (0x10) present in the ELF ⇒ both linked, not stripped ✔
The `_FS_NORTC` flip forced `ff.c` to recompile, re-surfacing the known `ff.c:5032` warning (was cached and therefore hidden earlier) — matches the acceptance criterion.

### P2-10 — register order + build flags ✅
`src/lis3dsh.c:74-79` → REG6(0x25) → REG5(0x24) → REG4(0x20), CTRL_REG4 read-back stays after all three.
`platformio.ini:35-45` → `-O2 -fno-fast-math -funroll-loops`, `-DCMSIS_DSP` removed, `-DHSE_VALUE=8000000` kept; `include/sd_writer.h:25-27` ARR comment corrected.
**Disassembly (register order proven):**
```
800421c: movs r0, #37 @0x25 → bl lis3dsh_write_reg   ; CTRL_REG6 first
8004228: movs r0, #36 @0x24 → bl lis3dsh_write_reg   ; CTRL_REG5
8004234: movs r0, #32       ; + movs r1,#159 @0x9f
8004236:          → bl lis3dsh_write_reg             ; CTRL_REG4 (ODR) last
8004240: movs r1, #160 @0xa0 ... and.w r3, lr, #248 @0xf8 ; cmp r3, #152 @0x98  ; read-back
```
**Flags:** `pio run -v | grep -E "ffast-math|Ofast"` → **0 hits**; actual `main.c` command line: `-O2 -fno-fast-math -funroll-loops`.
⚠ **New finding:** PlatformIO appends its own `-Os` **after** `build_flags` in every compile line, so `-Os` actually wins. `-ffast-math` is gone (the real goal), but `-Ofast` was never effective here either. Left as spec'd.

---

## Deviations (all flagged in-code with `DEVIATION:`)

| # | Spec said | Applied | Why |
| :-- | :-- | :-- | :-- |
| **D1** | `EXTI15_13_IRQHandler` | **`EXTI15_10_IRQHandler`** | STM32F405 has no EXTI15_13 vector — `stm32f405xx.h:117 EXTI15_10_IRQn = 40`, `startup_stm32f405xx.s:188 .word EXTI15_10_IRQHandler`. The spec'd name would compile, be exported, and never fire (silent no-op). |
| **D2** | `f_close(&fs)` | `f_mount(NULL, "0:", 0)` | R0.12c `f_close()` takes `FIL*` (`ff.h:243`); passing `FATFS*` makes `f_sync()` walk a wrong struct layout. Unmount in R0.12c = `f_mount(NULL,...)` (`ff.c:3277-3293`). |
| **D3** | clear `dropped` **after** the flush | snapshot `dropped` **inside** the same `__disable_irq()` as `full_mask` | The spec's ordering re-opens the exact race the audit calls out in WARN-1 (`buf_ready = 0u` runs ~640 ms after the event). Cost: 1 extra load/store in the critical section. |
| **D4** | P2-8 `lis3dsh_err = 1u` "for now" **and** the `>= 10` gate | only the `>= 10` gate | The spec applied sequentially: keeping `lis3dsh_err = 1u` makes `if (lis3dsh_err) hil3_error_loop(4)` fire on the **first** glitch, so P2-8's threshold would be dead code and the acceptance criterion unmeetable. |
| **D5** | `512u` / "remove `vibl_samples_written`" | `VIBL_HEADER_SIZE` = **256** | This tree's header is 256 B and the parser reads from 256. Seeking back to 512 would have inserted a 256-byte hole. `vibl_samples_written` does not exist here — nothing to remove. |
| **D6** | (not in spec) | `vibl_data_end` + `f_truncate()` in `vibl_close()` | Without it the trailer is written at `f_size()` = 42 MB ⇒ CRC/gap check fails (reproduced, see above). This is a hard integration requirement of P1-7, not an optional extra. |
| **D7** | `res = VIBL_ERR_WRITE; goto fail;` | close + `f_unlink()` + `VIBL_ERR_OPEN` | No `res`/`fail:` label exists here, and a bare `f_close()` after a **partially** stretched chain strands up to 42 MiB per retry (`ff.c:4115` only clips size on full disk). |
| **D8** | stop TIM2 **after** `vibl_close()` | stop **before** the drain | `vibl_close()` now runs `remove_chain()` over ~10 300 clusters (0.3-0.5 s @ 4 KiB clusters); stopping first removes ISR jitter from that window and prevents new `full_mask`/`dropped` updates after the snapshot. Also refreshes IWDG every 200 ms → safe vs. `T_IWDG = 4.095 s`. |
| **D9** | — | added `__HAL_GPIO_EXTI_CLEAR_IT(BUTTON_Pin)` | CubeMX never enabled EXTI15_10 in the NVIC and never cleared a possibly-pending bit; without the clear the first interrupt could be spurious. |
| **D10** | "3600 chunks ≈ 1 h" | kept the number, fixed the label | 3600 chunks × 1.28 s = **76.8 min** (1 h needs 2813 chunks). Comment corrected. |

---

## Verification checklist (all 6 items)

| # | Check | Result |
| :-- | :-- | :-- |
| 1 | clean rebuild, 0 warnings in project files | **PASS** — only `ff.c:5032` |
| 2 | no `-ffast-math` / `-Ofast` in compile lines | **PASS** — 0 hits (see the `-Os` note above) |
| 3a | `lis3dsh_init` writes REG6 before REG4 | **PASS** — 0x25 → 0x24 → 0x20, read-back last |
| 3b | handler contains `bl/b.w HAL_GPIO_EXTI_IRQHandler` | **PASS** — `b.w 8000aac` + vector slot 56 populated |
| 4 | `grep -n "buf_ready" src/main.c` → 0 | **PASS** — 0 (word also removed from comments so the literal grep is 0) |
| 5 | `vibl_close` + `vibl_mark_gap` both called | **PASS** — `main.c:199, 201, 238` |
| 6 | no flashing, no hardware tests | **RESPECTED** |

Extra (beyond the checklist): synthetic VIBL file generated by a pure-Python port of `crc32_update()` (`test/mk_synth_vibl.py`, kept) and fed to the real `parser_vibl.py` → `Header CRC: OK`, `File CRC32: ✅ MATCH`, `Gaps Found: Yes`, `total_records == trailer.total_records`. This validates the P1-7 geometry and the close() trailer offset without a board. **No firmware-semantics file was modified** (P0-1/BUG-3 territory untouched).

---

## Open items (NOT closed by this patch set)

1. **U-1 (BLOCKER for V-4) — LIS3DSH burst byte `0xA8` vs `0xE8`.** Closed in code by the earlier **P0-2** (`lis3dsh.c` `_Static_assert == 0xA8`), but the audit's own acceptance tests are **hardware-only**: scope on PA5/PA6/PA7 and the "flat ⇒ Z ≈ +16384 LSB" orientation test. Not performed (no flash, no bench).
2. **U-2 — realistic 1-bit SDIO throughput / P99 flash duration.** Needs DWT instrumentation + `vibl_write_chunk` timing over 24 h. Not measured.
3. **P1-6 acceptance ("slow `disk_write` with `HAL_Delay(1500)`, then check `Gaps Found: Yes` + `dropped > 0`")** — requires flashing. Not run; only the synthetic-file equivalent was verified.
4. **P2-9 acceptance (`dir` on PC shows real date/time)** — requires flashing + a set RTC. Static proof only that `get_fattime()` is linked. **Note:** `MX_RTC_Init()` never sets the date/time; if the RTC is never written by the user, `get_fattime()` returns 1980-scaled values and `vibl_get_start_ts() == 0` ⇒ `hil3_error_loop(6)`.
5. **Per-SPI-failure gap reporting.** Deliberately not implemented (D4): a partially invalid arena cannot be expressed as a gap without a new VIBL record type. The file stays self-consistent (seq + CRC) while affected samples are stale/partial. Needs a format decision from the owner.
6. **WARN-3 (4-bit SDIO)** and **WARN-5's separate `-Ofast` env** — explicitly out of scope ("not in this patch").
7. **WARN-4 second half — `f_sync()` on every chunk** (67 500 metadata ops/day). The audit marks this an **owner decision** (moving to every-4th chunk worsens the loss window 1.28 s → 5.12 s). Not changed.
8. **U-4/U-5/U-8** — card-cache power-loss depth, mid-sector writeback FTL behaviour, erase-unit/P-E assumptions: unchanged, still need a real card datasheet + 24 h soak.
9. **U-7** — HIL-vs-production profile conflict (PA5 = `SPI1_SCK` here, no DAC/ADC in the tree) is an owner/TZ decision.