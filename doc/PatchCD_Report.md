# IMPLEMENT REPORT — CD (PA8) polarity inversion

**Scope**: 3 files, `single_find_and_replace` only. No flashing, no hardware tests. Static verification only.

## Change 1 — `src/main.c` (CD-gate inverted)

**File:line** — gate `if` now at **`src/main.c:130`**; whole block lives inside `USER CODE BEGIN PreSDIO` (L120) … `USER CODE END PreSDIO` (L133). Markers preserved, `MX_SDIO_SD_Init()` placement unchanged.

```c
// src/main.c:120-133  (USER CODE BEGIN/END PreSDIO preserved)
if (HAL_GPIO_ReadPin(SD_DETECT_GPIO_Port, SD_DETECT_Pin) == GPIO_PIN_RESET) {
  hil3_error_loop(2);  /* PA8 LOW = card NOT inserted (NC switch closes to GND) */
}
```

The stale P0-4 comment ("PA8 is Active LOW", citing pinout §1) was rewritten to state the bench result (inserted 2.5 V / absent 0.4 V) and to note the SSOT correction. I kept the gate position, the T_IWDG = 4.095 s justification and the `hil3_error_loop(2)` call byte-identical — only the condition flipped.

**Grep evidence** (`Select-String src/main.c`):
- `GPIO_PIN_SET` → **211** (`Led_Pin` clean stop), **349** (`GPIO_PIN_2` blink ON). No CD occurrence. ✅
- `GPIO_PIN_RESET` → **130** (new CD-gate), **352** (`GPIO_PIN_2` blink OFF). ✅
- `SD_DETECT` appears **only** at L130 in `main.c`. ✅

## Change 2 — `src/diskio.c` (`disk_status()` inverted)

**File:line** — `disk_status()` body at **`src/diskio.c:26-27`** (function starts L18).

```c
// src/diskio.c:26-27
if ((GPIOA->IDR & GPIO_PIN_8) == 0U) return 0;   /* LOW  = card present */
return STA_NOINIT;                                 /* HIGH = card absent */
```

`disk_initialize()` untouched (`HAL_SD_Init` only, 1-bit bus preserved).

**Grep evidence** (`STA_NOINIT|GPIO_PIN_8`):
- **26** — `== 0U` → return `0` (ready).
- **27** — `STA_NOINIT` returned on HIGH.
- **38** — `STA_NOINIT` in `disk_initialize()` on `HAL_SD_Init` failure (unrelated, pre-existing). ✅

## Change 3 — `doc/WEACT_F405_PINOUT.md` (SSOT updated)

**File:line** — §1 table row at **`doc/WEACT_F405_PINOUT.md:15`**; "ВАЖНЫЕ ЗАМЕТКИ" bullet at **`:62`**.

- L15 → NC-type wording: R11 = 100 kΩ pull-up to VDD33, R12 = 10 kΩ series with PA8, **Карта вставлена → PA8 HIGH**, Active HIGH = inserted.
- L62 → "активен ВЫСОКИМ уровнем (карта вставлена → 1, вытащили → 0) — NC-тип переключателя".

**Grep evidence** — `NC-тип` at **62**, `Active HIGH` at **15**, `ВЫСОКИМ уровнем` at **62** and **63** (PB2 bullet, untouched). Verified clean UTF-8 by direct byte/line read (`python`): the earlier PowerShell mojibake was a console codepage artifact, **not** file corruption. Bullets 1 (PC13), 3 (PB2), 4 (LSE), 5 (HIL/production conflict) left byte-identical.

## Build status

`pio run -t clean && pio run` → **SUCCESS**, 3.55 s.
- RAM: 27 408 / 131 072 = **20.9 %** · Flash: 20 340 / 1 048 576 = **1.9 %**
- Warnings: **exactly one** — `src/ff.c:5032:2: [-Wmisleading-indentation]` (pre-existing, explicitly allowed).
- **0 new warnings** in `main.c`, `diskio.c`, `sd_writer.c`, `lis3dsh.c`. Code size delta is 1 `if` condition inversion → no measurable RAM/Flash change.

## Deviations from spec

1. **Path**: spec said `docs/WEACT_F405_PINOUT.md`; the actual SSOT is **`doc/WEACT_F405_PINOUT.md`** (single `doc/` dir, confirmed by `file_glob_search`). I edited the existing file. All in-code comments already cite `doc/WEACT_F405_PINOUT.md`.
2. **Comment text in Change 1/2** expanded slightly beyond the spec snippet (kept the P0-4/IWDG justification, added the bench numbers). Rationale: deleting the P0-4 audit citation would orphan the gate's purpose; the spec's intent (comment must match the new logic) is satisfied.
3. **Change 2 form**: spec snippet used two lines with comments inline; I implemented as the file's existing single-expression style. Logic is identical to the spec.
4. **Stale audit docs not rewritten** (spec listed only 3 files): `doc/audit_13_11_26_1.md` (P0-4 fix text: `== GPIO_PIN_SET`, "PA8 active LOW — pinout §1") and `doc/final_pre_flash_audit.md` **G3** ("PA8 Active LOW → `GPIO_PIN_SET` = card absent") now contradict the updated SSOT. Per Rule 15 I flag them rather than silently merge.

## Open items (NOT closed)

- **WARN-1 (hardware config, untouched by design)**: `src/gpio.c` configures PA8 as `GPIO_MODE_INPUT` + `GPIO_PULLUP`; `SDlogger405RG_Clean.ioc` has `PA8.GPIO_PuPd=GPIO_PULLUP`. With the NC switch this is **harmless** (external R11 100 k and internal ~40 k pull-up reinforce HIGH; when the switch closes, internal pull-up sinks ≈ 3.3 V / 40 kΩ ≈ **82 µA**, switch short is the dominant path → LOW, measured 0.4 V). No change required, **but the `.ioc`/`gpio.c` pull-up is redundant** — `GPIO_NOPULL` would be cleaner. Flagged as TBD, not edited (CubeMX file, not in the spec's allowed line list).
- **WARN-2 (UNCERTAIN)**: `disk_initialize()` calls blocking `HAL_SD_Init(&hsd)` with the HAL default SDMMC timeout. The CD-gate protects boot, and `disk_status()` protects post-mount access, but a card removal **during** a `f_write` chunk is still governed by `HAL_SD_Read/WriteBlocks` 200 ms timeouts (`diskio.c:56,71`) + IWDG 4.095 s. Not in scope of this patch; behavior on hot-removal not verified on hardware.
- **WARN-3 (stale docs)**: `final_pre_flash_audit.md` G3 acceptance text needs a re-check against the new polarity before the next audit round.
- **UNCERTAIN**: PA8 0.4 V in the "absent" state is 0.4 V against a `GPIO_PULLUP` input — above the F405 V_IL spec limit (0.3·VDD = 0.99 V is the limit, so 0.4 V is comfortably LOW). Reading is safe; no schmitt-trigger change needed. Cited from RM009 general limits, not from files in this project.

## Hardware steps remaining (user actions)

1. **Flash** (`pio run -t upload`) — **not performed**, per Rule 18.
2. **Card inserted** → expect **no** 2-blink error, normal boot, LED behavior per logging session.
3. **Card absent** → expect **2 blinks** (`hil3_error_loop(2)`) ≥ 5 s, **no** IWDG reset loop.
4. Re-measure PA8 with the multimeter after flash to confirm the reading matches the gate's decision.
5. Optional: re-run `final_pre_flash_audit.md` G3 with corrected polarity text.
~~~