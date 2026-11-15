# AUDIT + IMPLEMENT REPORT — `f_write` failure in `vibl_open()`

**Scope:** `src/diskio.c`, `src/sd_writer.c`. No flashing performed (Rule 18).
**NOTE (Rule 15):** the SSOT is **`doc/WEACT_F405_PINOUT.md`** (single `doc/` dir, not `docs/`). `doc/audit_13_11_26_1.md` (P0-4 text: «PA8 active LOW», `== GPIO_PIN_SET`) and `doc/final_pre_flash_audit.md` **G2** («SD timeouts 200 ms», hierarchy `200 ms < 4.095 s`) / **G3** («PA8 Active LOW → GPIO_PIN_SET = card absent») **contradict the current SSOT and this session's patches**. They are flagged, not silently merged.

---

## VERDICT — гипотеза ОПРОВЕРГНУТА

Корневая причина — **не таймаут `HAL_SD_WriteBlocks`**, а **инвертированная полярность CD в `disk_status()`**, которая заставляет FatFs R0.12c возвращать `FR_INVALID_OBJECT (9)` из `validate()` вместо `FR_DISK_ERR (1)`.

### Доказательство по коду (не «из памяти», а из `src/ff.c`)

**1. Цепочка, объясняющая ВСЕ три наблюдаемых кода.**

| Шаг | Цитата | Результат |
| :-- | :-- | :-- |
| `f_mount(&fs,"0:",1)` | `ff.c:3036` `stat = disk_initialize(fs->drv);` | `fs->fs_type==0` → идёт путь **`disk_initialize`**, `disk_status()` НЕ вызывается → **`FR_OK`**._mount succeeds despite bad CD._ |
| `f_open(...FA_CREATE_ALWAYS\|FA_WRITE)` | `ff.c:3021–3029`: `if (fs->fs_type) { stat = disk_status(fs->drv); if (!(stat & STA_NOINIT)) … return FR_OK; }` → НЕ попало в `return FR_OK` → `ff.c:3034` `fs->fs_type = 0;` → `ff.c:3036` **повторный `disk_initialize()`** (второй `HAL_SD_Init`) | **`FR_OK` (0)** — отсюда и «файл успешно создан». Побочный эффект: карта ре-инициализируется на **каждом** публичном FatFs-вызове. |
| `vibl_write_all` → `f_write` | `ff.c:3626` `res = validate(&fp->obj, &fs);` → `ff.c:3221` `FRESULT res = FR_INVALID_OBJECT;` → `ff.c:3236` `if (!(disk_status(obj->fs->drv) & STA_NOINIT)) res = FR_OK;` — условие **ложно** | `f_write` возвращает **9**. `vibl_write_all`: `fr != FR_OK` → `VIBL_ERR_WRITE (-4)`. |
| `f_close` | `ff.c:3821` `res = f_sync(fp);` → `ff.c:3745` `validate()` → **9**; `ff.c:3822` `if (res == FR_OK)` ложно → `ff.c:3839 return res;` | **`9 = FR_INVALID_OBJECT`** (enum подтверждён: `include/ff.h:224` `FR_INVALID_OBJECT, /* (9) */`). |

**2. Почему это НЕ таймаут записи.**
`disk_write` → `RES_ERROR` в R0.12c превращается в `ABORT(fs, FR_DISK_ERR)` (`ff.c:3677`, `ff.c:455`) → код **1**, а не 9. Наблюдаем 9 — это подпись `validate()`, то есть **отказ состояния диска**, а не I/O.

**3. Почему 256-байтный заголовок вообще не доходит до `disk_write` данных.**
Свежий файл (`FA_CREATE_ALWAYS`, `objsize==0`), `btw=256 < SS(fs)=512` → `ff.c:3672–3673` `cc = btw/SS(fs) == 0` → ветка прямого мультисекторного write не выполняется; `ff.c:3700–3704` «fill sector cache» пропускается, т.к. `fp->fptr < fp->obj.objsize` ложно; данные ложатся в `fp->buf` + `FA_DIRTY` (`ff.c:3715–3716`). Значит `HAL_SD_WriteBlocks(…, 200U)` в первом `f_write` **не вызывался ни разу** — менять было нечего. Таймаут записки мог сыграть роль только позже, в `f_sync` (window write-back `ff.c:912–933`, FAT/FSINFO `ff.c:964`) и тогда дал бы `VIBL_ERR_SYNC (-5)`, а не `-4`.

