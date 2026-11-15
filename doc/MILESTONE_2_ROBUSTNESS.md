# MILESTONE 2 — Robustness: ISR integrity (BLOCKER-1), graceful SD-error shutdown, parser truncated-file handling

**Status:** CODE-COMPLETE, STATICALLY VERIFIED. Not flashed, not bench-verified — flashing requires explicit user instruction (rule 18), and the hardware acceptance list (§7) is still open.
**Date:** 2026-11-15.
**Supersedes:** `doc/final_pre_flash_audit.md` — check **C3 / BLOCKER-1 (FAIL → CLOSED)** and the parser's zeros-only power-loss path. The audit file is annotated in place, its 2026-11-14 findings are not rewritten (rule 15: newer artefact is authoritative, contradiction is flagged explicitly, never silently merged).
**Build state at this milestone:** `pio run -e release` and `pio run -e debug` both SUCCESS; release RAM 27 436 B (20.9 %) / Flash 25 976 B (2.5 %); full clean rebuild at `-O2` with zero `-Os`.
**Hardware claims used:** `doc/WEACT_F405_PINOUT.md` §1 — PA8 = SD card detect, **inserted = PA8 HIGH** (NC switch, R11 100 k pull-up, R12 10 k series; bench 2.5 V / 0.4 V). No new pins were introduced.

---

## 1. Change set

| ID | Patch | File:lines | Purpose |
|----|-------|-----------|---------|
| **P2-1** | BLOCKER-1 closure (audit check C3) | `src/main.c:398-448` | TIM2 callback: one **shared** advance+wrap block executed by BOTH the SPI-success and the SPI-error path. Out-of-bounds `&b[6144..6146]` becomes structurally impossible; a saturated arena is always published (`full_mask`) or counted (`dropped`). |
| **P2-2a** | Graceful shutdown on SD write error | `src/main.c:41-48` (`SD_RECOVERY_ATTEMPTS`), `:92-168` (`sd_error_shutdown()`), `:317-332` (call site) | Bus recovery attempts, then truncate + trailer, then stop the sampler, then park with blink code 5. |
| **P2-2b** | File must stay open for the shutdown to work | `src/sd_writer.c:94` (`vibl_dbg_sd_error`), `:177-198` (`vibl_stall()`), `:406-413`, `:419-424` | A failed CHUNK no longer closes the FIL; `FIL.err` is unlatched so that `vibl_close()` can actually run. |
| **P2-3** | Parser tail handling | `parser_vibl.py:46-57`, `:83-97`, `:111-131`, `:148-177` | All-0x00 **and** all-0xFF tails, garbage headers and partial chunks report `⚠️ No trailer found (power loss/error)` with **no** `File CRC32` verdict; chunk statistics and `--plot` preserved. |
| **P2-3b** | New fixtures + generator | `test/mk_synth_error_tail.py`, `test/SYNTH_FFTAIL.VIB`, `test/SYNTH_TRUNC_PARTIAL.VIB`, `test/SYNTH_SDERROR_CLOSED.VIB` | Board-free acceptance evidence for the SD-error shapes (the old fixtures only modelled power loss). |

C code was NOT changed in the documentation round that produced this file; §1 describes the already-applied code patches.

---

## 2. P2-1 — BLOCKER-1 CLOSED (ISR arena accounting)

### 2.1 What was wrong (verbatim from the audit, kept for the record)

`src/main.c` (2026-11-14 revision) lines 320-325: the SPI-failure branch executed `fill_cnt += 3u;` and `return;`, while the wrap check lived only on the success path (`main.c:330`). Failure sequence: SPI error at `fill_cnt = 6141` → `fill_cnt = 6144` → return; on the **next** tick the success path ran `lis3dsh_read(&b[6144], &b[6145], &b[6146])` while `arena[i]` is indexed `0..6143` (`VIBL_CHUNK_WORDS = VIBL_SAMPLES_PER_CHUNK * 3u = 2048 * 3 = 6144` int16 = 12 288 B per arena) → a 6-byte write past the arena into the adjacent `.bss` object (`.bss` = 27 416 B in the current map). Secondary defect: the arena whose last slots were burned was **never marked full**, so those samples vanished with no `dropped` count, no seq jump, and `fill_idx` stuck on the saturated arena.

