/**
 * @file    lis3dsh.h
 * @brief   LIS3DSH accelerometer driver over SPI1 (WeAct STM32F405RG).
 */
#ifndef LIS3DSH_H
#define LIS3DSH_H

#include "main.h"

#ifdef __cplusplus
extern "C" {
#endif


int lis3dsh_init(void);
int lis3dsh_read(int16_t *x, int16_t *y, int16_t *z);

#ifdef __cplusplus
}
#endif

#endif /* LIS3DSH_H */