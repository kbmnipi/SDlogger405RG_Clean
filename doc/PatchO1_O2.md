# O-1 + O-2 — закрыты. Изменены ровно 2 файла: `parser_vibl.py`, `src/rtc.c`

## TASK 1 — O-1: parser больше не печатает «File truncated» на целостном файле

**Реализация** (`parser_vibl.py:50-79`, вариант «проба перед итерацией» — самый pythonic из трёх предложенных, без отдельного чтения трейлера заранее):

```python parser_vibl.py
    def trailer_candidate(p):
        # O-1 fix (doc/MILESTONE_1_SUCCESS.md §5) ...
        if len(data) - p != 16:
            return False
        end_ts, total_records, _ = struct.unpack_from('<QII', data, p)
        return end_ts != 0 and total_records != 0 and (end_ts & 0xFFFFFFFF) != len(chunks)

    while pos + 6 <= len(data):
        # Silence O-1: step out of the loop exactly at the trailer, no warning.
        if trailer_candidate(pos):
            break
        # ... existing code ...
```

Почему именно так, а не «просто `seq != len(chunks)`»:
- `struct.unpack_from` без промежуточного среза, `len(chunks)` — ожидаемый следующий индекс (ровно как в spec задачи).
- **Дополнительный reject `end_ts == 0 or total_records == 0` — обязателен.** Без него power-loss файл, обрезанный `f_truncate()` (`sd_writer.c` vibl_close) ровно на границе чанка, оставил бы 16 нулевых байт → их u32 = 0 ≠ `len(chunks)` → их назвали бы трейлером и напечатали bogus `File CRC32: 00000000 → ❌ MISMATCH` вместо честного `No trailer found (power loss)`. Это ровно тот класс ошибки, который уже закрыт флагом `power_loss_tail`.
- `seq == len(chunks)` по-прежнему трактуется как заголовок чанка → штатное предупреждение о усечении сохраняется (см. T2 ниже).

**Верификация (выполнено сразу после патча):**

| # | Вход | Ожидалось | Факт |
|---|------|-----------|------|
| 1 | `LOG0003.VIB` 246 152 B (реальный, чистая остановка) | без warning, `MATCH` | `✅ Header CRC: OK` → `Chunks: 20`, `Records: 40960`, `Gaps: No`, `File CRC32: 9C383A17 → ✅ MATCH` — **предупреждения нет** |
| 2 | `test/SYNTH01.VIB` (gap-файл) | warning отсутствует, gap виден | `Chunks: 5`, `⚠️ Gap detected! 2 → 10`, `64B68650 → ✅ MATCH` |
| 3 | `test/SYNTH_POWERLOSS.VIB` 44 258 656 B | хвост-защита цела | `⚠️ No trailer found (power loss)`, `Chunks: 10`; время разбора **0.514 с** (< 1 с по O-3(b)) |
| 4 | `test/SYNTH_STALE.VIB` | хвост-защита цела | `No trailer found (power loss)`, `Chunks: 11`, gap `9 → 1326` |
| T1 | `LOG0003` урезан на 3000 B внутрь чанка | warning ОБЯЗАН остаться | `⚠️ File truncated during chunk 19`, `chunks=19` |
| T2 | 16 B слева, `seq == len(chunks)` (заголовок чанка, payload потерян) | warning ОБЯЗАН остаться | `⚠️ File truncated during chunk 20` ✅ |
| T4 | 20 чанков + 16 нулевых байт (power-loss, граница чанка) | НЕ трейлер | `⚠️ No trailer found (power loss)` ✅ |
| T5 | ноль-чанговая сессия (`end_ts = 0, total = 0`) | не «съедать» молча | `⚠️ No trailer found (power loss)` ✅ |

`python -m py_compile parser_vibl.py` → OK. Временный скрипт `_o1_regression.py`, использованный для T1/T2/T4/T5, удалён.

## TASK 2 — O-2: RTC получает календарь при старте

**`src/rtc.c`** — и хелпер, и вызов размещены внутри `/* USER CODE BEGIN 0 */` и `/* USER CODE BEGIN RTC_Init 2 */`, поэтому перегенерация из `SDlogger405RG_Clean.ioc` их не сотрёт. Вне USER CODE блоков ничего не изменено.