### 2.2 What is there now

```c
    int16_t *b = arena[fill_idx];                                   /* main.c:402 */
    int rc = lis3dsh_read(&b[fill_cnt], &b[fill_cnt + 1], &b[fill_cnt + 2]);  /* :419 */
    if (rc != 0) {                                                  /* :420-430 */
        if (++spi_err_streak >= 10u) { lis3dsh_err = 1u; }         /* :428-430 */
    } else {
        spi_err_streak = 0u;                                        /* :432 */
    }
    fill_cnt += 3u;                                                 /* :438  <- single advance */
    if (fill_cnt >= VIBL_CHUNK_WORDS) {                             /* :439  <- shared wrap     */
        if (full_mask & (uint8_t)(1u << fill_idx)) { dropped += VIBL_SAMPLES_PER_CHUNK; }
        else                                       { full_mask |= (uint8_t)(1u << fill_idx); }
        fill_idx ^= 1u;                                             /* :445 */
        fill_cnt = 0u;                                              /* :446 */
    }
```

**Invariant (holds by construction):** at every ISR entry `fill_cnt ∈ {0, 3, …, 6141}`, because the only increment and the only reset are the two statements at `:438` / `:446` and every path reaches them. Therefore `&b[fill_cnt + 2] ≤ &b[6143]` — the OOB write is unreachable, and no path can leave an arena unfilled.

**Cost.** TIM2 fires at 1600 Hz (`tim.c:44` PSC=124, `:46` Period=419, APB1 timer clock 84 MHz → 84e6/(125·420) = 1600.0000 Hz → 625 µs period). The error path gains the wrap block (~4 instructions ≈ 24 ns at 168 MHz); the success path is unchanged. Added cost ≈ 0.004 % of the ISR period — no effect on the sampling budget or on the 12 294 B chunk cadence (1.28 s per arena).

**Acceptance gate (measured 2026-11-15):** `Select-String src\main.c -Pattern 'fill_cnt \+= 3u'` → **1 match**, `main.c:438`, immediately before the wrap block at `:439`. Equivalent to the audit gate `grep -n "fill_cnt += 3u" src/main.c` = exactly one line.

---

## 3. P2-3 — parser handles truncated / error-shaped files

### 3.1 The hole that remained after MISS-1

`parser_vibl.py` (2026-11-14) stopped only on an **all-0x00** preallocated tail (`if count == 0`). An **all-0xFF** region — what a card returns for sectors that were never written after a failed write (erase state = 1), i.e. exactly what the new SD-error path can leave behind — produced `seq = 0xFFFFFFFF, count = 0xFFFF`:
* `count != 0` → the MISS-1 guard did not fire;
* `seq` is monotonic-greater than any real seq → the monotonicity guard did not fire;
* `count > 2048` → **silent `break`**, no diagnostic at all;
* then the trailer probe read 16 bytes of `0xFF` as `end_ts = 0xFFFFFFFFFFFFFFFF`, `total_records = 0xFFFFFFFF`, `file_crc32 = 0xFFFFFFFF` → **`File CRC32: FFFFFFFF → ❌ MISMATCH`**, a verdict the operator could act on but which was pure noise.

A partial chunk at EOF had the same class of defect: after `⚠️ Warning: File truncated during chunk N` the parser still probed 16 garbage bytes as a trailer.

### 3.2 The rule that is now enforced

There are exactly two outcomes after the chunk loop, and they are mutually exclusive:

| Loop ended because… | Parser prints | `File CRC32` line |
|---|---|---|
| a 16-byte trailer candidate passed `trailer_candidate()` (`:70-87`) | `--- TRAILER ---` + `✅ MATCH` / `❌ MISMATCH` | **yes**, only here |
| tail is blank: `count == 0` **or** `count == 0xFFFF` (`:115-117`), or the whole remaining region is uniformly 0x00 / 0xFF (`tail_is_blank()`, `:89-97`, used at `:150`) | `⚠️ No trailer found (power loss/error)` | **never** |
| header is garbage (`count > 2048` `:123-125`, non-monotonic `seq` `:118-120`) or the chunk is cut off by EOF (`:127-131`) | the matching truncation warning + `⚠️ No trailer found (power loss/error)` | **never** |
| fewer than 16 bytes left after the last chunk | `⚠️ No trailer found (power loss/error)` (`:175-177`) | **never** |