**4. Проверка альтернативных причин (по заданию).**
- **Выравнивание буфера** — не причина: `f_write` не требует выравнивания (сам копает `mem_cpy` в `fp->buf`, `ff.c:3715`), а `HAL_SD_WriteBlocks` читает `pData` побайтно в FIFO (`stm32f4xx_hal_sd.c:864–879`). `vibl_hdr` — `static` глобальная структура, естественно выровнена.
- **Отсутствие `CTRL_SYNC`** — не причина: `disk_ioctl(CTRL_SYNC)` (`diskio.c`) возвращает `RES_OK` безусловно; `f_sync` уже вызывается (`sd_writer.c`) и в `vibl_open`, и в `vibl_write_chunk`.
- **Баг многоблочной обработки в `disk_write`** — не обнаружен: `count` передаётся напрямую в `NumberOfBlocks`, `HAL_SD_WriteBlocks` сам выбирает CMD25/CMD24 (`stm32f4xx_hal_sd.c:829–842`) и шлёт CMD12 при `count>1` (`:894–910`). `count` из FatFs ограничен `fs->csize` (`ff.c:3674–3676`) — переполнения нет. **UNCERTAIN (не related to symptom):** семантика «write-verify» отсутствует — `HAL_SD_WriteBlocks` возвращает `HAL_OK` по `DATAEND`, т.е. данные приняты картой, а не записаны в NAND (см. `doc/audit_13_11_26_1.md` U-4).

---

## BUG-1 (ROOT CAUSE) — `disk_status()` противоречит SSOT и `main.c`

**Было** (`src/diskio.c:26–27`):
```c
if ((GPIOA->IDR & GPIO_PIN_8) == 0U) return 0;   /* LOW  = card present */  ← НЕВЕРНО
return STA_NOINIT;                                /* HIGH = card absent  */  ← НЕВЕРНО
```
**SSOT** `doc/WEACT_F405_PINOUT.md` §1 (verbatim): «Механический переключатель слота карты **NC-типа** (R11=100kΩ pull-up к VDD33, R12=10kΩ последовательно с PA8). **Карта вставлена → PA8 HIGH**; карта отсутствует → переключатель замыкает PA8 на GND → PA8 LOW. **Active HIGH = inserted** (вопреки ранним ревизиям).» + §2 note 2.
**Bench** в `src/main.c:127–128`: inserted = 2.5 V (HIGH), absent = 0.4 V (LOW); `main.c:130` трактует `GPIO_PIN_RESET` = **нет карты** → корректно.
→ `disk_status()` возвращал `STA_NOINIT` **ровно когда карта вставлена**. Прямое противоречие внутри одного дерева (Rule 15).

**Статус:** ✅ ИСПРАВЛЕНО.

---

## Реализованные патчи

### FIX-A — `src/diskio.c:26–27` (root cause)
```c
    if ((GPIOA->IDR & GPIO_PIN_8) != 0U) return 0;   /* HIGH = card present (SSOT doc/WEACT_F405_PINOUT.md §1: inserted=PA8 HIGH; matches main.c:130 CD-gate) */
    return STA_NOINIT;                                 /* LOW  = card absent  (NC switch closes PA8 to GND) */
```
`SD_DETECT_Pin == GPIO_PIN_8`, `SD_DETECT_GPIO_Port == GPIOA` (`include/main.h`) — эквивалентность `GPIOA->IDR & GPIO_PIN_8` подтверждена, пины не выдуманы.

