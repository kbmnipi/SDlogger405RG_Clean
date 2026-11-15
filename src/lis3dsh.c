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

/* SPI Command bits (P0-2 / audit_13_11_26_1 BUG-1, closed by doc/DS_lis3dsh.md §5.2:
 *   First byte (command): bit 0 (transmitted first, i.e. MSB of the byte): RW.
 *                         bit 1..7: AD6..AD0 register address.
 *   read command byte = 0x80 | (addr & 0x7F)
 *   "There is no MS/auto-increment bit inside the SPI command byte on this device."
 * Auto-increment is controlled ONLY by CTRL_REG6.ADD_INC (bit 4), already = 1.
 * Bit 0x40 therefore belongs to the sub-address field: OR-ing it into the command
 * byte retargets the burst from OUT_X_L (0x28) to 0x68 (ST2_9, write-only) ->
 * silent data corruption (seq/CRC stay perfect, axes are garbage).
 */
#define SPI_READ_BIT      0x80u
#define SPI_RW_ADDR       SPI_READ_BIT   /* burst = 0x80 | 0x28 = 0xA8 */

/* --- ПРАВИЛЬНЫЕ ЗНАЧЕНИЯ РЕГИСТРОВ (СОГЛАСНО ДАТАШИТУ) --- */
#define WHO_AM_I_VAL      0x3Fu
#define CTRL_REG4_VAL     0x9Fu   /* ODR=1.6 kHz (1001), BDU=1, XYZ=111 */
#define CTRL_REG5_VAL     0x20u   /* Full scale ±16 g, BW=800Hz, 4-wire SPI */
#define CTRL_REG6_VAL     0x10u   /* ADD_INC=1 (автоинкремент включен!), BOOT=0, FIFO=0 */

/* P2-8 (audit WARN-2): 10 ms -> 1 ms. This timeout is consumed INSIDE the
 * 1600 Hz TIM2 callback (period 625 us), so 10 ms = ~16 lost ticks.
 * A legitimate 7-byte frame costs 56 bit / 1.3125 MHz = 42.7 us (+~10 us HAL
 * overhead = ~53 us), so 1 ms is still a 19x margin. */
#define LIS3DSH_SPI_TIMEOUT_MS  1u

/* P0-2 compile-time guard: the burst command byte must be 0xA8
 * (0x80 read | 0x28 OUT_X_L) and must NOT contain 0x40. */
_Static_assert((OUT_X_L_ADDR | SPI_RW_ADDR) == 0xA8u, "LIS3DSH burst command must be 0xA8 (see DS_lis3dsh.md §5.2)");

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

/* P0-2 hardening: single-byte register read, command = 0x80 | addr
 * (doc/DS_lis3dsh.md §5.2 / §10 "SPI read: CS low, send (0x80 | addr), read N bytes"). */
static int lis3dsh_read_reg(uint8_t addr, uint8_t *value)
{
    if (value == NULL) return -1;
    uint8_t tx[2] = { (uint8_t)(addr | SPI_READ_BIT), 0x00u };
    uint8_t rx[2] = { 0u };
    cs_low();
    int rc = spi_tx_rx(tx, rx, 2u);
    cs_high();
    if (rc != 0) return -1;
    *value = rx[1];         /* rx[0] = echo/unused, rx[1] = register content */
    return 0;
}

int lis3dsh_init(void)
{
        /* LIS3DSH requires ~10-20 ms to boot, but bench tests show 100 ms
     * is safer to guarantee SPI interface readiness on cold start
     * (first WHO_AM_I read returns 0xFF; a re-read >1 s later returns 0x3F). */
    HAL_Delay(100);

    /* CS (PA4) init manually (CubeMX doesn't do this by default) */
    GPIO_InitTypeDef g = {0};
    __HAL_RCC_GPIOA_CLK_ENABLE();
    g.Pin       = LIS3DSH_CS_PIN;
    g.Mode      = GPIO_MODE_OUTPUT_PP;
    g.Pull      = GPIO_NOPULL;
    g.Speed     = GPIO_SPEED_FREQ_VERY_HIGH;
    HAL_GPIO_Init(LIS3DSH_CS_PORT, &g);
    cs_high();

        /* P2-11 (cold-start fix): WHO_AM_I check with retry loop.
     * Evidence: on cold boot the FIRST SPI read returns 0xFF (sensor not
     * ready); the second read (>1 s later) returns 0x3F. A single attempt
     * therefore fails -> lis3dsh_init() == -1 -> hil3_error_loop(1) in main.c.
     * 5 attempts x (2-byte SPI frame ~13 us @1.3125 MHz + 10 ms HAL_Delay)
     * = ~50 ms worst case, bounded, still inside the ~100 ms power-up budget. */
    uint8_t who_am_i = 0u;
    int rc = -1;
    for (int i = 0; i < 5; i++) {
        if (lis3dsh_read_reg(WHO_AM_I_ADDR, &who_am_i) == 0 && who_am_i == WHO_AM_I_VAL) {
            rc = 0;
            break;
        }
        HAL_Delay(10);
    }
    if (rc != 0) {
        return -1; /* Ошибка связи или неверный ID после 5 попыток */
    }

        /* Настройка датчика — P2-10 (audit WARN-6, doc/DS_lis3dsh.md §7.23-7.25 /
     * §10 init sequence): write REG6 -> REG5 -> REG4, i.e. the output-data-rate
     * register LAST. Only CTRL_REG4 carries ODR, so ordering the writes this way
     * guarantees sampling never starts before FS/BW/SIM (REG5) and ADD_INC (REG6)
     * are settled. The previous order worked only because REG4 was written first
     * with the same value it would end with — i.e. it relied on the default
     * power-up state and was NOT brown-out safe. */
    if (lis3dsh_write_reg(CTRL_REG6_ADDR, CTRL_REG6_VAL) != 0) { return -1; }  /* ADD_INC=1 first */
    if (lis3dsh_write_reg(CTRL_REG5_ADDR, CTRL_REG5_VAL) != 0) { return -1; }  /* FS, BW, SIM       */
    if (lis3dsh_write_reg(CTRL_REG4_ADDR, CTRL_REG4_VAL) != 0) { return -1; }  /* ODR last -> starts */

    /* P0-2 hardening (audit_13_11_26_1 §3.1, +5 lines, ~12 us one-shot cost):
     * read back CTRL_REG4 and confirm the write landed. Kept AFTER all three
     * writes, as it must be (CTRL_REG4 is now the last write).
     * CTRL_REG4 = ODR3..ODR0 | BDU | Zen | Yen | Xen (DS §7.20). Written value
     * 0x9F = ODR 1001 (1600 Hz) + BDU 1 + XYZ 111; reserved/writable low bits
     * may be altered by the device, so compare the top 5 bits only:
     *   (rb & 0xF8) == 0x98  ->  ODR=1001, BDU=1   (read-back accepted: 0x98..0x9F).
     * A mismatch means the SPI write path (CS/mode/wiring) is broken -> -2. */
    uint8_t ctrl4_rb = 0u;
    if (lis3dsh_read_reg(CTRL_REG4_ADDR, &ctrl4_rb) != 0) { return -1; }
    if ((ctrl4_rb & 0xF8u) != 0x98u) { return -2; }

    return 0;
}

int lis3dsh_read(int16_t *x, int16_t *y, int16_t *z)
{
    if ((x == NULL) || (y == NULL) || (z == NULL)) return -1;

    /* Burst read: 0xA8 = Read(0x80) | OUT_X_L(0x28). Auto-increment comes from
     * CTRL_REG6.ADD_INC=1 (P0-2), NOT from a bit in the command byte. */
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