Chunk statistics (`Number of Chunks`, `Total Records (Chunks)`, gap detection) and `--plot` are printed in every case — the samples up to the last successfully `f_sync`'ed chunk are intact and remain analysable.

`trailer_candidate()` additionally rejects an all-0xFF 16-byte block (`:85-86`), and the probe branch has a belt-and-braces check `trailer_bytes == b'\x00'*16 or b'\xff'*16` (`:163-165`). `tail_is_blank()` uses `bytes.count(sub, start)`, which is C-speed and does not copy the slice: a 44 258 656 B file is fully parsed and scanned in **0.49 s** (measured, acceptance was < 1 s).

### 3.3 Fixture matrix (measured 2026-11-15)

| Fixture | Shape | Result |
|---|---|---|
| `test/SYNTH01.VIB` | clean stop, trailer present | `Header CRC: OK`, `File CRC32: 64B68650 → ✅ MATCH`, 5 chunks / 10 240 records, `Gap … 2 → 10` (regression check) |
| `test/SYNTH_POWERLOSS.VIB` | 10 chunks + 44 135 460 B of **0x00** | `⚠️ No trailer found (power loss/error)`, 10 chunks / 20 480 records, `Gaps Found: No`, no CRC line |
| `test/SYNTH_FFTAIL.VIB` (new) | 10 chunks + 44 135 460 B of **0xFF**, no trailer | `⚠️ No trailer found (power loss/error)`, 10 chunks / 20 480 records, **no `File CRC32` line**, 0.49 s |
| `test/SYNTH_TRUNC_PARTIAL.VIB` (new) | 4 chunks + 6-byte header of chunk 4 + only 300 of its 12 288 payload bytes | `File truncated during chunk 4` + `⚠️ No trailer found (power loss/error)`, 4 chunks / 8 192 records; `--plot` renders (verified with `MPLBACKEND=Agg`) |
| `test/SYNTH_SDERROR_CLOSED.VIB` (new) | the file the new shutdown path produces: 4 chunks truncated at `vibl_data_end` + trailer | `File CRC32: DB132020 → ✅ MATCH`, 4 chunks / 8 192 records |
| `test/SYNTH_STALE.VIB` | stale plausible chunk headers | `⚠️ No trailer found (power loss/error)`, 11 chunks, `Gap … 9 → 1326` |

---

## 4. P2-2 — the new recovery / shutdown strategy

### 4.1 Flow

```
chunk write (main.c:322-330)
      |
      +-- VIBL_OK ----------------------------------> continue recording
      |
      +-- VIBL_ERR_WRITE / VIBL_ERR_SYNC
              |
              v
   sd_error_shutdown()                       (main.c:129-168)
     1. loop attempt = 0..SD_RECOVERY_ATTEMPTS-1    (SD_RECOVERY_ATTEMPTS = 3u, main.c:48)
          CD gate: PA8 LOW (card absent) -> break out immediately      (main.c:138-140)
          HAL_IWDG_Refresh                                             (main.c:141)
          HAL_SD_Init(&hsd) == HAL_OK -> sd_recovery_ok=1, HAL_Delay(100), break  (:142-146)
          HAL_IWDG_Refresh, HAL_Delay(200), HAL_IWDG_Refresh -> retry  (:148-150)
     2. vibl_close()  -> header record_count + header_crc, f_sync,
                         f_lseek(vibl_data_end) + f_truncate(), trailer, f_sync, f_close
     3. HAL_TIM_Base_Stop_IT(&htim2)                                   (main.c:161)
     4. f_mount(NULL, "0:", 0)   (R0.12c unmount, ff.c:3277-3293)      (main.c:165)
     5. hil3_error_loop(5)  — parks forever, feeds IWDG every 200 ms   (main.c:167)
```

Error code **5** keeps its existing meaning ("SD write error", already used by the PC13 stop path at `main.c:286`). New watch symbols: `sd_recovery_ok` (`main.c:127`) and `vibl_dbg_sd_error` (`sd_writer.c:94`); the FRESULT of the last failing call is still in `vibl_dbg_last_fr` / `vibl_dbg_attempts` (FIX-C, `sd_writer.c:89-93`). TIM2 is stopped AFTER `vibl_close()` because the TIM2 callback touches RAM only (`arena` / `full_mask` / `dropped`) and cannot interfere with the FatFs work; the PC13 stop path keeps its own, deliberately reversed order (stop first — documented at `main.c:268-272`).

