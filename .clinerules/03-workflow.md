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
- Synthetic-format check (no board needed): `python parser_vibl.py test\SYNTH01.VIB`, `test\SYNTH_POWERLOSS.VIB`, `test\SYNTH_STALE.VIB` (generators `test\mk_synth_vibl.py`, `test\mk_synth_powerloss.py`) and, since the Milestone-2 parser fix, `test\SYNTH_FFTAIL.VIB`, `test\SYNTH_TRUNC_PARTIAL.VIB`, `test\SYNTH_SDERROR_CLOSED.VIB` (generator `test\mk_synth_error_tail.py`). Expected: the two no-trailer fixtures print `⚠️ No trailer found (power loss/error)` and NO `File CRC32` line at all; `SYNTH01.VIB` and `SYNTH_SDERROR_CLOSED.VIB` print `File CRC32: … ✅ MATCH`.
- Link/wiring check: `arm-none-eabi-nm -u .pio\build\<env>\firmware.elf`, `arm-none-eabi-readelf -S .pio\build\<env>\firmware.elf`.
- Grep-style acceptance gates. `grep -n "fill_cnt += 3u" src\main.c` must return exactly one line — **gate SATISFIED since 2026-11-15**: one occurrence, `src\main.c:438`, inside the shared advance/wrap block that BOTH the SPI-success and the SPI-error path now fall through. The earlier failing status (gate unmatched, BLOCKER-1 open) is retired; BLOCKER-1 is CLOSED — see `doc\MILESTONE_2_ROBUSTNESS.md` and the annotated `doc\final_pre_flash_audit.md` (addendum 2026-11-15). Companion gates from the same patch round: `vibl_stall();` in `src\sd_writer.c` = 3 (all inside `vibl_write_chunk`), `vibl_abort();` = 7 (all inside `vibl_close`), `sd_error_shutdown` in `src\main.c` = 3 (definition `:129`, comment `:317`, call `:332`).