### FIX-B — `src/diskio.c:67` и `:88` (defense-in-depth, как просил заказчик)
```c
    if (HAL_SD_ReadBlocks(&hsd, (uint8_t *)buff, sector, count, 1000U) != HAL_OK) {   /* было 200U */
    if (HAL_SD_WriteBlocks(&hsd, (uint8_t *)buff, sector, count, 1000U) != HAL_OK) {  /* было 200U */
```
**Обоснование числами.** `200 ms` был подобран под **старый** `T_IWDG = 1.023 s` (P0-4). После `iwdg.c` `IWDG_PRESCALER_32`, `Reload=4095`: `T_IWDG = 4095 × 32 / 32000 = 4.095 s`. Иерархия теперь `1000 ms (1 SD-op) < 4.095 s (IWDG)`. `HAL_IWDG_Refresh()` вызывается **после каждого** успешного `disk_read`/`disk_write` (`diskio.c`), поэтому последовательные операции не могут суммироваться до reset.
**Что реально покрывает:** 1-bit SDIO @ `SDIOCLK 48 MHz / (ClockDiv 16 + 2) = 2.667 MHz` (теор. ≈ 2.67 МБ/с, реалистично 130–200 КБ/с по докам) — 512 Б передаются за `4096 bit / 2.667 MHz ≈ 1.54 ms`, т.е. **99.85 % бюджета 1000 ms уходит именно на stall карты (GC/program-erase)**, а не на шину. `Timeout` в HAL сторожит только цикл draining FIFO (`stm32f4xx_hal_sd.c:882–890`); внутренний stall карты покрывается `SDMMC_DATATIMEOUT = 0xFFFFFFFF` (`Inc/stm32f4xx_ll_sdmmc.h:304`) — он **не** защищает от «зависшей» шины, поэтому bound на уровне HAL обязателен.
> ⚠️ `doc/final_pre_flash_audit.md` G2 фиксирует иерархию как `200 ms < 4.095 s` — после FIX-A/B она становится `1000 ms < 4.095 s`. Документ требует перевыпуска (flagged, не изменён).

### FIX-C — `src/sd_writer.c` `vibl_write_all()`: bounded retry + корректное возобновление
Ключевой факт, определяющий дизайн (`src/ff.c:3635–3637`):
```c
for ( ;  btw; wbuff += wcnt, fp->fptr += wcnt, fp->obj.objsize = ..., *bw += wcnt, btw -= wcnt) {
    ... if (disk_write(fs->drv, wbuff, sect, cc) != RES_OK) ABORT(fs, FR_DISK_ERR);   /* ff.c:3677 */
```
`ABORT` = `ff.c:455` `{ fp->err = (BYTE)(res); LEAVE_FF(fs, res); }` → **выход изнутри тела**, therefore инкременты выполнились только для завершённых итераций. ⇒ **`*bw` = число байт, уже физически записанных на карту, и `fp->fptr` продвинулся ровно на столько же.** Retry обязан продолжаться с `buf + bw`; повторная отправка чанка целиком **дублировала бы `bw` байт** в неверной позиции файла.
Второй факт (`ff.c:3627`): `(res = (FRESULT)fp->err) != FR_OK` → после `ABORT` **каждый** последующий `f_write` возвращает застарелый код, **не делая ни одной попытки I/O**. `FIL::err` — публичный (`ff.h:158 BYTE err;`) → сброс легален и **обязателен**, иначе retry бессмыслен. (`FR_INVALID_OBJECT` из `validate()` `ff.c:3221` `err` НЕ устанавливает — ещё одно подтверждение, что 9 был не дисковой ошибкой.)