### 4.2 Two facts that shaped the design (read out of `src/ff.c`, not from memory)

**a) Recovery must NOT re-mount.** `find_volume` sets `fs->id = ++Fsid` on every mount (`ff.c:3192`) and `validate()` accepts an object only if `obj->id == obj->fs->id` (`ff.c:3224`). Calling `f_mount(&fs,"0:",1)` inside the shutdown would invalidate the **already open** `vibl_fil`, and every later `f_lseek` / `f_truncate` / `f_sync` would answer `FR_INVALID_OBJECT` — the trailer could never be written. Plain `HAL_SD_Init(&hsd)` is sufficient: `validate()` consults only `disk_status()` (`ff.c:3236`), which in this tree is the PA8 card-detect read (`diskio.c:26`), and `fs->fs_type` stays non-zero. The only `f_mount` in the shutdown is the final unmount, after which no FatFs call is made.

**b) A failed chunk must not close the file.** `vibl_write_chunk()` used to call `vibl_abort()` on failure (`sd_writer.c:383/387/398` in the 2026-11-14 revision); `vibl_abort()` does `f_close()` + `vibl_active = 0` (`sd_writer.c:169-175`), so the prescribed `vibl_close()` could only return `VIBL_ERR_NOT_OPEN` (`sd_writer.c:466-470`) and write nothing. Fix: `vibl_stall()` (`sd_writer.c:194-198`) keeps the FIL open and clears `vibl_fil.err`, because after a disk error FatFs latches the code in `FIL.err` (`ABORT`, `ff.c:455`) and `f_write` (`ff.c:3627`), `f_lseek` (`ff.c:4015`) and `f_truncate` (`ff.c:4485`) return the stale code **without attempting any I/O** — without the unlatch the whole close sequence is a silent no-op. The seven `vibl_abort()` calls inside `vibl_close()` are untouched (measured: `vibl_stall();` = 3, `vibl_abort();` = 7).

### 4.3 Why the resulting file is *valid*, not merely "saved"

`vibl_data_end` (`sd_writer.c:431`) and `vibl_file_crc` (`sd_writer.c:416-417`) advance **only after both `vibl_write_all()` calls of that chunk succeed**, and `vibl_close()` seeks to `vibl_data_end` (`sd_writer.c:514`) and calls `f_truncate()` (`sd_writer.c:518`) before writing the trailer (`sd_writer.c:523-528`). Consequently:

* the bytes the dead chunk managed to land on the card are cut off, not left in the middle of the stream;
* the trailer's `file_crc32` covers exactly the payload that survives → the parser reports `✅ MATCH` (proven by `test/SYNTH_SDERROR_CLOSED.VIB`, CRC `DB132020`);
* the 42 MiB preallocated tail (P1-7) is released by `remove_chain()` instead of being stranded on the card.

### 4.4 Time budget

| Stage | Bounded by | Numbers |
|---|---|---|
| The failing chunk itself | `VIBL_WRITE_RETRY_MAX = 3u` (`sd_writer.c:88`) × `HAL_SD_WriteBlocks(…, 1000U)` (`diskio.c:88`) | 3.000 s max, `HAL_IWDG_Refresh` after every attempt (`sd_writer.c:158`) — fits `T_IWDG = 4.095 s` (`iwdg.c:45-46`: 4095 × 32 / 32000) |
| Recovery loop | `SD_RECOVERY_ATTEMPTS = 3u`, IWDG refreshed at `main.c:141/145/148/150` | ≥ 0.6 s of explicit `HAL_Delay(200)`; the **card-absent** case costs ≈ 0 s — the CD gate breaks before any CMD retry |
| Close | header write + `f_sync` + `f_lseek` + `f_truncate` over the preallocated tail | ~10 300 clusters at 4 KiB on a 32 GB card ≈ 0.3-0.5 s (same basis as audit E7/F1); IWDG fed on every successful `disk_write` (`diskio.c:124`) |
| Park | `hil3_error_loop(5)` (`main.c:451-467`) | refreshes every 200 ms blink and every 100 ms pause |

