# ЧТО ПРОДЕЛАНО ПОСЛЕ АУДИТА (ФАЙЛ - audit_13_11_26.md)



## Галлюцинации модели (можно игнорировать)

1. BUG-1 (конфликт PA5 / DAC/ADC): она пишет про MX_ADC1_Init, dac.c и HIL-стенд.
   Мы это все удалили. Она нашла старые файлы в контексте или галлюцинирует.
2. BUG-10 (частота 108 МГц вместо 168): она пишет, что PLLM=8, PLLN=216.
   Это ложь. В твоем main.c от CubeMX стоит PLLM=4, PLLN=168, PLLP=2.
   Это дает ровно 168 МГц. Модель просто перепутала с каким-то стандартным
   шаблоном.
3. BUG-8 (RTC): она права, что мы не читаем RTC. Но для первого теста нам важнее
   проверить, что данные вообще пишутся. Метки времени добавим потом.

## Реальные баги (за что нужно хвататься)

Модель нашла две колоссальные ошибки в нашей архитектуре, которые я, если честно,
проглядел. Если бы мы прошились, мы бы получили мусор.

### BUG-5: состояние гонки в Ping-Pong буфере

Что случилось: когда ISR заполняет половину B и сбрасывает sample_idx = 0,
он начинает писать в половину A. Но главный цикл в этот момент все еще может
записывать половину A на SD-карту. В итоге ISR затирает данные прямо во время
их отправки. Файл получится валидным по CRC, но данные внутри будут кашей.
Как лечить: нужны два независимых массива (A и B) и флаг active_buf. ISR пишет
только в активный массив. Когда он заполнен, ISR переключается на другой,
а первый отдает главному циклу.

### BUG-4: отсутствие vibl_close()

Что случилось: мы открываем файл (vibl_open), но никогда его не закрываем.
В формате VIBL в конце файла должен писаться Trailer (финальный CRC и количество
сэмплов). Из-за этого парсер не сможет проверить целостность.
Как лечить: нужен механизм graceful stop по кнопке PC13, который вызовет
vibl_close().

### BUG-9: ошибка в Python-парсере (CRC)

Модель права на 100%. В parser_vibl.py мы обнуляем байты [51:55], а в C-коде поле
CRC занимает [52:56]. Из-за этого даже идеальный файл будет выдавать ошибку CRC.
Это надо поправить в питоне.

## План действий (по одному шагу)

Давай исправим две главные вещи: гонку буферов и закрытие файла.

### Шаг 1: исправляем гонку буферов в main.c

Замени блоки в main.c на эти.

USER CODE BEGIN PV:

```c
FATFS fs;
/* Два независимых буфера. 2048 сэмплов * 3 оси = 6144 int16_t на буфер */
static int16_t pingpong_buf[2][VIBL_SAMPLES_PER_CHUNK * 3u];
static volatile uint32_t sample_idx = 0u;
static volatile uint8_t  active_buf = 0u;  /* 0 или 1 - в какой буфер пишет ISR */
static volatile uint8_t  buf_ready  = 0u;  /* 0 = нет данных, 1 = буфер 0 готов, 2 = буфер 1 готов */
static volatile uint8_t  lis3dsh_err = 0u;
static void hil3_error_loop(uint8_t err_code);
```

USER CODE BEGIN 4 (прерывание):

```c
void HAL_TIM_PeriodElapsedCallback(TIM_HandleTypeDef *htim)
{
    if (htim->Instance == TIM2) {
        int16_t x, y, z;
        if (lis3dsh_read(&x, &y, &z) != 0) {
            lis3dsh_err = 1u;
            return;
        }
        /* Пишем в ТЕКУЩИЙ активный буфер */
        pingpong_buf[active_buf][sample_idx++] = x;
        pingpong_buf[active_buf][sample_idx++] = y;
        pingpong_buf[active_buf][sample_idx++] = z;
        /* Если буфер заполнен */
        if (sample_idx >= (VIBL_SAMPLES_PER_CHUNK * 3u)) {
            buf_ready = active_buf + 1;  /* 1 для буфера 0, 2 для буфера 1 */
            active_buf ^= 1;             /* Переключаемся на ДРУГОЙ буфер */
            sample_idx = 0;              /* Сбрасываем счетчик */
        }
    }
}
```

USER CODE BEGIN 3 (главный цикл):

```c
if (lis3dsh_err != 0u) hil3_error_loop(4);

if (buf_ready == 1u) {
    /* Буфер 0 готов. ISR уже пишет в буфер 1. */
    if (vibl_write_chunk(&pingpong_buf[0][0], VIBL_SAMPLES_PER_CHUNK) != VIBL_OK) {
        hil3_error_loop(5);
    }
    buf_ready = 0u;  /* Сбрасываем флаг только после записи */
}
else if (buf_ready == 2u) {
    /* Буфер 1 готов. ISR уже пишет в буфер 0. */
    if (vibl_write_chunk(&pingpong_buf[1][0], VIBL_SAMPLES_PER_CHUNK) != VIBL_OK) {
        hil3_error_loop(5);
    }
    buf_ready = 0u;
}
HAL_IWDG_Refresh(&hiwdg);
```

### Шаг 2: исправляем Python-парсер

Открой parser_vibl.py в VS Code. Найди строку:
crc_bytes[51:55] = b'\0'*4
Замени ее на:
crc_bytes[52:56] = b'\x00\x00\x00\x00'