```c
#define VIBL_WRITE_RETRY_MAX 3u
static volatile FRESULT vibl_dbg_last_fr  = FR_OK;
static volatile UINT    vibl_dbg_last_bw  = 0u;
static volatile int     vibl_dbg_attempts = 0;
static volatile FRESULT vibl_dbg_open_fr  = FR_OK;  /* FIX-D */
static volatile int     vibl_dbg_cd_pa8   = -1;     /* FIX-D: 1 = PA8 HIGH = inserted */

static int vibl_write_all(const void *buf, uint32_t len)
{
    const uint8_t *p = (const uint8_t *)buf;

    while (len > 0u) {
        UINT chunk = (len > 0xFFFFu) ? (UINT)0xFFFFu : (UINT)len;
        UINT done  = 0u;
        int  attempt = 0;

        while (done != chunk) {
            UINT bw = 0u;

            if (++attempt > (int)VIBL_WRITE_RETRY_MAX) {
                vibl_dbg_attempts = attempt;
                return VIBL_ERR_WRITE;      /* реальный код - в vibl_dbg_last_fr */
            }

            FRESULT fr = f_write(&vibl_fil, p + done, chunk - done, &bw);

            vibl_dbg_last_fr  = fr;
            vibl_dbg_last_bw  = bw;
            vibl_dbg_attempts = attempt;

            if (bw > 0u) done += bw;        /* учитываем РЕАЛЬНЫЙ прогресс (ff.c:3635) */
            if (fr != FR_OK) vibl_fil.err = 0u;   /* снять latch, иначе попытка N+1 = no-op */
            if ((fr == FR_OK) && (bw == 0u)) return VIBL_ERR_WRITE;  /* защита от spinning */

            HAL_IWDG_Refresh(&hiwdg);       /* см. WARN-1 по бюджету */
        }

        p   += chunk;
        len -= chunk;
    }
    return VIBL_OK;
}
```
Публичные сигнатуры `vibl_open/write_chunk/mark_gap/close` **не изменены** (Rule 11).

### FIX-D — `src/sd_writer.c` `vibl_open()`: диагностика до `f_close()`
```c
    FRESULT fr = f_open(&vibl_fil, filename, FA_CREATE_ALWAYS | FA_WRITE);
    if (fr != FR_OK) {
        vibl_dbg_open_fr = fr;
        return VIBL_ERR_OPEN;
    }
    vibl_dbg_open_fr = fr;

    vibl_dbg_cd_pa8 = (HAL_GPIO_ReadPin(SD_DETECT_GPIO_Port, SD_DETECT_Pin)
                       == GPIO_PIN_SET) ? 1 : 0;

    if (vibl_write_all((const uint8_t *)&vibl_hdr, sizeof(vibl_hdr)) != VIBL_OK) {
        /* STOP HERE: снять vibl_dbg_* и hsd.ErrorCode ДО f_close():
         * ff.c:3832 fp->obj.fs = 0 инвалидирует FIL - позже читать уже нечего. */
        f_close(&vibl_fil);
        vibl_active = 0;
        return VIBL_ERR_WRITE;
    }
```
(`main.h` уже включён в `sd_writer.c:9` → макросы CD доступны.)

**Таблица дешифровки для debugger watch (5 символов, один снимок):**

| `vibl_dbg_last_fr` | `cd_pa8` | Диагноз | Действие |
| :-- | :-- | :-- | :-- |
| **9** `FR_INVALID_OBJECT` | 0 | Карта реально отсутствует | проверить слот/PA8 |
| **9** | 1 | `disk_status()` отверг исправный диск → состояние CD не совпадает с SSOT | `diskio.c disk_status()` |
| **1** `FR_DISK_ERR` | 1 | Физический SD I/O failure | `hsd.ErrorCode`: `HAL_SD_ERROR_TIMEOUT / _DATA_TIMEOUT / _TX_UNDERRUN` (`stm32f4xx_hal_sd.h`) |
| **7** `FR_DENIED` | 1 | Нет `FA_WRITE` / volume full | режим open / `f_getfree` |
| `open_fr=0`, `last_fr≠0` | 1 | Открытие удалось, отказ в I/O | таймаут/питание карты |

---

## WARN-1 — бюджет IWDG при 3 попытках (граница не запредельна, но запас тонкий)
Худший случай одной попытки: один провалившийся I/O-оп ≤ 1000 ms (после `ABORT` FatFs немедленно выходит, `ff.c:455`; успешные ops сбрасывают бюджет через `HAL_IWDG_Refresh` в `disk_read/disk_write`). 3 попытки ≈ **3.000 s < 4.095 s** — запас ≈ **1.1 s**. Если bench покажет, что `f_sync`/`f_close` добавляют в том же окне ещё ≥1 s, уменьшить до 500 ms (одна правка в `diskio.c:67,88`): `3 × 500 = 1.500 s`, запас 2.6 s. **UNCERTAIN:** реальный P99 длительности флеш-столла карты не измерен (`doc/audit_13_11_26_1.md` U-2) — выбирать 1000 vs 500 ms следует по DWT-замеру, а не по догадке.

