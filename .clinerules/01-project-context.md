# 01 — Project context (hardware, pins, stack)

Source of the VERBATIM block below: `.continue/rules/architectror.md`, lines 1-4 and 10-15 (rule numbering as in the original). The original file is NOT modified by this migration.

## VERBATIM (architectror.md, lines 1-4)

description: Architecture rules for SD Logger V2 (WeAct F405RG + SDIO + HIL)
You are an Expert Embedded Systems Architect with 20 years of experience in industrial IoT, DSP, and real-time operating systems.
ALWAYS read files in `doc/` first to understand project history, previous audits, and hardware constraints. Specifically:
`doc/WEACT_F405_PINOUT.md` — SINGLE SOURCE OF TRUTH for pin mapping. Never guess or invent pin assignments. Always cite this file.

## VERBATIM (architectror.md, lines 10-15)

TARGET BOARD: WeAct STM32F405RG (168MHz, 192KB SRAM + 64KB CCM, 1MB Flash).
STORAGE: MicroSD via hardware 4-bit SDIO (NOT SPI). Currently using blocking mode in HAL.
NO SENSOR YET: LIS3DSH accelerometer is not available. HIL test uses DAC (PA5) -> ADC (PA0, PA1, PA2) loopback with TIM2 @1600Hz.
LIBRARY: FatFs (R0.12c) for file system. SD card detect on PA8 (Active LOW).
PIN MAP: All pin assignments MUST come from `doc/WEACT_F405_PINOUT.md`. If a pin is not listed there, do not assume — flag it as "TBD in pinout".
Every claim about timing, memory, or hardware MUST be justified with numbers (ns/µs/ms, KB, cycles).

---

## Repo-state corrections — NOT copied from architectror.md

These four lines above are stale with respect to the current tree. Per rule 15 (FILE AUTHORITY) they are FLAGGED here instead of being silently rewritten, so the verbatim copy above stays auditable:

- NOTE: `doc/WEACT_F405_PINOUT.md` §1 supersedes "SD card detect on PA8 (Active LOW)" — the switch is NC-type (R11 = 100 k pull-up to VDD33, R12 = 10 k in series), bench result inserted = 2.5 V / absent = 0.4 V, so **PA8 Active HIGH = card inserted**. Implemented that way in `src/diskio.c:26` (`disk_status()`) and `src/main.c:130` (CD-gate). See `doc/Patch_f_write.md` (FIX-A) and `doc/PatchCD_Report.md`.
- NOTE: `src/sdio.c:45` runs `SDIO_BUS_WIDE_1B`, and `src/sdio.c` + `SDlogger405RG_Clean.ioc` now set `ClockDiv = 2` (SDMMC_CK = 48/(2+2) = **12 MHz**, 1-bit, ceiling 1.5 MB/s vs the 9 600 B/s needed -> 156x margin; HAL transfer limit is 25 MHz, `stm32f4xx_ll_sdmmc.h:726-727`). Since 2026-11-15 (Milestone 3); the Milestone-1 value was `ClockDiv = 16` (2.667 MHz). `src/diskio.c` explicitly keeps the 4-bit call removed, so the storage bus is **1-bit, not 4-bit**; 4-bit stays a deferred option (`doc/final_pre_flash_audit.md` W-4 / O-6).
- NOTE: the sensor IS available and integrated — `src/lis3dsh.c` / `include/lis3dsh.h` (SPI1, burst read `0xA8`, CTRL_REG4 = 0x9F → ODR 1600 Hz), first end-to-end artifact `LOG0003.VIB` (see `doc/MILESTONE_1_SUCCESS.md`). "NO SENSOR YET" and the DAC(PA5)→ADC(PA0/1/2) HIL loopback describe an earlier HIL milestone; there is no `adc.c` / `dac.c` in this tree.
- NOTE on "Currently using blocking mode in HAL": still true — `HAL_SD_ReadBlocks` / `HAL_SD_WriteBlocks` are blocking with a 1000 ms bound (`src/diskio.c:67`, `:88`) against `T_IWDG = 4.095 s` (`src/iwdg.c`), and the `HAL_Delay(10)` in `disk_write` is intentional (FIX-D, documented in `src/diskio.c:92-110`).
