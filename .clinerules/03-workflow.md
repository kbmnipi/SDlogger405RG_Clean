# 03 — Workflow (hardware authorization, verify-after-patch, concise reasoning)

Source of the VERBATIM blocks below: `.continue/rules/architectror.md`, rule 18 (lines 63-69), rule 19 (lines 70-74), rule 20 (lines 75-78). Original file NOT modified.

## VERBATIM — rule 18. HARDWARE ACTIONS REQUIRE EXPLICIT AUTHORIZATION (enforced for IMPLEMENT mode)

18. HARDWARE ACTIONS REQUIRE EXPLICIT AUTHORIZATION (enforced for IMPLEMENT mode)
Do NOT run any of the following without explicit user instruction:
  - `pio run -t upload` / `pio run -t program` (flashing firmware)
  - Any command that writes to the target board's flash
  - Any command that modifies files outside the project tree
These actions can brick hardware, erase user data, or conflict with other tools.
If the acceptance criteria mention "flash the board", ASK the user whether to attempt it now, or leave it for manual execution.

## VERBATIM — rule 19. VERIFY-AFTER-EACH-PATCH (enforced for IMPLEMENT mode)

19. VERIFY-AFTER-EACH-PATCH (enforced for IMPLEMENT mode)
After applying each patch from the spec, IMMEDIATELY run the relevant acceptance check (build, unit test, synthetic file, grep verification).
Do NOT batch all patches and verify at the end.
Rationale: early verification catches integration issues before they compound. A 5-minute verify after each patch saves hours of debugging later.
Exception: if the patch is trivially correct (e.g., a comment change) and the acceptance criterion is at the end (e.g., "run on real hardware"), batching is acceptable.

## VERBATIM — rule 20. CONCISE REASONING (enforced for all modes)

20. CONCISE REASONING (enforced for all modes)
Keep internal reasoning focused on the NEXT concrete action.
If your reasoning exceeds 300 tokens without producing a tool call, you are overthinking — emit the best current conclusion and act.
Do not enumerate all possible approaches before picking one; pick the most likely correct one and verify.

---

## Acceptance checks available in this tree (added by migration, not part of the verbatim text)