Known limits of this strategy — what it deliberately does **not** do — are listed in §7.

## 5. Verification evidence (all measured 2026-11-15, no board)

| Check | Command | Result |
|---|---|---|
| BLOCKER-1 gate | `Select-String src\main.c -Pattern 'fill_cnt \+= 3u'` | **1 match** → `main.c:438` (before the wrap block at `:439`) |
| Shutdown wiring | `Select-String src\sd_writer.c -Pattern 'vibl_stall\(\);'` / `'vibl_abort\(\);'` | **3** (all inside `vibl_write_chunk`) / **7** (all inside `vibl_close`) |
| Shutdown wiring | `Select-String src\main.c -Pattern 'sd_error_shutdown'` | **3** — definition `:129`, comment `:317`, call `:332` |
| Parser regression + new shapes | `python parser_vibl.py test\…` on 6 fixtures | see §3.3; `SYNTH01` and `SYNTH_SDERROR_CLOSED` → `✅ MATCH`, the four no-trailer fixtures → no `File CRC32` line at all |
| Parser speed / memory | stopwatch around the 44 258 656 B all-0xFF file | **0.49 s**, 10 chunks held in memory, no OOM |
| Plot path on a truncated file | `set MPLBACKEND=Agg && python parser_vibl.py test\SYNTH_TRUNC_PARTIAL.VIB --plot` | `Rendering plot…` executed (only the Agg "non-interactive" notice) |
| Build after P2-1 | `pio run -e release` | SUCCESS — RAM 27 428 B / Flash 25 840 B |
| Build after P2-1 + P2-2 | `pio run -e release`, `pio run -e debug` | SUCCESS — release RAM 27 436 B (20.9 %) / Flash 25 976 B (2.5 %); debug RAM 27 448 B / Flash 35 832 B |
| Clean rebuild, optimisation gate | `pio run -e release -t clean` then `pio run -e release -v` | 194 log lines, **109 `arm-none-eabi-gcc` invocations, 107 of them carrying `-O2`, `-Os`: 0**, `-ffast-math` / `-Ofast`: 0, `-DHSE_VALUE=8000000` present; **1 warning total** — the pre-existing `src/ff.c:5032:2: this 'for' clause does not guard [-Wmisleading-indentation]` (its note is at `src/ff.c:5052`); nothing from `main.c`, `sd_writer.c` or `diskio.c` |
| Sections (release) | size report | `.isr_vector` 392, `.text` 25 744, `.rodata` 212, `.data` 20, `.bss` 27 416, `._user_heap_stack` 1 540 |
| Link gate | `arm-none-eabi-nm -u .pio\build\release\firmware.elf` | no unresolved HAL/FatFs references |

Note for debugging: at `-O2` both `sd_error_shutdown()` and `vibl_stall()` are inlined (they do not appear in the `nm` symbol table); set breakpoints by **file:line** (`main.c:129/156/161/167`, `sd_writer.c:194`) or use the `-e debug` (`-O0 -g3`) image for stepping.

---

## 6. Rule-15 contradiction register (what is now stale, and where it was flagged)