```c src/rtc.c
#define RTC_DEFAULT_YEAR   26u   /* 2000 + 26 = 2026 (RTC Year = offset from 2000) */
#define RTC_DEFAULT_MONTH  11u   /* RTC_MONTH_NOVEMBER */
#define RTC_DEFAULT_DATE   15u
#define RTC_DEFAULT_HOUR   12u
static volatile int rtc_stamp_ok = 0;   /* 1 = stamped OK, 0 = Set/Set failed */

static void Set_RTC_Time(void)
{
  RTC_TimeTypeDef sTime;   /* F4 HAL: time and date are SEPARATE structs. */
  RTC_DateTypeDef sDate;   /* RTC_DateTimeTypeDef DOES NOT exist in STM32F4 HAL. */
  ...
  if (HAL_RTC_SetTime(&hrtc, &sTime, RTC_FORMAT_BIN) != HAL_OK) { rtc_stamp_ok = 0; return; }
  if (HAL_RTC_SetDate(&hrtc, &sDate, RTC_FORMAT_BIN) != HAL_OK) { rtc_stamp_ok = 0; return; }
  rtc_stamp_ok = 1;
}
/* MX_RTC_Init(), после if (HAL_RTC_Init(&hrtc) != HAL_OK) Error_Handler(); */
  Set_RTC_Time();
```

Соответствие пунктам задачи: ✅ `static void Set_RTC_Time(void)` внутри `rtc.c`; ✅ 2026 / Nov / 15, 12:00:00; ✅ `HAL_RTC_SetTime(&hrtc, &sTime, RTC_FORMAT_BIN)` + `HAL_RTC_SetDate(...)` сразу после `HAL_RTC_Init`; ✅ только `RTC_TimeTypeDef`/`RTC_DateTypeDef`; ✅ комментарий «TEMPORARY HARDCODE — to be replaced by a host-side time sync (SWD write to RTC_TR/RTC_DR, or a `--set-rtc` UART command) per open item O-9».

Корректность значений — проверено по исходникам HAL, не по памяти:
- `Year = 26` = смещение от 2000 → ровно то, что ожидаю `sd_writer.c:189` (`year = 2000u + sDate.Year`) и `diskio.c:189` (`(d.Year + 20) << 25` == year−1980).
- `RTC_MONTH_NOVEMBER = 0x11`; в BIN-режиме `HAL_RTC_SetDate` сам ребейзит месяц (`stm32f4xx_hal_rtc.c:878`: `if ((Format == BIN) && ((sDate->Month & 0x10U) == 0x10U))`), поэтому `0x11` → BCD `0x11` в `RTC_DR`. `IS_RTC_YEAR(26)`/`IS_RTC_MONTH(11)`/`IS_RTC_DATE(15)` (`stm32f4xx_hal_rtc.h:842-844`) проходят; `IS_RTC_WEEKDAY(RTC_WEEKDAY_SUNDAY)` — 2026-11-15 действительно воскресенье (проверено на хосте).
- `sTime.TimeFormat = 0x00`, `DayLightSaving = RTC_DAYLIGHTSAVING_NONE`, `StoreOperation = RTC_STOREOPERATION_RESET` → `assert_param` в `HAL_RTC_SetTime` (`stm32f4xx_hal_rtc.c:721-736`) проходит, `RTC_CR.BKP/SUB1H/ADD1H` остаются нулевыми. В 24-часовом режиме HAL сам принудительно пишет `TimeFormat = 0x00U`.
- Порядок Time → Date соблюдён (HAL документирует «`HAL_RTC_SetTime()` must be called before enabling the Alarm», и оба входа независимы по init-mode).
- **Access к backup domain уже есть:** `HAL_RTC_MspInit` → `HAL_RCCEx_PeriphCLKConfig` поднимает PWR-часы и `PWR->CR |= PWR_CR_DBP` (`stm32f4xx_hal_rcc_ex.c:184-187`), а MspInit вызывается внутри `HAL_RTC_Init`, т.е. до `Set_RTC_Time()`. Дополнительного `HAL_PWR_EnableBkUpAccess()` не потребовалось.

Осознанное отклонение (не «молчаливое», а зафиксированное в комментарии в коде): при `HAL_ERROR` **не** вызывается `Error_Handler()`. `Error_Handler` — это `__disable_irq(); while(1)` (`main.c:368-377`), а IWDG уже запущен `MX_IWDG_Init()` (`main.c:118`) с `T_IWDG = 4.095 s` → гарантированный бесконечный reset-цикл вместо диагностируемого состояния. Вместо него — флаг `rtc_stamp_ok` (watch window) и существующий guard `main.c:165` → `hil3_error_loop(6)`.