- Build: `pio run -e release` / `pio run -e debug` (only one `default_envs` line is uncommented in `platformio.ini`; `-Os` must never survive — check with `pio run -e release -v`).
- Synthetic-format check (no board needed): `python parser_vibl.py test\SYNTH01.VIB`, `test\SYNTH_POWERLOSS.VIB`, `test\SYNTH_STALE.VIB` (generators `test\mk_synth_vibl.py`, `test\mk_synth_powerloss.py`) and, since the Milestone-2 parser fix, `test\SYNTH_FFTAIL.VIB`, `test\SYNTH_TRUNC_PARTIAL.VIB`, `test\SYNTH_SDERROR_CLOSED.VIB` (generator `test\mk_synth_error_tail.py`). Expected: the two no-trailer fixtures print `⚠️ No trailer found (power loss/error)` and NO `File CRC32` line at all; `SYNTH01.VIB` and `SYNTH_SDERROR_CLOSED.VIB` print `File CRC32: … ✅ MATCH`. **Updated 2026-11-15 (Milestone 3 rollback):** `test\SYNTH_CORRUPT_TRAILER.VIB` (also `test\mk_synth_error_tail.py`, fixture 4 = the 12 MHz bench artefact: 102 chunks + 1 corrupted bit in the trailer CRC) must print 102 chunks, `Gaps Found: No`, `⚠️ Trailer CRC mismatch (likely corrupted during power loss / SD error)` and `File status : DATA USABLE …`; the parser NEVER prints `❌ MISMATCH` any more - a trailer CRC mismatch is a warning about the 16 B trailer block only. **Updated 2026-11-16 (V2 LOSSY MODE, TASK 3):** add `test\SYNTH_LOSSY_GAP.VIB`, `test\SYNTH_LOSSY_HEADGAP.VIB`, `test\SYNTH_LOSSY_GAP_NOTRAILER.VIB` (generator `test\mk_synth_lossy_gap.py`) — 96 chunks / `Gaps Found: Yes` / `✅ MATCH A52BC412`, head gap `first written seq = 1` / `✅ MATCH EABBBB4F`, and a gapped no-trailer file. **Exit-code contract** (`parser_vibl.py:22-33`, `:347-348`): `0` = data usable, gaps INCLUDED (a gap is an expected operational state, it never changes the code) · `2` = file cannot be interpreted (bad magic / too short / missing file / no chunks and no trailer) · `3` = `--plot` requested but numpy/matplotlib unavailable. Measured 2026-11-16: **all 11 SYNTH fixtures exit 0**, `python parser_vibl.py test\DOES_NOT_EXIST.VIB` exits **2** with no traceback. The generator also runs a byte-level assert (`test\mk_synth_lossy_gap.py:137-189`): 4 stalled chunks that each left 7 006 B at `vibl_data_end` are fully overwritten by the following chunks and the resulting file is byte-identical to a clean gap chain (`SYNTH_LOSSY_OVERWRITE.VIB`, `✅ MATCH 15D16328`).
- Link/wiring check: `arm-none-eabi-nm -u .pio\build\<env>\firmware.elf`, `arm-none-eabi-readelf -S .pio\build\<env>\firmware.elf`.
- Grep-style acceptance gates. `grep -n "fill_cnt += 3u" src\main.c` must return exactly one line — **gate SATISFIED since 2026-11-15**: one occurrence, now `src\main.c:575` (renumbered again 2026-11-16 by the Lossy Mode patch: the **count** gate is unchanged), inside the shared advance/wrap block that BOTH the SPI-success and the SPI-error path now fall through. The earlier failing status (gate unmatched, BLOCKER-1 open) is retired; BLOCKER-1 is CLOSED — see `doc\MILESTONE_2_ROBUSTNESS.md` and the annotated `doc\final_pre_flash_audit.md` (addendum 2026-11-15). Companion gates from the same patch round: `vibl_abort();` in `src\sd_writer.c` = 7 (all inside `vibl_close`).
- **Gates re-measured 2026-11-16 after the V2 LOSSY MODE patch (V2-1 / V2-1a-d):** `vibl_stall();` in `src\sd_writer.c` = **4** (3 inside `vibl_write_chunk` + **1 new** in the rewind-failure guard at `sd_writer.c:566`) — the old expectation "3" is superseded; `sd_error_shutdown` in `src\main.c` = **6** (definition `:166`, comments `:61`, `:157`, `:213`, `:428`, fallback call `:465`) — the old expectation "3" is superseded, because it is no longer the primary error path; `vibl_recover` in `src\main.c` = **3** (comments `:136`, `:247`, call `:250`); `sd_error_recover` = 4 (`:109`, `:136`-shared line, def `:233`, call `:463`), `sd_gap_log` = 2 (def `:262`, call `:462`), `lost_chunks` = 7 (`:431`, `:435`, `:445`, `:447`, `:459`, `:460`, `:461`). `arm-none-eabi-nm -u .pio\build\release\firmware.elf` = **0 undefined symbols**. Build after the patch: `pio run -e release` SUCCESS RAM 27 464 B / Flash 27 044 B, `pio run -e debug` SUCCESS RAM 27 476 B / Flash 36 944 B, 0 warnings in both.
- Timeout / watchdog gates added 2026-11-16 (FIX-B2 / FIX-E / FIX-F): `findstr /n "2000U" src\diskio.c` must hit exactly two live calls — `src\diskio.c:85` (`HAL_SD_ReadBlocks`) and `src\diskio.c:114` (`HAL_SD_WriteBlocks`); the FAILURE-path feeds are `src\diskio.c:87` and `:116`, the success-path feeds `:91` and `:162`; `#define VIBL_WRITE_RETRY_MAX 1u` sits at `src\sd_writer.c:99`. Arithmetic gate: 1 × 2000 ms (one failed attempt) + ≤ 500 ms of `sd_error_shutdown` delays < `T_IWDG = 4095 ms` (`src\iwdg.c:45-46`). Line citations inside `doc\MILESTONE_2_ROBUSTNESS.md` and `doc\final_pre_flash_audit.md` (main.c:129/317/332/438, diskio.c:56/71/88/124, sd_writer.c:88/155-158) are stale by 1–5 lines after this patch round; those docs are annotation-only and need a re-issue.