### Шаг 3: исправляем disk_status в diskio.c (чтобы не висло при вытаскивании карты)

Найди функцию disk_status. Замени STA_NODISK на STA_NOINIT.

```c
DSTATUS disk_status(BYTE pdrv)
{
    if ((GPIOA->IDR & GPIO_PIN_8) != 0U) return STA_NOINIT;  /* Карта вытащена */
    return 0;
}
```
### Шаг 4: исправляем LIS3DSH.c

/**
 * @file    lis3dsh.c
 * @brief   LIS3DSH driver over SPI1 (using CubeMX init).
 */
#include "lis3dsh.h"
#include "spi.h"

#define LIS3DSH_CS_PORT   GPIOA
#define LIS3DSH_CS_PIN    GPIO_PIN_4

/* Register map */
#define WHO_AM_I_ADDR     0x0Fu
#define CTRL_REG4_ADDR    0x20u
#define CTRL_REG5_ADDR    0x24u
#define CTRL_REG6_ADDR    0x25u
#define OUT_X_L_ADDR      0x28u

/* SPI Command bits */
#define SPI_READ_BIT      0x80u
#define SPI_AUTO_INC_BIT  0x40u
#define SPI_RW_ADDR       (SPI_READ_BIT | SPI_AUTO_INC_BIT)

/* --- ПРАВИЛЬНЫЕ ЗНАЧЕНИЯ РЕГИСТРОВ (СОГЛАСНО ДАТАШИТУ) --- */
#define WHO_AM_I_VAL      0x3Fu
#define CTRL_REG4_VAL     0x9Fu   /* ODR=1.6 kHz (1001), BDU=1, XYZ=111 */
#define CTRL_REG5_VAL     0x20u   /* Full scale ±16 g, BW=800Hz, 4-wire SPI */
#define CTRL_REG6_VAL     0x10u   /* ADD_INC=1 (автоинкремент включен!), BOOT=0, FIFO=0 */

#define LIS3DSH_SPI_TIMEOUT_MS  10u

static void cs_low(void)  { HAL_GPIO_WritePin(LIS3DSH_CS_PORT, LIS3DSH_CS_PIN, GPIO_PIN_RESET); }
static void cs_high(void) { HAL_GPIO_WritePin(LIS3DSH_CS_PORT, LIS3DSH_CS_PIN, GPIO_PIN_SET); }

static int spi_tx_rx(const uint8_t *tx, uint8_t *rx, uint16_t len)
{
    return (HAL_SPI_TransmitReceive(&hspi1, tx, rx, len, LIS3DSH_SPI_TIMEOUT_MS) == HAL_OK) ? 0 : -1;
}

static int lis3dsh_write_reg(uint8_t addr, uint8_t value)
{
    uint8_t tx[2] = { addr, value };
    uint8_t rx_dummy[2];
    cs_low();
    int rc = spi_tx_rx(tx, rx_dummy, 2u);
    cs_high();
    return rc;
}

int lis3dsh_init(void)
{
    HAL_Delay(20); 

    /* CS (PA4) init manually (CubeMX doesn't do this by default) */
    GPIO_InitTypeDef g = {0};
    __HAL_RCC_GPIOA_CLK_ENABLE();
    g.Pin       = LIS3DSH_CS_PIN;
    g.Mode      = GPIO_MODE_OUTPUT_PP;
    g.Pull      = GPIO_NOPULL;
    g.Speed     = GPIO_SPEED_FREQ_VERY_HIGH;
    HAL_GPIO_Init(LIS3DSH_CS_PORT, &g);
    cs_high();

    /* WHO_AM_I check */
    uint8_t tx[2] = { WHO_AM_I_ADDR | SPI_READ_BIT, 0x00 };
    uint8_t rx[2] = { 0u };
    cs_low();
    int rc = spi_tx_rx(tx, rx, 2u);
    cs_high();
    if ((rc != 0) || (rx[1] != WHO_AM_I_VAL)) {
        return -1; /* Ошибка связи или неверный ID */
    }

    /* Настройка датчика */
    if (lis3dsh_write_reg(CTRL_REG4_ADDR, CTRL_REG4_VAL) != 0) { return -1; }
    if (lis3dsh_write_reg(CTRL_REG5_ADDR, CTRL_REG5_VAL) != 0) { return -1; }
    if (lis3dsh_write_reg(CTRL_REG6_ADDR, CTRL_REG6_VAL) != 0) { return -1; }

    return 0;
}

int lis3dsh_read(int16_t *x, int16_t *y, int16_t *z)
{
    if ((x == NULL) || (y == NULL) || (z == NULL)) return -1;

    /* Burst read: 0xE8 = Read(0x80) | AutoInc(0x40) | OUT_X_L(0x28) */
    uint8_t tx[7] = { OUT_X_L_ADDR | SPI_RW_ADDR, 0u, 0u, 0u, 0u, 0u, 0u };
    uint8_t rx[7];

    cs_low();
    int rc = spi_tx_rx(tx, rx, 7u);
    cs_high();
    if (rc != 0) return -1;

    /* rx[0] - эхо адреса, данные начинаются с rx[1] */
    *x = (int16_t)(((uint16_t)rx[2] << 8) | rx[1]);
    *y = (int16_t)(((uint16_t)rx[4] << 8) | rx[3]);
    *z = (int16_t)(((uint16_t)rx[6] << 8) | rx[5]);
    return 0;
}
```