## Сборка (acceptance «build and verify»)

`pio run -e debug` → **SUCCESS**; `pio run -e release -v` → **SUCCESS**, `-O2` в командной строке компиляции `src/rtc.c` подтверждён, `-Os` нет. Единственное предупреждение — предсуществующее `src/ff.c:5032:2: warning: [-Wmisleading-indentation]` (не относится к правке).

| Профиль | RAM | Flash | Δ Flash |
|---|---|---|---|
| debug | 27 444 / 131 072 (20.9%) | 35 628 B | +752 B |
| release | 27 428 / 131 072 (20.9%) | 25 848 B | +1 264 B |

Прирост объяснён и измерен через `nm -S` по `firmware.elf`: в образ попали ранее отсекаемые `--gc-sections` функции `HAL_RTC_SetTime` = **0x26c (620 B)** и `HAL_RTC_SetDate` = **0x23c (572 B)** (итого 1 192 B) + разрастание `MX_RTC_Init`; `rtc_stamp_ok` = **+4 B** `.bss` (27 424 → 27 428 — сходится байт в байт). `RTC_EnterInitMode`/`RTC_ExitInitMode`/`HAL_RTC_WaitForSynchro` уже были в образе из `HAL_RTC_Init`.

Дизасемблирование `MX_RTC_Init` (release) подтверждает фактический вызов: `bl HAL_RTC_Init` → `bl HAL_RTC_SetTime` (r1 = sp+4, формат `0x1a0f1107` = sec 07/12, min 01, hour 12 в BCD) → `bl HAL_RTC_SetDate` → ветки записи `rtc_stamp_ok` (`movs r0,#1` / `movs r4,#0`). Отладочный хелпер корректно инлайнится при `-O2` — отдельного символа `Set_RTC_Time` в `.elf` нет, что ожидаемо для `static` + `-O2`.

Числовая связка O-2 → парсер: `2026-11-15T12:00:00Z` → `start_ts = 1 794 744 000 000` ms (пересчитал алгоритмом `vibl_rtc_ms()` из `sd_writer.c` — совпало с эталоном на хосте) → `format_ts()` печатает `2026-11-15 12:00:00.000 UTC`; `start_ts ≠ 0`, поэтому guard `main.c:165` не срабатывает (год 2000 больше не появится).

## Что осталось открытым (не в объёме этой правки)

1. **Сенinel-проверка не добавлена.** MILESTONE §5 O-2 указывает второй вариант закрытия — гейт `Year == 0 && (Hours|Minutes|Seconds) == 0`. Он **не реализован**: при мёртвом LSE `RTC_EnterInitMode()` не достигнет `INITF` (таймаут `RTC_TIMEOUT_VALUE = 1000 ms`, `stm32f4xx_hal_rtc.h:792`) → `HAL_RTC_SetTime` вернёт `HAL_ERROR`, `TR/DR` останутся в значении сброса, `vibl_rtc_ms()` даст **ненулевой** epoch-2000 → `main.c:165` его не поймает. Это ровно тот остаточный зазор, что назван в O-2; зафиксирован комментарием в `rtc.c`.
2. **O-9 (политика установки RTC) остаётся открытой.** Жёсткий код детерминирован, но при VBAT/суперкапе (pinout §3 `VBAT` → RTC) часы между boot’ами продолжают идти, а патч при каждом power-on **перезаписывает** наработанное время константой. Хост-синхронизация (SWD-запись `RTC_TR`/`RTC_DR` либо `--set-rtc` по UART) — обязательный следующий шаг; альтернатива «устанавливать только при `!__HAL_RTC_IS_CALENDAR_INITIALIZED()`» (`stm32f4xx_hal_rtc.h:497`) не выбрана, т.к. оставляет карту 2000 года после любой backups-domain потери.
3. **Проверка на железе не выполнена** по правилу о запрете `pio run -t upload` без явного разрешения. Acceptance на плате: после `pio run -e release -t upload` новый `LOG000N.VIB` должен показать `start_ts` 2026-11-15T12:00:00Z и `mtime` каталога того же года (сейчас `diskio.c:189` уже использует живой RTC, а не `_FS_NORTC`-заглушку). Скажите — запускать upload или оставляем вручную.

HAL-библиотеки, `platformio.ini`, `.ioc` и другие файлы `src/` не изменялись.
~~~