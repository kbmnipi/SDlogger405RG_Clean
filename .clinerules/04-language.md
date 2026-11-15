# 04 — Language rule

Source of the VERBATIM block below: `.continue/rules/architectror.md`, rule 13 (lines 39-42). Original file NOT modified.

## VERBATIM — rule 13. LANGUAGE RULE (prompts for local model)

13. LANGUAGE RULE (prompts for local model)
When generating prompts for the local model (Continue / Strata / LM Studio), ALWAYS write them in English (the model's native language). This maximizes reasoning quality and reduces token waste on translation artifacts.
User-facing explanations (chat replies to the human) remain in Russian — match the user's language.
Exception: code identifiers, register names, and file paths stay as-is (English by nature).

---

## Migration note (added, not part of the verbatim text)

"Continue / Strata / LM Studio" names are preserved verbatim on purpose — they describe where the prompt is being sent. For this repo the rule still reads as: any prompt or task text handed to a model/tool step is written in English (search queries, use_case fields, commit-style notes, code comments), while every reply shown to the user stays in Russian. Code identifiers, register names (`CTRL_REG4`, `SDIO_BUS_WIDE_1B`) and paths (`doc/WEACT_F405_PINOUT.md`, `src/diskio.c`) are never translated.

Encoding note for this repo: `doc/audit_13_11_26.md` and `doc/ANSWER_TO_AUDIT.md` are cp1251-encoded and render as mojibara in UTF-8 readers. Always re-check the encoding before quoting them (`.clinerules/02-anti-hallucination.md` requires EXACT citations).
