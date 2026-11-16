# MILESTONE 2 — Robustness: ISR integrity (BLOCKER-1), graceful SD-error shutdown, parser truncated-file handling

**Status:** CODE-COMPLETE, STATICALLY VERIFIED. Not flashed, not bench-verified — flashing requires explicit user instruction (rule 18), and the hardware acceptance list (§7) is still open.
**Date:** 2026-11-15.
**Supersedes:** `doc/final_pre_flash_audit.md` — check **C3 / BLOCKER-1 (FAIL → CLOSED)** and the parser's zeros-only power-loss path. The audit file is annotated in place, its 2026-11-14 findings are not rewritten (rule 15: newer artefact is authoritative, contradiction is flagged explicitly, never silently merged).

**Addendum 2026-11-16 — Milestone 2b, V2 LOSSY MODE (§10):** a card GC stall no longer ends the session. The §4 shutdown strategy stays in place as the FALLBACK; §4.4, §5 gates, §7 items 1-2 and §8 items 2-3/5 are annotated **SUPERSEDED** in place, nothing is deleted, for exactly the same reason as in the line above.
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

> **SUPERSEDED (2026-11-16, §10):** this table is the budget of the *fallback* path. Under V2 Lossy Mode the ordinary GC stall no longer reaches stages 2-4 of §4.4 at all: the failed chunk costs its 1 × 2000 ms (`VIBL_WRITE_RETRY_MAX = 1u`, `sd_writer.c:110`), then `sd_gap_log` (60 ms) + `sd_error_recover` (1 × `HAL_SD_Init` + 100 ms) + the recovery `f_lseek` (≤ 2000 ms, IWDG-fed) are paid and the loop **continues**. `Close` and `Park` are only reached when the recovery itself fails. Note the stale numbers in the first row (retry 3u / 1000U) — the tree is at 1u / 2000U since FIX-B2 / FIX-F.

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

> **Status of items 1-4 on 2026-11-16 (rule 15 — flagged, not rewritten).**
> *Items 1 and 2 are SUPERSEDED by §10 (Milestone 2b, V2 Lossy Mode):* the session now resumes after a dropped chunk, the primitive item 1 asked for exists (`vibl_recover()` / `vibl_rewind_to_data_end()`, `src/sd_writer.c:235-275`, `:613-630`), and the dropped chunk IS reported — `src/main.c:461` calls `vibl_mark_gap(lost_chunks * VIBL_SAMPLES_PER_CHUNK)` and because recording continues there *is* a following chunk, so the seq jump is visible to `parser_vibl.py`. **O-11 is CLOSED.** Item 2 stays true only for a *trailing* hole (§10.5 item 1).
> *Item 3 is unchanged and is now the main residual risk* — Lossy Mode calls `HAL_SD_Init()` once per dropped chunk, so a physically-present-but-electrically-dead card can still outlive `T_IWDG` and reset the board. Card-granular data safety is unchanged.
> *Item 4 is CLOSED by FIX-E (2026-11-16):* `disk_read` and `disk_write` refresh the IWDG on the failure path too (`src/diskio.c:87`, `:116`), and the timeouts are 2000U with `VIBL_WRITE_RETRY_MAX = 1u` (`src/diskio.c:85`, `:114`; `src/sd_writer.c:110`). The `diskio.c:68/71/89/124` citations in this item are stale by 1-35 lines.

