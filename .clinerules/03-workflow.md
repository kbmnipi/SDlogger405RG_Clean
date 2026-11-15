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
- Synthetic-format check (no board needed): `python parser_vibl.py test\SYNTH01.VIB`, `test\SYNTH_POWERLOSS.VIB`, `test\SYNTH_STALE.VIB`; generators are `test\mk_synth_vibl.py` and `test\mk_synth_powerloss.py`.
- Link/wiring check: `arm-none-eabi-nm -u .pio\build\<env>\firmware.elf`, `arm-none-eabi-readelf -S .pio\build\<env>\firmware.elf`.
- Grep-style acceptance gates used by previous patch rounds, e.g. `grep -n "fill_cnt += 3u" src\main.c` must return exactly one line (that gate is currently NOT satisfied — see `doc\final_pre_flash_audit.md` BLOCKER-1).