## WARN-2 — семантика кода 9 изменилась (важно для эксплуатации)
После FIX-A `disk_status()` честен: вынимание карты **в середине сессии** даст `FR_INVALID_OBJECT (9)` в `vibl_write_chunk`, а не `FR_DISK_ERR (1)`. Т.е. `9 = «карта вынута / CD-состояние»`, `1 = «физический I/O»`. `main.c` вызывает `hil3_error_loop(5)` для обоих одинаково — различить их можно только по `vibl_dbg_last_fr`. Предложение (не реализовано без явного разрешения): вынести код в число миганий LED.

## WARN-3 — избыточный internal pull-up на PA8
`src/gpio.c:83` `GPIO_InitStruct.Pull = GPIO_PULLUP` для `SD_DETECT_Pin` при NC-переключателе (R11 100 k pull-up + R12 10 k series). Безвредно (switch short — доминирующий путь), но дублирует внешний R11. Правка требует изменения `.ioc`/`gpio.c` — **TBD, не трогал**.

## WARN-4 — двойная `HAL_SD_Init` больше не маскирует симптом
До FIX-A каждая публичная FatFs-операция делала ре-инициализацию (`ff.c:3034→3036`), что (а) стоило ~`SDIO_CMDTIMEOUT 5000` циклов на команду (`Inc/stm32f4xx_ll_sdmmc.h:325`) и (б) создавало иллюзию «f_open успешен». После FIX-A путь `disk_initialize` вызывается один раз на mount. Побочный выигрыш: устранён скрытый re-init на каждый `f_write`.

---

## Статус сборки (проверено после КАЖДОГО патча — Rule 19)

| После | RAM | Flash | Результат |
| :-- | :-- | :-- | :-- |
| FIX-A | 27 408 B (20.9 %) | 20 384 B (1.9 %) | SUCCESS 1.35 s |
| FIX-B | 27 408 B | 20 392 B | SUCCESS 1.36 s |
| FIX-C | 27 420 B | 20 460 B | SUCCESS 1.39 s |
| FIX-C refinement + FIX-D | 27 424 B | 20 500 B | SUCCESS 1.36 s |

0 новых warnings. Изменение размера ≈ +160 B flash / +16 B RAM на всё.

## Следующие шаги (требуют явного разрешения — Rule 18)
1. `pio run -t upload` — **НЕ выполнял** (флеш = действие над железом).
2. Acceptance на железе: свежая FAT32 карта → `vibl_open("LOG0003.VIB", …)` → ожидаем `VIBL_OK` и **отсутствие** `hil3_error_loop(3)`; в watch: `vibl_dbg_open_fr==0`, `vibl_dbg_last_fr==0`, `vibl_dbg_cd_pa8==1`, `vibl_dbg_attempts==1`.
3. Проверка обратного сценария: карта вынута при подаче питания → ожидаем `hil3_error_loop(2)` (CD-gate `main.c:130`), **не** reset по IWDG.
4. DWT-замер P99 длительности `vibl_write_chunk` за 24 h → окончательный выбор 500 vs 1000 ms (WARN-1).
5. Перевыпустить `doc/final_pre_flash_audit.md` G2/G3 и `doc/audit_13_11_26_1.md` P0-4 (устаревшая полярность PA8 и таймауты).
~~~

Кратко по-русски: **файл создавался «успешно» только потому, что `disk_status()` возвращал `STA_NOINIT` при вставленной карте, и FatFs каждый раз перемонтировал том** (`ff.c:3034→3036`). Код **9** — это `validate()` (`ff.c:3221/3236`), а не таймаут: при таймауте `disk_write` FatFs вернул бы **1** (`FR_DISK_ERR`). 256-байтный заголовок вообще не порождал вызова `HAL_SD_WriteBlocks` (`btw < 512` → путь через `fp->buf`, `ff.c:3715–3716`), поэтому 200 ms в этом отказе не участвовали. Таймаут поднят до 1000 ms как defense-in-depth (безопасно: 3×1000 ms < 4.095 s IWDG), retry добавлен, но с обязательным возобновлением с `buf + bw` и сбросом `vibl_fil.err` — иначе он дублировал бы байты и был бы бессмысленным.