| Statement | Where it lives | Action taken |
|---|---|---|
| "…that gate is currently **NOT** satisfied — see `doc\final_pre_flash_audit.md` BLOCKER-1" | `.clinerules/03-workflow.md:37` | **Rewritten**: gate SATISFIED since 2026-11-15, one occurrence at `src\main.c:438`; companion gates added |
| Synthetic-check list without the error-tail fixtures | `.clinerules/03-workflow.md:35` | Extended with `SYNTH_FFTAIL` / `SYNTH_TRUNC_PARTIAL` / `SYNTH_SDERROR_CLOSED` + generator + expected messages |
| Check **C3 = FAIL**, **BLOCKER-1** open, "M4 is gated by BLOCKER-1", hardware step 6 "requires BLOCKER-1 patch", parser "No trailer found (power loss)", O-5 zeros-only | `doc/final_pre_flash_audit.md` | **Annotated in place** (addendum at the top + status lines at C3, BLOCKER-1, W-10, new W-11, M3, M4, steps 6 and 8, O-5, new O-11/O-12). The 2026-11-14 findings were **not** rewritten |
| "`vibl_write_chunk()` failure ⇒ `hil3_error_loop(5)`" | `doc/ANSWER_TO_AUDIT.md:98-107`, `doc/report2.txt` (historical patch logs) | Left as-is (historical record); superseded behaviour documented here (§4) and flagged in the audit addendum |
| Parser message text `No trailer found (power loss)` quoted in older audits | `doc/audit_13_11_26.md`, `doc/report2.txt` | Left as-is; current text is `⚠️ No trailer found (power loss/error)` (`parser_vibl.py:159/165/177`). Reminder: those two files are **cp1251**-encoded — re-check encoding before quoting them |
| `doc/report.md:40` P2-8 "ISR error branch burns the slot (`fill_cnt += 3u`)" | `doc/report.md` | Still accurate; now the burn and the wrap are **shared** by both paths — see §2.2 |
| All `main.c:NNN` / `sd_writer.c:NNN` citations in `doc/final_pre_flash_audit.md` (checks C1, C4, C5, D3, D4, E4-E7, F1-F5, G3-G4, M3-M9) | `doc/final_pre_flash_audit.md` | Valid for the **2026-11-14 revision**; they now drift by **+6 / +82 / +91** in `main.c` and **+1 / +24** in `sd_writer.c` (drift table added to that file's addendum). Old numbers were deliberately NOT rewritten, so the original evidence remains auditable |

## 7. Known limits of the new shutdown strategy

1. **No resume after a successful `HAL_SD_Init()`.** The session always ends. Continuing would need a "discard the abandoned chunk, stay aligned" primitive in `sd_writer.c` (`f_lseek(vibl_data_end)` + `f_truncate()` + `FIL.err` unlatch, i.e. a future `vibl_recover_chunk()`) plus a decision about the samples of the abandoned arena — they are already consumed and gone. Owner decision, tracked as **O-11** in `doc/final_pre_flash_audit.md`. Rationale for the current choice: a *truncated but CRC-valid* file is worth more than a *long* file with an undetectable hole in the middle of the stream.
2. **The samples of the abandoned chunk are not reported anywhere.** `vibl_mark_gap(VIBL_SAMPLES_PER_CHUNK)` would only bump `next_chunk_seq`; with no following chunk written the seq jump is invisible to the parser. Same root cause as audit **O-2** (per-failure gap accounting needs a record-level invalid flag in the VIBL format).
3. **UNCERTAIN (hardware, audit U-3 / W-5):** `HAL_SD_Init()` contains no watchdog feed, and the SDMMC waits are millisecond-scaled counters (`SDIO_CMDTIMEOUT = 5000U`, `Drivers/STM32F4xx_HAL_Driver/Inc/stm32f4xx_ll_sdmmc.h:325`; `Src/stm32f4xx_ll_sdmmc.c:1330-1331` converts it via `SystemCoreClock / 8U / 1000U`). A card that is physically inserted but electrically dead can therefore make one attempt outlive `T_IWDG = 4.095 s` → the board resets instead of blinking code 5. Data is still safe (chunk-granular `f_sync`); only the trailer is missing, which the parser now reports as `No trailer found (power loss/error)`.
4. **UNCERTAIN (hardware):** `disk_read` / `disk_write` feed the watchdog on the **success** path only (`diskio.c:71`, `diskio.c:124`) and `return RES_ERROR` before any refresh on failure (`diskio.c:68`, `diskio.c:89`). A single FatFs call whose every sector times out — e.g. `f_truncate()` walking ~10 300 clusters inside `vibl_close()` — is not bounded by 4.095 s in the current code, so on a dying card a reset can win. Candidate remedies, **deliberately not applied in this documentation round**, tracked as **O-12**: (a) `HAL_IWDG_Refresh(&hiwdg)` before the `RES_ERROR` returns in `diskio.c:68` / `:89`; (b) skip `f_truncate()` when `sd_recovery_ok == 0` (the next session's `FA_CREATE_ALWAYS` releases the chain anyway) so the close sequence stays short and the blink becomes reachable.

---

## 8. Hardware acceptance for Milestone 2 (NOT executed — rule 18)

Flashing (`pio run -e release -t upload`) was not performed and is not performed by this milestone. Steps, in order:

1. **Sensor-fault injection, proves §2 / BLOCKER-1.** During a recording, lift `CS_LIS` (PA4, `doc/WEACT_F405_PINOUT.md` §6) or momentarily disconnect MISO (PA6) for ≥ 10 ms ⇒ ≥ 10 consecutive `lis3dsh_read()` failures ⇒ `lis3dsh_err` ⇒ blink **code 4**, and the arenas must stay inside bounds. Watch: `fill_cnt` must never read ≥ 6144 (`main.c:64`), `full_mask`, `dropped`, `spi_err_streak`. Expected file: contiguous seq chain, possibly `dropped > 0` only if the consumer was slow; `parser_vibl.py` must still report `✅ MATCH`.
2. **SD-stall injection, proves §4.** During a recording, either pull the card out (PA8 goes LOW ⇒ `disk_status()` → `STA_NOINIT` ⇒ `validate()` → `FR_INVALID_OBJECT` ⇒ `VIBL_ERR_WRITE`) or, in a **temporary test build**, add `HAL_Delay(1500)` in `disk_write` (`diskio.c:88` path). Observe on the LED: exactly **5** short flashes then a 1 s pause, repeated; watch `sd_recovery_ok` (`main.c:127`), `vibl_dbg_sd_error` (`sd_writer.c:94`), `vibl_dbg_last_fr`, `hsd.ErrorCode`. Expected on the card: a file that parses as `File CRC32 … ✅ MATCH` (bus recovered ⇒ trailer written) or `⚠️ No trailer found (power loss/error)` (bus not recovered / reset) — in **both** cases the chunk count equals the last successfully `f_sync`'ed chunk, and no `File CRC32 … MISMATCH` line ever appears.
3. **No-watchdog-reset check.** Repeat step 2 ten times, verifying that the board parks in the blink loop instead of resetting (distinguish by `vibl_get_chunk_count()` continuity and by the absence of a fresh `vibl_open()` — see `main.c` boot path). If a reset wins, close **O-12** first.
4. **M4 hot-removal (unblocked since BLOCKER-1 closed).** Record, pull the card ~3 s, re-insert, stop with PC13, parse ⇒ `Gaps Found: Yes` and `dropped > 0`; expect a *truncated but valid* file per §7 item 1.
5. **Soak (M9, U-2).** 24 h with DWT_CYCCNT instrumentation around `vibl_write_chunk` ⇒ P99 < 200 ms, `dropped == 0`, no reset.

---

## 9. How to re-verify this milestone from scratch

```bat
:: 1. static gates
powershell -NoProfile -Command "(Select-String src\main.c -Pattern 'fill_cnt \+= 3u').Count"     :: expect 1
powershell -NoProfile -Command "(Select-String src\sd_writer.c -Pattern 'vibl_stall\(\);').Count" :: expect 3
powershell -NoProfile -Command "(Select-String src\sd_writer.c -Pattern 'vibl_abort\(\);').Count" :: expect 7

:: 2. rebuild both profiles and confirm -O2 (no -Os)
pio run -e release -t clean -v
pio run -e debug

:: 3. regenerate the synthetic fixtures and run all six through the parser
python test\mk_synth_vibl.py
python test\mk_synth_powerloss.py
python test\mk_synth_error_tail.py
for %f in (test\SYNTH*.VIB) do @python parser_vibl.py %f

:: 4. link/wiring check
"%USERPROFILE%\.platformio\packages\toolchain-gccarmnoneeabi\bin\arm-none-eabi-nm" -u .pio\build\release\firmware.elf
```

Expected parser output for the six files: `SYNTH01` `MATCH 64B68650` · `SYNTH_SDERROR_CLOSED` `MATCH DB132020` · `SYNTH_POWERLOSS`, `SYNTH_FFTAIL`, `SYNTH_STALE`, `SYNTH_TRUNC_PARTIAL` → `⚠️ No trailer found (power loss/error)` and **no** `File CRC32` line.

**Not covered by this milestone (unchanged):** 4-bit SDIO (`sdio.c:45` still `SDIO_BUS_WIDE_1B`, `ClockDiv = 16` → SDMMC_CK = 48/(16+2) = 2.667 MHz), per-failure gap flags (O-2), RTC-set policy (O-9), fixed filename `LOG0003.VIB` (W-7), CubeMX regeneration check (W-3/O-7).

