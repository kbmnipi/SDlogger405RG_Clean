# 02 — Anti-hallucination & file authority

Source of the VERBATIM blocks below: `.continue/rules/architectror.md`, rule 12 (lines 31-38) and rule 15 (lines 47-52). Original file NOT modified.

## VERBATIM — rule 12. ANTI-HALLUCINATION (strictly enforced for audit & implement)

12. ANTI-HALLUCINATION (strictly enforced for audit & implement)
If you claim a bug exists, cite the EXACT line from the provided file.
If you're not sure, write "UNCERTAIN: <reason>".
Never invent: HAL functions, FatFs API, register names, pin numbers. If you don't see it in the provided files → say "not in provided files".
For pin claims, cite `doc/WEACT_F405_PINOUT.md` verbatim.
Historical hallucination to avoid: `RTC_DateTimeTypeDef` does NOT exist in STM32F4 HAL (time and date are separate structs: `RTC_TimeTypeDef` and `RTC_DateTypeDef`).
If you need to check API behavior, DO NOT rely on your memory. Use the `grep_search` or `read_file` tools to search the provided `ff.c` file. For example, to check FA_WRITE, search `ff.c` for "FA_WRITE".
IMPORTANT: If you catch yourself repeating the same thought, STOP. Output the best conclusion you have and move to the next item. Do not loop.

## VERBATIM — rule 15. FILE AUTHORITY (freshness rule)

15. FILE AUTHORITY (freshness rule)
If the user uploads a new file that contradicts an older doc in the knowledge base, treat the NEWER file as authoritative for the current session.
Explicitly flag the contradiction in the output, e.g.:
"NOTE: <new_file> §X supersedes <old_file> §Y (value A → value B)."
If two uploaded files conflict with each other and no timestamp/order is clear, STOP and ask the user which one is authoritative before proceeding. Do not silently pick one.
Never silently merge contradictory claims — this is a primary source of architectural drift.

---

## Tool-name mapping (added by migration, not part of the verbatim text)

The original names Continue's tools. Cline equivalents used in this project:

| In `architectror.md` | In Cline |
| :-- | :-- |
| `grep_search` | `search_codebase` |
| `read_file` | `read_files` (use `start_line`/`end_line` instead of `sed -n 'X,Yp'`) |
| `run_terminal` | `run_commands` |
| `create_new_file` | `editor` (new path) |
| `single_find_and_replace` | `editor` (`old_text` → `new_text`) |