1. **No resume after a successful `HAL_SD_Init()`.** The session always ends. Continuing would need a "discard the abandoned chunk, stay aligned" primitive in `sd_writer.c` (`f_lseek(vibl_data_end)` + `f_truncate()` + `FIL.err` unlatch, i.e. a future `vibl_recover_chunk()`) plus a decision about the samples of the abandoned arena — they are already consumed and gone. Owner decision, tracked as **O-11** in `doc/final_pre_flash_audit.md`. Rationale for the current choice: a *truncated but CRC-valid* file is worth more than a *long* file with an undetectable hole in the middle of the stream.
2. **The samples of the abandoned chunk are not reported anywhere.** `vibl_mark_gap(VIBL_SAMPLES_PER_CHUNK)` would only bump `next_chunk_seq`; with no following chunk written the seq jump is invisible to the parser. Same root cause as audit **O-2** (per-failure gap accounting needs a record-level invalid flag in the VIBL format).
3. **UNCERTAIN (hardware, audit U-3 / W-5):** `HAL_SD_Init()` contains no watchdog feed, and the SDMMC waits are millisecond-scaled counters (`SDIO_CMDTIMEOUT = 5000U`, `Drivers/STM32F4xx_HAL_Driver/Inc/stm32f4xx_ll_sdmmc.h:325`; `Src/stm32f4xx_ll_sdmmc.c:1330-1331` converts it via `SystemCoreClock / 8U / 1000U`). A card that is physically inserted but electrically dead can therefore make one attempt outlive `T_IWDG = 4.095 s` → the board resets instead of blinking code 5. Data is still safe (chunk-granular `f_sync`); only the trailer is missing, which the parser now reports as `No trailer found (power loss/error)`.
4. **UNCERTAIN (hardware):** `disk_read` / `disk_write` feed the watchdog on the **success** path only (`diskio.c:71`, `diskio.c:124`) and `return RES_ERROR` before any refresh on failure (`diskio.c:68`, `diskio.c:89`). A single FatFs call whose every sector times out — e.g. `f_truncate()` walking ~10 300 clusters inside `vibl_close()` — is not bounded by 4.095 s in the current code, so on a dying card a reset can win. Candidate remedies, **deliberately not applied in this documentation round**, tracked as **O-12**: (a) `HAL_IWDG_Refresh(&hiwdg)` before the `RES_ERROR` returns in `diskio.c:68` / `:89`; (b) skip `f_truncate()` when `sd_recovery_ok == 0` (the next session's `FA_CREATE_ALWAYS` releases the chain anyway) so the close sequence stays short and the blink becomes reachable.

---

## 8. Hardware acceptance for Milestone 2 (NOT executed — rule 18)

Flashing (`pio run -e release -t upload`) was not performed and is not performed by this milestone. Steps, in order:

> **SUPERSEDED 2026-11-16 (see §10.6 for the current procedure):** step 2 and step 3 describe the shutdown park (blink code 5) as the expected outcome of an SD stall. Under V2 Lossy Mode a stall now produces **one LED flash per dropped chunk and no park**: the expected evidence is `Gaps Found: Yes` in the parser, `sd_gap_events > 0`, `sd_fatal_events == 0` and a trailer that still reports `✅ MATCH`. Step 5's `dropped == 0` soak criterion is replaced by "gaps allowed and counted, session never ends". Steps 1 and 4 (sensor-fault injection, hot-removal) are unchanged.

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

> **Re-issued 2026-11-16 for Milestone 2b — use §10.7 instead of the bat block above.** The gates above drifted (`vibl_stall();` is now **4**, `sd_error_shutdown` references in `main.c` are now **6**, `fill_cnt += 3u` is now `main.c:575`), and the fixture set is now **eleven** files: the six above (unchanged verdicts, `SYNTH01` additionally reports its pre-existing gap 2→10) plus `SYNTH_LOSSY_GAP` (`✅ MATCH A52BC412`, `Gaps Found: Yes`), `SYNTH_LOSSY_HEADGAP` (`✅ MATCH EABBBB4F`, head gap), `SYNTH_LOSSY_OVERWRITE` (`✅ MATCH 15D16328`, byte-level rewind/overwrite proof) and `SYNTH_LOSSY_GAP_NOTRAILER` (gaps + no `File CRC32` line). Every one of them must exit with code **0**; a gap never changes that. The generator for the new four is `python test\mk_synth_lossy_gap.py`.

**Not covered by this milestone (unchanged):** 4-bit SDIO (`sdio.c:45` still `SDIO_BUS_WIDE_1B`, `ClockDiv = 16` → SDMMC_CK = 48/(16+2) = 2.667 MHz), per-failure gap flags (O-2), RTC-set policy (O-9), fixed filename `LOG0003.VIB` (W-7), CubeMX regeneration check (W-3/O-7).

---

## 10. Milestone 2b (2026-11-16) — V2 **LOSSY MODE**: survive a GC stall instead of ending the session

**Status:** CODE-COMPLETE, BUILT, STATICALLY + SYNTHETICALLY VERIFIED (no board). Not flashed — flashing still requires an explicit user instruction (rule 18).
**Owner decision recorded:** the consumer SD card is at its physical limit and its GC stalls exceed the 2000 ms HAL bound. V2 accepts a **lossy** stream: drop the affected chunk, record the hole, keep recording. **Continuous recording time now outranks a lossless stream.**
**Closes:** **O-11** of `doc/final_pre_flash_audit.md` (the `vibl_recover_chunk()` primitive it asked for) and §7 items 1-2 of this file.

### 10.1 Change set

| ID | Patch | File:lines | Purpose |
|----|-------|-----------|---------|
| **V2-1a** | Main loop no longer halts on a failed chunk | `src/main.c:421-469` | Drop → gap-mark → log → recover → **continue**. `sd_error_shutdown()` is demoted to a fallback (`main.c:465`, reached only if the recovery itself fails) |
| **V2-1b** | `sd_error_recover()` | `src/main.c:211-256` | CD gate → refresh → **exactly one** `HAL_SD_Init(&hsd)` (`:240`) → refresh → `HAL_Delay(100)` (`:242`) → refresh → `vibl_recover()` (`:250`). Returns 0 = keep recording |
| **V2-1c** | Gap log | `src/main.c:145-164` (counters), `:262-272` (`sd_gap_log`), `:63` (`SD_GAP_BLINK_MS 30u`) | No UART exists in this tree (`include/main.h:60-67` defines only BUTTON / CS_LIS / Led / SD_DETECT), so an event = 1 LED flash (30 ms ON + 30 ms OFF, PB2 Active HIGH, `doc/WEACT_F405_PINOUT.md` §2) + the debugger counters `sd_gap_events`, `sd_fatal_events`, `sd_last_write_rc` |
| **V2-1** | Recovery primitive | `src/sd_writer.c:228-231` (`vibl_stall` + CRC rollback), `:235-275` (`vibl_rewind_to_data_end`), `:613-630` (`vibl_recover`) | Unlatch `FIL::err`, rewind `fptr` to `vibl_data_end`, roll the running CRC back |
| **V2-1d** | Self-aligning chunk writer + CRC anchor + seeds | `src/sd_writer.c:563-569` (guard at the top of `vibl_write_chunk`), `:48-57` (`vibl_chunk_crc_start`), `:375-383` (`vibl_data_end` seeded to 256 in `vibl_open`) | A chunk can only ever be appended at the last committed byte |
| **V2-2** | Parser: gaps are an expected state | `parser_vibl.py:7-20` (optional plot deps), `:22-33` (geometry + exit codes), `:230-238`, `:247-282` (gap analysis + head-gap branch), `:308-345` (verdict + status), `:347-348` (`sys.exit`) | Quantified gap report, no crash, documented exit codes; gaps do **not** change the exit status |
| **V2-3** | Lossy-mode fixtures | `test/mk_synth_lossy_gap.py`, `test/SYNTH_LOSSY_GAP.VIB`, `test/SYNTH_LOSSY_HEADGAP.VIB`, `test/SYNTH_LOSSY_GAP_NOTRAILER.VIB`, `test/SYNTH_LOSSY_OVERWRITE.VIB` | Board-free proof that a gapped, recovered stream still parses as `✅ MATCH`; the 4th fixture simulates the **byte-level** write pattern (4 stalled chunks that each left 7 006 B mid-stream) and asserts the result is byte-identical to a gap chain with no orphan bytes |

### 10.2 Why resuming is now safe (the §7 item 1 objection, answered)

Milestone 2 refused to resume because "the stream offset after a half-written chunk can no longer be guaranteed". It **can** be guaranteed, and that guarantee is the whole patch:

1. `f_write()` advances `fp->fptr` **only by the bytes it actually committed** (`ff.c:3635-3637`; `ABORT` at `ff.c:455` returns from inside the loop body). A dead chunk therefore leaves 0 … 12 294 bytes punched between `vibl_data_end` and `fptr` — `vibl_data_end` advances only after a fully successful chunk (`sd_writer.c:596`).
2. `vibl_recover()` seeks `fptr` back to `vibl_data_end`, so the **next chunk overwrites the dead range byte-for-byte**: both are `chunk_hdr(6) + 2048 × 6 = 12 294 B` (`include/sd_writer.h:30`, `:80-84`). No orphan bytes stay in the middle of the stream, so `parser_vibl.py`'s sequential `[hdr][samples]` walk can never read int16 sample data as a chunk header. This is exactly the "undetectable hole in the middle of the stream" that §7 item 1 was written to avoid — it is now structurally impossible.
3. `FIL::err` must be cleared **before** the seek. `f_lseek` returns the stale latched code without doing any I/O (`ff.c:4015`), and `f_write` (`ff.c:3627`) / `f_truncate` (`ff.c:4485`) behave the same way — that is the TASK-2 requirement ("the next `vibl_write_chunk()` must actually attempt to write again instead of instantly returning the stale error"). `vibl_stall()` (`sd_writer.c:230`) and `vibl_rewind_to_data_end()` (`:258`) both clear the latch, and the rewind's failure branch clears it **a second time** so that `vibl_close()` can still write the trailer.
4. Running-CRC integrity: `vibl_write_chunk()` folds the chunk header + samples into `vibl_file_crc` **before** the committing `f_sync()` (`sd_writer.c:581-582` vs `:585`), so a chunk that dies in `f_sync` had already polluted the CRC while `next_chunk_seq` / `total_records` / `chunk_count` (`:591-593`) stayed un-advanced. `vibl_stall()` restores the snapshot taken at entry (`:569` → `:230`). Without that rollback **every** dropped chunk would end the session with `⚠️ Trailer CRC mismatch` — Lossy Mode would have been unverifiable.
5. `f_truncate()` is deliberately **not** called during recovery, even though the O-11 wording suggested it. In STRETCH mode it would release the whole remaining preallocated tail (44 258 656 / 4096 ≈ **10 805 clusters** of `remove_chain()`, 0.3-0.5 s) and force re-allocation for every following chunk. Only the ≤ 3 clusters (12 294 / 4096) of the last dead chunk stay allocated, and `vibl_close()` releases them at the end (`sd_writer.c:611`) exactly as §4.3 describes.
6. Cost of the rewind: a backward `f_lseek` restarts at the first cluster (`ff.c:4092-4103`) and follows the chain with `create_chain()`, which returns the **existing** next cluster without `put_fat` (`ff.c:1369`) because both preallocation modes already linked it; a backward seek never grows `objsize` (`ff.c:4134` is guarded by `fp->fptr > fp->obj.objsize`). `_USE_FASTSEEK = 0` (`include/ffconf.h:18`), so there is no CLMT shortcut, and `_FS_TINY = 0` (`ffconf.h:49`) means the sector-cache write-back + sector read path (`ff.c:4141-4148`) is live. Worst case at the end of a full 76.8-min file ≈ 10 805 `get_fat` steps over FAT sectors holding 128 entries → ≈ **85 sector reads** ≈ 85 × 1.54 ms ≈ **0.13 s**, each op bounded by 2000 ms and each feeding the IWDG on both paths (`src/diskio.c:87`, `:116`). On the healthy path the guard costs **one integer compare**: `f_tell` is the macro `(fp)->fptr` (`include/ff.h:278`), no disk access.

7. No re-mount anywhere (`src/main.c:215-223`): `f_mount` would run `fs->id = ++Fsid` (`ff.c:3192`) while `validate()` demands `obj->id == obj->fs->id` (`ff.c:3224`), which would invalidate the open FIL and turn every later call — including the close — into a silent `FR_INVALID_OBJECT` no-op. The §4.2a analysis is unchanged and still correct.

### 10.3 New control flow in the main loop

```
vibl_write_chunk(arena[0..1])  == VIBL_OK  ─────────────────────────> continue
        |
        +-- VIBL_ERR_WRITE (-4) / VIBL_ERR_SYNC (-5)      main.c:435 / :445
                |
                |  lost_chunks++            (also for the arena skipped after a
                |                           timeout, main.c:447)
                v
        vibl_mark_gap(lost_chunks * 2048)   main.c:461   explicit seq jump
        vibl_mark_gap(d)                    main.c:453   arenas TIM2 could not hand over
        sd_gap_log(rc)                      main.c:462   LED flash + counters
        sd_error_recover()                  main.c:463   CD gate -> 1 x HAL_SD_Init
                |                                         -> 100 ms -> vibl_recover()
                +-- 0  -> fall through (main.c:467): the loop writes the next arena
                +-- !=0 -> sd_fatal_events++ -> sd_error_shutdown()  main.c:465
                          (the unchanged §4.4 fallback: close + trailer + park code 5)
```

**Gap accounting (every hole is counted exactly once, in chunk slots of 2048 samples = 1.28 s):**

| Where the samples are lost | Counted by | Effect on `next_chunk_seq` |
|---|---|---|
| the chunk whose commit timed out | `lost_chunks` (`main.c:435`, `:445`) | +1 |
| the second arena, skipped because the bus already timed out | `lost_chunks++` (`main.c:447`) | +1 |
| arenas TIM2 could not publish (both already full) | `dropped` → `vibl_mark_gap(d)` (`main.c:453`) | +`floor(d / 2048)`, min 1 |
| a stalled *first* chunk of the session | `lost_chunks` + `mark_gap` | first written header carries `seq > 0` |

`vibl_mark_gap()` floors to at least one jumped chunk index (`sd_writer.c:645`, `:650`), advances nothing else, and latches `overflow_latched` (`:652`) which no part of `main.c` consumes — so it can no longer end a session. `total_records`, `chunk_count` and `next_chunk_seq` are advanced only after a successful `f_sync()` (`sd_writer.c:591-593`), which is why a failed chunk consumes **no** sequence number and the jump added by `vibl_mark_gap()` is the exact size of the hole.

Example (the shape of `test/SYNTH_LOSSY_GAP.VIB`): chunk 30 stalls, both arenas are lost (`lost_chunks = 2`), `d = 0` → the written chain is `… 29, 31, 32 … 54, 57, 58 …` → the parser sees one event of 1 missing chunk (29 → 31) and one of 2 (54 → 57).

### 10.4 Time budget of one lossy cycle (watchdog-safe by construction)

| Stage | Bounded by | Measured / computed |
|---|---|---|
| Failed chunk commit | 1 attempt × `HAL_SD_WriteBlocks(…, 2000U)` (`diskio.c:114`, `VIBL_WRITE_RETRY_MAX = 1u` at `sd_writer.c:110`), IWDG fed on **both** paths (`diskio.c:116`) | ≤ 2.000 s |
| Gap log | `2 × HAL_Delay(30)` (`main.c:267`, `:270`) | 60 ms = 4.7 % of the 1 280 ms arena period |
| Bus re-init | 1 × `HAL_SD_Init` + `HAL_Delay(100)`, framed by refreshes (`main.c:239-243`) | ~0.1 s typical; **UNCERTAIN worst case, §7 item 3 unchanged** |
| Stream rewind | 1 backward `f_lseek` + at most 1 cache write-back + 1 sector read (`ff.c:4141-4148`) | ≈ 0.13 s worst case at EOF (§10.2 item 6), ≤ 2000 ms per bounded op |
| Next chunk | as row 1 | ≤ 2.000 s |
| **Total wall time per stall event** | | **≈ 2.3-4.3 s typical, ≤ ~6 s worst case**, with a fresh `T_IWDG = 4095 × 32 / 32000 = 4.095 s` budget (`src/iwdg.c:45-46`) between **every** pair of feeds |
| Sampling holes caused by that wall time | 1600 Hz producer vs. a blocked consumer | 4.3 s / 1.28 s ≈ 3.4 arenas lost, all of them gap-counted per §10.3 |

`sd_error_shutdown()` keeps its original §4.4 budget unchanged; it is now reached only when `sd_error_recover()` fails.

### 10.5 What Lossy Mode does **not** fix (accepted residuals)

1. **A trailing hole is still invisible to the parser.** A hole with no following chunk cannot bracket itself — unchanged from §7 item 2, and `test/SYNTH_LOSSY_GAP.VIB` models it deliberately (chunk 99 dropped, reported count = 3 events, not 4). The on-board counters (`sd_gap_events`, `vibl_dbg_recover_count`, `vibl_get_dropped_samples()`) are the only place that number exists. Writing the gap total into the trailer's 159 reserved bytes remains open (**O-2**).
2. **A gap is still not labelled by cause.** A seq jump can be produced by a dropped chunk, by an ISR collision or by a stale chunk header read off a previously-used card (`test/SYNTH_STALE.VIB` jumps 9 → 1326 and is now reported as 1316 dropped chunks = 1684 s). The format has no per-record invalid flag, so the parser reports the arithmetic, not the cause (**O-2 / W-10 stay open**; audit G4 "no stale-data guard" is unchanged).
3. **`HAL_SD_Init()` still has no internal watchdog feed** (§7 item 3, audit U-3 / W-5). Lossy Mode calls it once per dropped chunk, so a present-but-electrically-dead card can still outlive `T_IWDG = 4.095 s` and reset the board. Data written before the reset stays valid (chunk-granular `f_sync`); only the trailer is missing. Measuring `HAL_SD_Init` on a wedged card is still open.
4. **A GC pause longer than ~2 s is now accepted instead of survived.** One 2000 ms attempt per chunk (`VIBL_WRITE_RETRY_MAX = 1u`, `sd_writer.c:110`); a card that needs 3 s per chunk will now lose ~1 chunk every 1.28 s and keep logging. That is the intended trade, but it changes the soak criterion from "no gaps" to "gap rate ≤ X" (see §10.6 item 4).
5. **Stall distribution is still unmeasured.** No `DWT_CYCCNT` instrumentation around `vibl_write_chunk()` exists (`O-6` / §7 of the audit); the P99 < 200 ms target is still open, and the gap rate measured on the bench will be the first real number for it.
6. **Map cost (measured, not estimated):** release `-O2` = RAM **27 464 B (21.0 %)** / Flash **27 044 B (2.6 %)**; debug `-O0` = RAM **27 476 B** / Flash **36 944 B**, zero compiler warnings in both. The 5 new globals account for 20 B of .data/.bss (+28 B release / +36 B debug after alignment). UNCERTAIN: the Milestone-2 reference figures (27 436 / 25 976 release) were measured before FIX-B2 / FIX-E / FIX-F, which are also uncommitted in this tree, so the remaining flash delta is **cumulative**, not attributable to Lossy Mode alone. Measured within this patch: the `sd_writer.c` half produced 36 616 B at `-O0`, the `main.c` half 36 944 B (+328 B).

### 10.6 Hardware acceptance for Milestone 2b (NOT executed — rule 18)

`pio run -e release -t upload` is **not** run by this milestone. Procedure for the board:

1. **Force a stall, prove the session survives.** In a temporary test build add `HAL_Delay(2500);` at the top of `disk_write` (`src/diskio.c:104`, before the `HAL_SD_WriteBlocks` call at `:114`). Expected on a 30 s run: LED flashes at every stall (one per dropped chunk, `SD_GAP_BLINK_MS = 30u`), the board **does not** park in blink code 5, `sd_gap_events > 0`, `sd_fatal_events == 0`, `vibl_dbg_recover_count == sd_gap_events`, and the run keeps producing chunks.
2. **Parse it.** `python parser_vibl.py LOG0003.VIB` must print `Gaps Found: Yes`, one `⚠️ Gap detected!` line per event, a `Gap summary` line, `File CRC32: … -> ✅ MATCH`, `File status : COMPLETE …` and `Exit status : 0`. The `MATCH` line is the proof of the §10.2 item 4 CRC rollback — it would be a mismatch without it.
3. **Prove the fallback still works.** During the same forced-stall build, pull the card. Expected: the CD gate in `sd_error_recover()` (`main.c:235`) returns non-zero, `sd_fatal_events++`, `sd_error_shutdown()` runs (§4.4 unchanged), the blink code is 5, and the file still parses as `✅ MATCH` (bus recovered at close) or `⚠️ No trailer found (power loss/error)` (not recovered) — never as a CRC mismatch.
4. **Re-issue the soak (M9 / U-2, criterion changed).** 24 h at 2.667 MHz with the trailer intact. Pass = `Gaps Found: Yes` **allowed** with a recorded gap rate, `Exit status : 0`, trailer `✅ MATCH`, and no watchdog reset (`vibl_get_chunk_count()` continuous, no fresh `vibl_open()`). Record the gap rate as the first measured GC-stall frequency; add the DWT_CYCCNT histogram (item 5) to turn it into a P99 figure.
5. Keep steps 1 and 4 of §8 (sensor-fault injection, hot-removal) unchanged.

### 10.7 Verification actually run for this patch (2026-11-16, no board)

| Check | Command | Measured result |
|---|---|---|
| Build, debug profile | `pio run -e debug` | **SUCCESS**, RAM 27 476 B (21.0 %) / Flash 36 944 B (3.5 %), `warning:`/`error:` matches = **0** |
| Build, release profile | `pio run -e release` | **SUCCESS**, RAM 27 464 B (21.0 %) / Flash 27 044 B (2.6 %), warnings = **0** |
| Link / wiring | `arm-none-eabi-nm -u .pio\build\release\firmware.elf` | **0 undefined symbols**; `vibl_recover` = `T` (exported, `0x080064c0`), `vibl_rewind_to_data_end` = `t` (static), `sd_gap_events` = `b`, `vibl_chunk_crc_start` = `d` |
| ISR gate (unchanged) | `Select-String src\main.c -Pattern 'fill_cnt \+= 3u'` | **1** → `src\main.c:575` |
| Stall / abort wiring | `Select-String src\sd_writer.c -Pattern 'vibl_stall\(\);'` / `'vibl_abort\(\);'` | **4** (3 in `vibl_write_chunk` + **1 new** in the rewind-failure guard `:566`) / **7** (unchanged, all in `vibl_close`) |
| Shutdown wiring | `Select-String src\main.c -Pattern 'sd_error_shutdown'` | **6** — definition `:166`, comments `:61`, `:157`, `:213`, `:428`, fallback call `:465` (was 3 at Milestone 2) |
| Timeout wiring | `Select-String src\diskio.c -Pattern '2000U'` | 5 hits = **2 live calls** (`diskio.c:85` read, `:114` write) + 3 comment mentions (`:60`, `:62`, `:102`) |
| Retry policy | `Select-String src\sd_writer.c -Pattern '#define VIBL_WRITE_RETRY_MAX'` | **1** → `sd_writer.c:110`, value `1u` (unchanged — Lossy Mode does not add retries, it changes what happens *after* the single failure) |
| New fixtures generated | `python test\mk_synth_lossy_gap.py` | `SYNTH_LOSSY_GAP.VIB` 1 180 496 B / 96 chunks, `SYNTH_LOSSY_HEADGAP.VIB` 98 624 B / 8 chunks, `SYNTH_LOSSY_GAP_NOTRAILER.VIB` 110 902 B / 9 chunks (expected CRCs `A52BC412`, `EABBBB4F`), `SYNTH_LOSSY_OVERWRITE.VIB` 442 856 B / 36 chunks (expected CRC `15D16328`) |
| Byte-level proof of the rewind/overwrite invariant | same script, fixture 4 (`test\mk_synth_lossy_gap.py:137-189`) | The card image is built the way the firmware now writes it: 4 stalled chunks each leave **7 006 B** of a 12 294 B chunk at `vibl_data_end`, `vibl_recover()` rewinds, the next chunk overwrites them, `vibl_close()` truncates. `assert card == clean_gap_chain` → **PASS**: the file is byte-identical to a chain that never contained the dead bytes, exact size, no orphan bytes mid-stream (§10.2 items 2 and 5) |
| Parser, all 11 fixtures | `python parser_vibl.py test\SYNTH*.VIB` | **exit 0 for all 11** (`SYNTH01`, `SYNTH_CORRUPT_TRAILER`, `SYNTH_FFTAIL`, `SYNTH_LOSSY_GAP`, `SYNTH_LOSSY_GAP_NOTRAILER`, `SYNTH_LOSSY_HEADGAP`, `SYNTH_LOSSY_OVERWRITE`, `SYNTH_POWERLOSS`, `SYNTH_SDERROR_CLOSED`, `SYNTH_STALE`, `SYNTH_TRUNC_PARTIAL`) — no traceback, no non-zero code, gaps included |
| Lossy fixture verdicts | same | `SYNTH_LOSSY_GAP` → 96 chunks, `Gaps Found: Yes`, 2 events / 3 dropped chunks (29→31, 54→57), `File CRC32: A52BC412 -> ✅ MATCH`; `SYNTH_LOSSY_HEADGAP` → `Gap detected! Session head dropped: first written seq = 1`, `✅ MATCH EABBBB4F`; `SYNTH_LOSSY_OVERWRITE` → 36 chunks, 3 events / 4 dropped chunks (4→6, 11→14, 19→21), `Declared records: 73728 | parsed: 73728`, `✅ MATCH 15D16328`; `SYNTH_LOSSY_GAP_NOTRAILER` → gaps reported **and** `⚠️ No trailer found (power loss/error)` with **no** `File CRC32` line |
| Pre-existing fixtures unchanged | same | `SYNTH01` `✅ MATCH 64B68650` (it already contains a modelled gap 2→10, now also quantified: 7 chunks / 14 336 samples / 8.960 s), `SYNTH_SDERROR_CLOSED` `✅ MATCH DB132020`, `SYNTH_CORRUPT_TRAILER` → warning + `DATA USABLE`, `SYNTH_FFTAIL` / `SYNTH_POWERLOSS` / `SYNTH_STALE` / `SYNTH_TRUNC_PARTIAL` → no `File CRC32` line at all |
| Parser failure path | `python parser_vibl.py test\DOES_NOT_EXIST.VIB` | `❌ Parsing error: [Errno 2] No such file or directory: …` + `Exit status : 2 (FATAL …)`, **exit code 2**, no traceback |
| Plot deps on this PC | `python -c "import numpy; import matplotlib"` | present → the `RC_NO_PLOT = 3` branch is exercised only by the code path, not by this run |

**Gate drift caused by this patch** (the old numbers stay in §5 for auditability): `vibl_stall();` 3 → **4**, `sd_error_shutdown` references in `main.c` 3 → **6**, and every `main.c:NNN` / `sd_writer.c:NNN` citation in §2-§5 of this file drifts by **+120 / +90** in `main.c` and **+83** in `sd_writer.c`. `.clinerules/03-workflow.md` carries the refreshed gate list.

**Not covered by Milestone 2b (unchanged):** 4-bit SDIO (`src/sdio.c` still `SDIO_BUS_WIDE_1B`, `ClockDiv = 16` → 2.6667 MHz), per-failure gap labels (O-2), the `HAL_SD_Init` watchdog measurement (U-3), DWT_CYCCNT stall histogram (O-6), RTC-set policy (O-9), fixed filename `LOG0003.VIB` (W-7), CubeMX regeneration check (W-3/O-7), hot-removal mid-session (M4, now unblocked in software but still unproven on hardware).

