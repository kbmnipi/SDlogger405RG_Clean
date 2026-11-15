# LIS3DSH - Digital 3-axis accelerometer. Register / interface reference

Plain-text conversion of ST datasheet DocID022405 Rev 3 (September 2017).
Purpose: source document for writing a device driver.
Timing diagrams and package drawings are omitted. All register content is kept.

Device: MEMS digital output motion sensor, ultra-low-power high-performance
3-axis "nano" accelerometer with 2 embedded programmable state machines.
Package: LGA-16, 3x3x1.0 mm. Order code: LIS3DSHTR (tape and reel).
Temperature range: -40 to +85 degC.

## 1. Key features

- Supply voltage 1.71 V to 3.6 V (factory calibrated at 2.5 V)
- Independent IO supply (Vdd_IO), 1.8 V compatible
- Selectable full scale: +/-2 g, +/-4 g, +/-6 g, +/-8 g, +/-16 g
- I2C / SPI digital output interface (SPI 4-wire or 3-wire)
- 16-bit output data (two's complement), 3 axes
- Output data rates 3.125 Hz to 1.6 kHz
- Two programmable embedded state machines (16 steps each)
- Embedded temperature sensor (8-bit)
- Embedded self-test
- Embedded FIFO (32 levels x 3 channels)
- Two interrupt pins: INT1/DRDY, INT2
- 10000 g high shock survivability

## 2. Pin description

| Pin | Name | Function |
| --- | --- | --- |
| 1 | Vdd_IO | Power supply for I/O pins |
| 2 | NC | Not connected |
| 3 | NC | Not connected |
| 4 | SCL / SPC | I2C serial clock (SCL) / SPI serial port clock (SPC) |
| 5 | GND | 0 V supply |
| 6 | SDA / SDI / SDO | I2C serial data (SDA) / SPI data input (SDI) / 3-wire serial data output (SDO) |
| 7 | SEL / SDO | I2C address selection / SPI serial data output (SDO) |
| 8 | CS | SPI enable, I2C/SPI mode selection: 1 = SPI idle, I2C enabled; 0 = SPI mode, I2C disabled |
| 9 | INT2 | Interrupt 2 |
| 10 | Reserved | Connect to GND |
| 11 | INT1 / DRDY | Interrupt 1 / data ready |
| 12 | GND | 0 V supply |
| 13 | GND | 0 V supply |
| 14 | Vdd | Power supply |
| 15 | Reserved | Connect to Vdd, connect to GND, or leave unconnected |
| 16 | GND | 0 V supply |

Hardware notes:
- Decoupling: 100 nF ceramic + 10 uF as close as possible to pin 14 (Vdd).
- Vdd and Vdd_IO must both be present for correct operation.
- Vdd may be removed while Vdd_IO is kept: communication bus stays usable,
  measurement chain is powered off.
- For I2C usage CS must be tied high (to Vdd_IO).
- I2C: SCL and SDA need external pull-ups to Vdd_IO.
- Axis orientation: top view, pin 1 indicator corner; X to the right, Y up,
  Z out of the package top (positive acceleration direction per datasheet figure).

## 3. Electrical characteristics

Vdd = 2.5 V, T = 25 degC unless noted.

| Symbol | Parameter | Min | Typ | Max | Unit |
| --- | --- | --- | --- | --- | --- |
| Vdd | Supply voltage | 1.71 | 2.5 | 3.6 | V |
| Vdd_IO | I/O supply voltage | 1.71 | - | Vdd+0.1 | V |
| IddA | Current, active mode, ODR 1.6 kHz | - | 225 | - | uA |
| IddA | Current, active mode, ODR 3.125 Hz | - | 11 | - | uA |
| IddPdn | Current, power-down / standby | - | 2 | - | uA |
| VIH | Digital high-level input | 0.8*Vdd_IO | - | - | V |
| VIL | Digital low-level input | - | - | 0.2*Vdd_IO | V |
| VOH | High-level output | 0.9*Vdd_IO | - | - | V |
| VOL | Low-level output | - | - | 0.1*Vdd_IO | V |
| Top | Operating temperature | -40 | - | +85 | degC |

Absolute maximum ratings: Vdd and Vdd_IO -0.3 to 4.8 V;
input voltage on CS, SCL/SPC, SDA/SDI/SDO, SDO/SEL: -0.3 to Vdd_IO+0.3 V;
supply on any pin must never exceed 4.8 V;
shock 3000 g for 0.5 ms, 10000 g for 0.1 ms (powered and unpowered);
storage -40 to +125 degC; ESD 2 kV HBM.

## 4. Mechanical characteristics and sensitivity

| Symbol | Parameter | Condition | Typ | Unit |
| --- | --- | --- | --- | --- |
| FS | Full scale | FSCALE = 000 | +/-2.0 | g |
| FS | Full scale | FSCALE = 001 | +/-4.0 | g |
| FS | Full scale | FSCALE = 010 | +/-6.0 | g |
| FS | Full scale | FSCALE = 011 | +/-8.0 | g |
| FS | Full scale | FSCALE = 100 | +/-16.0 | g |
| So | Sensitivity | FSCALE = 000 (+/-2 g) | 0.06 | mg/LSB |
| So | Sensitivity | FSCALE = 001 (+/-4 g) | 0.12 | mg/LSB |
| So | Sensitivity | FSCALE = 010 (+/-6 g) | 0.18 | mg/LSB |
| So | Sensitivity | FSCALE = 011 (+/-8 g) | 0.24 | mg/LSB |
| So | Sensitivity | FSCALE = 100 (+/-16 g) | 0.73 | mg/LSB |
| TCSo | Sensitivity change vs temperature | FS = 00 | 0.01 | %/degC |
| TyOff | Zero-g level offset accuracy | FS = 00 | +/-40 | mg |
| TCOff | Zero-g level change vs temperature | max delta from 25 degC | +/-0.5 | mg/degC |
| An | Noise density | FS = 00, normal mode, ODR 100 Hz | 150 | ug/sqrt(Hz) |
| ST | Self-test positive difference | +/-2 g, X and Y axes, ST2:ST1 = 01 | 140 | mg |
| ST | Self-test positive difference | +/-2 g, Z axis, ST2:ST1 = 01 | 590 | mg |

Conversion: acceleration_mg = raw_int16 * sensitivity_mg_per_LSB.
Example +/-2 g: 1 g = about 16667 LSB; +/-16 g: 0.73 mg/LSB.

Zero-g level: sensor flat on a table reads about 0 g on X and Y, +1 g on Z.
Factory calibrated for sensitivity and zero-g; trimming stored in non-volatile
memory and reloaded at every power-up. No user calibration required.

Self-test: ST bits apply an electrostatic force simulating a known acceleration.
Output with self-test = real acceleration + test force.
Self-test output change = OUTPUT(ST=01) - OUTPUT(ST=00), compare with ST values above.

## 5. Digital interfaces

Registers are accessible via I2C or SPI. Same pins are shared.
CS high = I2C enabled / SPI idle. CS low = SPI active.

### 5.1 I2C

- Slave only. Standard mode (100 kHz) and fast mode (400 kHz).
- 7-bit slave address SAD = 00111xxb, xx set by SEL/SDO pin:
  - SEL/SDO tied to supply (high): SAD = 0011101b = 0x1D
  - SEL/SDO tied to GND (low):     SAD = 0011110b = 0x1E
- Full bytes on the wire:
  - SEL high: write 0x3A? no: write 0x3C, read 0x3D (address 0x1D)
  - SEL low:  write 0x3A, read 0x3B (address 0x1E)
- Two devices can share the bus using the two addresses.
- MSB first, acknowledge mandatory after each byte.

Write one byte:
ST | SAD+W | SAK | SUB | SAK | DATA | SAK | SP

Write multiple bytes (auto-increment if ADD_INC=1):
ST | SAD+W | SAK | SUB | SAK | DATA | SAK | DATA | SAK | SP

Read one byte:
ST | SAD+W | SAK | SUB | SAK | SR | SAD+R | SAK | DATA | NMAK | SP

Read multiple bytes (auto-increment if ADD_INC=1):
ST | SAD+W | SAK | SUB | SAK | SR | SAD+R | SAK | DATA | MAK | DATA | MAK | DATA | NMAK | SP

SUB = sub-address byte: 7 LSB = register address; address increment behaviour
is defined by the ADD_INC bit of CTRL_REG6 (not by a bit inside SUB).

### 5.2 SPI

- Slave. 4-wire: CS, SPC, SDI, SDO. 3-wire: CS, SPC, SDI/SDO (SIM=1).
- Max clock 10 MHz.
- CS low at start of transfer, high at end. SPC is held high when CS is high.
- Data is driven on the falling edge of SPC and captured on the rising edge
  => SPI mode 3 (CPOL = 1, CPHA = 1). MSB first.
- Single transfer = 16 clock pulses; multiple byte = 16 + N*8 pulses.

First byte (command):
- bit 0 (transmitted first, i.e. MSB of the byte): RW. 1 = read, 0 = write.
- bit 1..7: AD6..AD0 register address.

So: read command byte = 0x80 | (addr & 0x7F); write command byte = addr & 0x7F.

Second byte: DI7..DI0 (write) or DO7..DO0 (read).
Further bytes: additional data, address auto-increments only if ADD_INC = 1
(CTRL_REG6 bit 4). If ADD_INC = 0 the same address is repeated for every byte.
There is no MS/auto-increment bit inside the SPI command byte on this device.

3-wire mode: set SIM = 1 (CTRL_REG5 bit 0). SDI becomes bidirectional SDI/SDO.
Read protocol identical, data phase driven by the device on the same line.

Key SPI timing (characterization, 10 MHz, 3- and 4-wire):
clock cycle min 100 ns; CS setup 6 ns; CS hold 8 ns; SDI setup 5 ns; SDI hold 15 ns;
SDO valid max 50 ns; SDO hold min 9 ns; SDO disable max 50 ns.
When no communication is ongoing SDO is driven by an internal pull-up.

Key I2C timing: SCL max 100 kHz (standard) / 400 kHz (fast);
SCL low 4.7 us / 1.3 us; SCL high 4.0 us / 0.6 us;
SDA setup 250 ns / 100 ns; bus free between STOP and START 4.7 us / 1.3 us.

## 6. Register map (complete)

Type: r = read only, w = write only, r/w = read/write.
Addresses not listed are reserved: do not write (may permanently damage device).
Registers loaded at boot contain factory calibration; do not modify.

| Address | Name | Type | Default | Description |
| --- | --- | --- | --- | --- |
| 0x0C | OUT_T | r | - | Temperature output |
| 0x0D | INFO1 | r | 0x21 | Information register 1 |
| 0x0E | INFO2 | r | 0x00 | Information register 2 |
| 0x0F | WHO_AM_I | r | 0x3F | Device ID |
| 0x10 | OFF_X | r/w | 0x00 | X-axis offset correction |
| 0x11 | OFF_Y | r/w | 0x00 | Y-axis offset correction |
| 0x12 | OFF_Z | r/w | 0x00 | Z-axis offset correction |
| 0x13 | CS_X | r/w | 0x00 | Constant shift X (state machine only) |
| 0x14 | CS_Y | r/w | 0x00 | Constant shift Y (state machine only) |
| 0x15 | CS_Z | r/w | 0x00 | Constant shift Z (state machine only) |
| 0x16 | LC_L | r/w | 0x01 | Long counter low |
| 0x17 | LC_H | r/w | 0x00 | Long counter high |
| 0x18 | STAT | r | - | Interrupt status / synchronization |
| 0x19 | PEAK1 | r | - | Peak value SM1 |
| 0x1A | PEAK2 | r | - | Peak value SM2 |
| 0x1B | VFC_1 | r/w | - | Vector filter coefficient 1 |
| 0x1C | VFC_2 | r/w | - | Vector filter coefficient 2 |
| 0x1D | VFC_3 | r/w | - | Vector filter coefficient 3 |
| 0x1E | VFC_4 | r/w | - | Vector filter coefficient 4 |
| 0x1F | THRS3 | r/w | - | Threshold value 3 |
| 0x20 | CTRL_REG4 | r/w | 0x07 | ODR, BDU, axis enable |
| 0x21 | CTRL_REG1 | r/w | 0x00 | SM1 control |
| 0x22 | CTRL_REG2 | r/w | 0x00 | SM2 control |
| 0x23 | CTRL_REG3 | r/w | 0x00 | Interrupt control, vector filter, soft reset |
| 0x24 | CTRL_REG5 | r/w | 0x00 | BW, full scale, self-test, SPI mode |
| 0x25 | CTRL_REG6 | r/w | 0x10 | Boot, FIFO, address increment, interrupt routing |
| 0x27 | STATUS | r | - | Data status / overrun |
| 0x28 | OUT_X_L | r | 0x00 | X output low |
| 0x29 | OUT_X_H | r | 0x00 | X output high |
| 0x2A | OUT_Y_L | r | 0x00 | Y output low |
| 0x2B | OUT_Y_H | r | 0x00 | Y output high |
| 0x2C | OUT_Z_L | r | 0x00 | Z output low |
| 0x2D | OUT_Z_H | r | 0x00 | Z output high |
| 0x2E | FIFO_CTRL | r/w | 0x00 | FIFO mode and watermark |
| 0x2F | FIFO_SRC | r | - | FIFO status |
| 0x40-0x4F | ST1_1..ST1_16 | w | - | SM1 code registers (16 steps) |
| 0x50 | TIM4_1 | w | - | SM1 timer 4 (8-bit) |
| 0x51 | TIM3_1 | w | - | SM1 timer 3 (8-bit) |
| 0x52-0x53 | TIM2_1 | w | - | SM1 timer 2 (16-bit) |
| 0x54-0x55 | TIM1_1 | w | - | SM1 timer 1 (16-bit) |
| 0x56 | THRS2_1 | w | - | SM1 threshold 2 |
| 0x57 | THRS1_1 | w | - | SM1 threshold 1 |
| 0x59 | MASK1_B | w | - | SM1 axis and sign mask (swap) |
| 0x5A | MASK1_A | w | - | SM1 axis and sign mask (default) |
| 0x5B | SETT1 | w | - | SM1 detection settings |
| 0x5C | PR1 | r | - | SM1 program / reset pointer |
| 0x5D-0x5E | TC1 | r | - | SM1 timer counter output (16-bit) |
| 0x5F | OUTS1 | r | - | SM1 output flags |
| 0x60-0x6F | ST2_1..ST2_16 | w | - | SM2 code registers (16 steps) |
| 0x70 | TIM4_2 | w | - | SM2 timer 4 (8-bit) |
| 0x71 | TIM3_2 | w | - | SM2 timer 3 (8-bit) |
| 0x72-0x73 | TIM2_2 | w | - | SM2 timer 2 (16-bit) |
| 0x74-0x75 | TIM1_2 | w | - | SM2 timer 1 (16-bit) |
| 0x76 | THRS2_2 | w | - | SM2 threshold 2 |
| 0x77 | THRS1_2 | w | - | SM2 threshold 1 |
| 0x78 | DES2 | w | - | SM2 decimation factor |
| 0x79 | MASK2_B | w | - | SM2 axis and sign mask (swap) |
| 0x7A | MASK2_A | w | - | SM2 axis and sign mask (default) |
| 0x7B | SETT2 | w | - | SM2 detection settings |
| 0x7C | PR2 | r | - | SM2 program / reset pointer |
| 0x7D-0x7E | TC2 | r | - | SM2 timer counter output (16-bit) |
| 0x7F | OUTS2 | r | - | SM2 output flags |

## 7. Register descriptions

### 7.1 OUT_T (0x0C), read only

| Bit | 7 | 6 | 5 | 4 | 3 | 2 | 1 | 0 |
| --- | --- | --- | --- | --- | --- | --- | --- | --- |
| Name | Temp7 | Temp6 | Temp5 | Temp4 | Temp3 | Temp2 | Temp1 | Temp0 |

Temp7..Temp0: temperature data, 8-bit resolution, 1 LSB per degree,
two's complement (signed int8).
Note: the datasheet gives no absolute reference / offset for this value;
it is a relative temperature reading.

### 7.2 INFO1 (0x0D), read only

Default 0x21 = 0010 0001. Read-only information register (no field description
in the datasheet).

### 7.3 INFO2 (0x0E), read only

Default 0x00 = 0000 0000. Read-only information register.

### 7.4 WHO_AM_I (0x0F), read only

Default 0x3F = 0011 1111. Fixed device identification value.
Use it as a presence / sanity check in the driver.

### 7.5 OFF_X (0x10), read/write

| Bit | 7 | 6 | 5 | 4 | 3 | 2 | 1 | 0 |
| --- | --- | --- | --- | --- | --- | --- | --- | --- |
| Name | OFFx_7 | OFFx_6 | OFFx_5 | OFFx_4 | OFFx_3 | OFFx_2 | OFFx_1 | OFFx_0 |

OFFx[7:0]: X-axis offset correction, signed value. Default 0x00.

### 7.6 OFF_Y (0x11), read/write

Same layout as OFF_X. OFFy[7:0]: Y-axis offset correction, signed. Default 0x00.

### 7.7 OFF_Z (0x12), read/write

Same layout as OFF_X. OFFz[7:0]: Z-axis offset correction, signed. Default 0x00.
Note: the datasheet does not specify the LSB weight of the OFF_ registers.

### 7.8 CS_X (0x13), read/write

| Bit | 7 | 6 | 5 | 4 | 3 | 2 | 1 | 0 |
| --- | --- | --- | --- | --- | --- | --- | --- | --- |
| Name | CS_7 | CS_6 | CS_5 | CS_4 | CS_3 | CS_2 | CS_1 | CS_0 |

CS[7:0]: constant shift, X-axis, signed. Used by the state machines only
(selected by D_CS bit in SETT2). Default 0x00.

### 7.9 CS_Y (0x14), read/write

CS[7:0]: constant shift, Y-axis, signed. State machine only. Default 0x00.

### 7.10 CS_Z (0x15), read/write

CS[7:0]: constant shift, Z-axis, signed. State machine only. Default 0x00.

### 7.11 LC_L (0x16), LC_H (0x17), read/write

16-bit long counter used for state machine program timing.
LC_L = low byte (register map default 0x01), LC_H = high byte (default 0x00).

Value meaning:
- 0x0001: counting stopped
- 0x0000: counter expired, interrupt available, counter reloaded to default
- values above 0x0001: counting in progress

The LONG flag in STAT (0x18) reports the long-counter interrupt (common to both
state machines).

### 7.12 STAT (0x18), read only

| Bit | 7 | 6 | 5 | 4 | 3 | 2 | 1 | 0 |
| --- | --- | --- | --- | --- | --- | --- | --- | --- |
| Name | LONG | SYNCW | SYNC1 | SYNC2 | INT_SM1 | INT_SM2 | DOR | DRDY |

- LONG: 0 = no interrupt; 1 = long counter (LC) interrupt flag, common for both SM
- SYNCW: synchronization for external host controller interrupt based on output data
  (0 = no action waiting from host; 1 = action from host based on output data)
- SYNC1: 0 = SM1 running normally; 1 = SM1 stopped, awaiting restart request from SM2
- SYNC2: 0 = SM2 running normally; 1 = SM2 stopped, awaiting restart request from SM1
- INT_SM1: SM1 interrupt flag (datasheet wording: 1 = SM1 interrupt enable,
  0 = SM1 interrupt disable)
- INT_SM2: SM2 interrupt flag (same wording as INT_SM1)
- DOR: data overrun, unread data in output registers when the next sample starts
  (0 = no overrun; 1 = overrun; bit is reset when the next sample is ready)
- DRDY: 0 = data not ready; 1 = data ready in output registers

### 7.13 PEAK1 (0x19), read only

| Bit | 7 | 6 | 5 | 4 | 3 | 2 | 1 | 0 |
| --- | --- | --- | --- | --- | --- | --- | --- | --- |
| Name | PKx_7 | PKx_6 | PKx_5 | PKx_4 | PKx_3 | PKx_2 | PKx_1 | PKx_0 |

PKx[7:0]: peak detected value for SM1 next condition. Default 0x00.
Peak detection is enabled by P_DET in SETT1.

### 7.14 PEAK2 (0x1A), read only

Same layout. PKx[7:0]: peak detected value for SM2. Default 0x00.
Peak detection is enabled by P_DET in SETT2.

### 7.15 VFC_1 (0x1B), read/write

VFC1[7:0]: vector filter coefficient 1 for the difference filter. Default 0x00.

### 7.16 VFC_2 (0x1C), read/write

VFC2[7:0]: vector filter coefficient 2 for the difference filter. Default 0x00.

### 7.17 VFC_3 (0x1D), read/write

VFC3[7:0]: vector filter coefficient 3 for the SM2 filter. Default 0x00.

### 7.18 VFC_4 (0x1E), read/write

VFC4[7:0]: vector filter coefficient 4 for the difference filter. Default 0x00.
The vector filter is enabled by the VFILT bit in CTRL_REG3.

### 7.19 THRS3 (0x1F), read/write

THRS3[7:0]: threshold value 3. Default 0x00.
Used as limit value for axis/sign mask reset, see THR3_SA and THR3_MA
in SETT1 / SETT2.

### 7.20 CTRL_REG4 (0x20), read/write, default 0x07

| Bit | 7 | 6 | 5 | 4 | 3 | 2 | 1 | 0 |
| --- | --- | --- | --- | --- | --- | --- | --- | --- |
| Name | ODR3 | ODR2 | ODR1 | ODR0 | BDU | Zen | Yen | Xen |

- ODR3..ODR0: output data rate and power mode selection, default 0000 (power down)
- BDU: block data update, default 0
  (0 = continuous update; 1 = output registers not updated until both MSB and LSB
  have been read). Use BDU = 1 to avoid mixing samples when reading byte by byte.
- Zen: Z-axis enable, default 1 (0 = disabled, 1 = enabled)
- Yen: Y-axis enable, default 1
- Xen: X-axis enable, default 1

ODR selection table:

| ODR3 | ODR2 | ODR1 | ODR0 | ODR |
| --- | --- | --- | --- | --- |
| 0 | 0 | 0 | 0 | Power down |
| 0 | 0 | 0 | 1 | 3.125 Hz |
| 0 | 0 | 1 | 0 | 6.25 Hz |
| 0 | 0 | 1 | 1 | 12.5 Hz |
| 0 | 1 | 0 | 0 | 25 Hz |
| 0 | 1 | 0 | 1 | 50 Hz |
| 0 | 1 | 1 | 0 | 100 Hz |
| 0 | 1 | 1 | 1 | 400 Hz |
| 1 | 0 | 0 | 0 | 800 Hz |
| 1 | 0 | 0 | 1 | 1600 Hz |

Codes 1010..1111 are not documented in the datasheet.

### 7.21 CTRL_REG1 (0x21), read/write, default 0x00 - SM1 control

| Bit | 7 | 6 | 5 | 4 | 3 | 2 | 1 | 0 |
| --- | --- | --- | --- | --- | --- | --- | --- | --- |
| Name | HYST2_1 | HYST1_1 | HYST0_1 | - | SM1_PIN | - | - | SM1_EN |

- HYST2_1..HYST0_1: unsigned hysteresis value added to or subtracted from the
  threshold value in SM1. Default 000. (LSB weight not specified in datasheet.)
- SM1_PIN: 0 = SM1 interrupt routed to INT1; 1 = SM1 interrupt routed to INT2.
  Default 0.
- SM1_EN: 0 = SM1 disabled; 1 = SM1 enabled. Default 0.
- Bits 4, 2, 1: reserved, keep 0.

### 7.22 CTRL_REG2 (0x22), read/write, default 0x00 - SM2 control

| Bit | 7 | 6 | 5 | 4 | 3 | 2 | 1 | 0 |
| --- | --- | --- | --- | --- | --- | --- | --- | --- |
| Name | HYST2_2 | HYST1_2 | HYST0_2 | - | SM2_PIN | - | - | SM2_EN |

- HYST2_2..HYST0_2: unsigned hysteresis added to or subtracted from the SM2
  threshold. Default 000.
- SM2_PIN: 0 = SM2 interrupt routed to INT1; 1 = routed to INT2.
  Default 0. (Datasheet text has a typo, it prints INT1 for both values;
  behaviour is symmetric to SM1_PIN.)
- SM2_EN: 0 = SM2 disabled; 1 = SM2 enabled. Default 0.
- Bits 4, 2, 1: reserved, keep 0.

### 7.23 CTRL_REG3 (0x23), read/write, default 0x00

| Bit | 7 | 6 | 5 | 4 | 3 | 2 | 1 | 0 |
| --- | --- | --- | --- | --- | --- | --- | --- | --- |
| Name | DR_EN | IEA | IEL | INT2_EN | INT1_EN | VFILT | - | STRT |

- DR_EN: DRDY signal connected to INT1. Default 0
  (0 = not connected; 1 = connected)
- IEA: interrupt polarity. Default 0
  (0 = interrupt signals active LOW; 1 = active HIGH)
- IEL: interrupt latching. Default 0
  (0 = latched interrupt; 1 = pulsed interrupt)
- INT2_EN: 0 = INT2 signal disabled; 1 = INT2 signal enabled. Default 0.
- INT1_EN: 0 = INT1/DRDY signal disabled; 1 = INT1/DRDY signal enabled. Default 0.
- VFILT: 0 = vector filter disabled; 1 = vector filter enabled. Default 0.
- STRT: soft reset bit (0 = no soft reset; 1 = soft reset, POR function).
- Bit 1: reserved.

### 7.24 CTRL_REG5 (0x24), read/write, default 0x00

| Bit | 7 | 6 | 5 | 4 | 3 | 2 | 1 | 0 |
| --- | --- | --- | --- | --- | --- | --- | --- | --- |
| Name | BW2 | BW1 | FSCALE2 | FSCALE1 | FSCALE0 | ST2 | ST1 | SIM |

- BW2..BW1: anti-aliasing filter bandwidth. Default 00.
  00 = 800 Hz; 01 = 200 Hz; 10 = 400 Hz; 11 = 50 Hz
- FSCALE2..FSCALE0: full-scale selection. Default 000.
  000 = +/-2 g; 001 = +/-4 g; 010 = +/-6 g; 011 = +/-8 g; 100 = +/-16 g
  (other codes not documented)
- ST2..ST1: self-test enable. Default 00.
- SIM: SPI serial interface mode. Default 0 (0 = 4-wire; 1 = 3-wire).

Self-test mode selection:

| ST2 | ST1 | Mode |
| --- | --- | --- |
| 0 | 0 | Normal mode |
| 0 | 1 | Positive sign self-test |
| 1 | 0 | Negative sign self-test |
| 1 | 1 | Not allowed |

### 7.25 CTRL_REG6 (0x25), read/write, default 0x10

| Bit | 7 | 6 | 5 | 4 | 3 | 2 | 1 | 0 |
| --- | --- | --- | --- | --- | --- | --- | --- | --- |
| Name | BOOT | FIFO_EN | WTM_EN | ADD_INC | P1_EMPTY | P1_WTM | P1_OVERRUN | P2_BOOT |

- BOOT: force reboot of the internal logic / trimming reload. Active high,
  cleared automatically when the reboot is finished.
- FIFO_EN: 0 = FIFO disabled; 1 = FIFO enabled. Default 0.
- WTM_EN: enable FIFO watermark level use. Default 0 (0 = disable, 1 = enable).
- ADD_INC: register address automatically incremented during multiple byte
  access (I2C or SPI). Default 1 (0 = disable, 1 = enable).
- P1_EMPTY: FIFO empty indication on INT1. Default 0.
- P1_WTM: FIFO watermark interrupt on INT1. Default 0.
- P1_OVERRUN: FIFO overrun interrupt on INT1. Default 0.
- P2_BOOT: BOOT interrupt on INT2. Default 0.

### 7.26 STATUS (0x27), read only

| Bit | 7 | 6 | 5 | 4 | 3 | 2 | 1 | 0 |
| --- | --- | --- | --- | --- | --- | --- | --- | --- |
| Name | ZYXOR | ZOR | YOR | XOR | ZYXDA | ZDA | YDA | XDA |

- ZYXOR: 1 = a new set of data has overwritten the previous set (X, Y, Z overrun)
- ZOR: 1 = new Z data has overwritten the previous one
- YOR: 1 = new Y data has overwritten the previous one
- XOR: 1 = new X data has overwritten the previous one
- ZYXDA: 1 = a new set of X, Y, Z data is available
- ZDA: 1 = new Z data available
- YDA: 1 = new Y data available
- XDA: 1 = new X data available
All default 0.

### 7.27 OUT_X_L (0x28), OUT_X_H (0x29), read only

OUT_X_L bits XD7..XD0 = X-axis output low byte.
OUT_X_H bits XD15..XD8 = X-axis output high byte.
Default 0x00. Value = signed 16-bit two's complement, little endian
(low byte at the lower address).

### 7.28 OUT_Y_L (0x2A), OUT_Y_H (0x2B), read only

YD7..YD0 low byte, YD15..YD8 high byte. Signed 16-bit, little endian. Default 0x00.

### 7.29 OUT_Z_L (0x2C), OUT_Z_H (0x2D), read only

ZD7..ZD0 low byte, ZD15..ZD8 high byte. Signed 16-bit, little endian. Default 0x00.

Reading: with BDU = 1 the registers hold their value until both bytes are read.
A 6-byte burst read starting at 0x28 returns XL, XH, YL, YH, ZL, ZH
(requires ADD_INC = 1).
When FIFO is enabled and in Bypass / FIFO / Stream mode, reads of OUT_X/Y/Z
return the oldest FIFO sample.

### 7.30 FIFO_CTRL (0x2E), read/write, default 0x00

| Bit | 7 | 6 | 5 | 4 | 3 | 2 | 1 | 0 |
| --- | --- | --- | --- | --- | --- | --- | --- | --- |
| Name | FMODE2 | FMODE1 | FMODE0 | WTMP4 | WTMP3 | WTMP2 | WTMP1 | WTMP0 |

- FMODE2..FMODE0: FIFO mode selection (table below)
- WTMP4..WTMP0: FIFO watermark pointer (FIFO depth if watermark is enabled)

FIFO trigger is the INT2 source.

| FMODE2 | FMODE1 | FMODE0 | Mode |
| --- | --- | --- | --- |
| 0 | 0 | 0 | Bypass mode, FIFO turned off |
| 0 | 0 | 1 | FIFO mode, stops collecting data when full |
| 0 | 1 | 0 | Stream mode, when full the new sample overwrites the oldest |
| 0 | 1 | 1 | Stream mode until trigger is de-asserted, then FIFO mode |
| 1 | 0 | 0 | Bypass mode until trigger is de-asserted, then Stream mode |
| 1 | 0 | 1 | Not used |
| 1 | 1 | 0 | Not used |
| 1 | 1 | 1 | Bypass mode until trigger is de-asserted, then FIFO mode |

### 7.31 FIFO_SRC (0x2F), read only

| Bit | 7 | 6 | 5 | 4 | 3 | 2 | 1 | 0 |
| --- | --- | --- | --- | --- | --- | --- | --- | --- |
| Name | WTM | OVRN_FIFO | EMPTY | FSS4 | FSS3 | FSS2 | FSS1 | FSS0 |

- WTM: watermark status. 0 = filling below watermark; 1 = filling at or above watermark
- OVRN_FIFO: overrun status. 0 = FIFO not completely filled; 1 = FIFO completely filled
- EMPTY: 0 = FIFO not empty; 1 = FIFO empty
- FSS4..FSS0: number of samples stored in FIFO (FIFO stored data level)

### 7.32 ST1_1 .. ST1_16 (0x40 - 0x4F), write only

State machine 1 code registers: 16 registers of 8 bits, one per program step
(16-step op-code). The op-code encoding is not described in this datasheet
(see ST application note on LIS3DSH state machine programming, AN3393).

### 7.33 TIM4_1 (0x50), write only

TM[7:0]: 8-bit unsigned general timer for SM1 operation timing. Default 0x00.

### 7.34 TIM3_1 (0x51), write only

TM[7:0]: 8-bit unsigned general timer for SM1 operation timing. Default 0x00.

### 7.35 TIM2_1 (0x52 low, 0x53 high), write only

TM[15:0]: 16-bit unsigned general timer for SM1. Default 0x0000.
0x52 = TM7..TM0, 0x53 = TM15..TM8.

### 7.36 TIM1_1 (0x54 low, 0x55 high), write only

TM[15:0]: 16-bit unsigned general timer for SM1. Default 0x0000.
0x54 = TM7..TM0, 0x55 = TM15..TM8.

### 7.37 THRS2_1 (0x56), write only

THS[7:0]: threshold value for SM1. Default 0x00.

### 7.38 THRS1_1 (0x57), write only

THS[7:0]: threshold value for SM1. Default 0x00.
Thresholds are unsigned or signed depending on the ABS bit in SETT1.

### 7.39 MASK1_B (0x59), write only - axis and sign mask (swap) for SM1

| Bit | 7 | 6 | 5 | 4 | 3 | 2 | 1 | 0 |
| --- | --- | --- | --- | --- | --- | --- | --- | --- |
| Name | P_X | N_X | P_Y | N_Y | P_Z | N_Z | P_V | N_V |

- P_X: 0 = X+ disabled; 1 = X+ enabled
- N_X: 0 = X- disabled; 1 = X- enabled
- P_Y: 0 = Y+ disabled; 1 = Y+ enabled
- N_Y: 0 = Y- disabled; 1 = Y- enabled
- P_Z: 0 = Z+ disabled; 1 = Z+ enabled
- N_Z: 0 = Z- disabled; 1 = Z- enabled
- P_V: 0 = V+ disabled; 1 = V+ enabled (V = vector / magnitude)
- N_V: 0 = V- disabled; 1 = V- enabled

### 7.40 MASK1_A (0x5A), write only - axis and sign mask (default) for SM1

Identical bit layout and meaning as MASK1_B:
P_X, N_X, P_Y, N_Y, P_Z, N_Z, P_V, N_V (1 = enabled, 0 = disabled).

### 7.41 SETT1 (0x5B), write only - SM1 detection settings

| Bit | 7 | 6 | 5 | 4 | 3 | 2 | 1 | 0 |
| --- | --- | --- | --- | --- | --- | --- | --- | --- |
| Name | P_DET | THR3_SA | ABS | - | - | THR3_MA | R_TAM | SITR |

- P_DET: SM1 peak detection. 0 = disabled; 1 = enabled. Default 0.
- THR3_SA: 0 = no action; 1 = threshold 3 (THRS3) is the limit value for
  axis and sign mask reset of MASK1_B. Default 0.
- ABS: 0 = unsigned thresholds; 1 = signed thresholds. Default 0.
- THR3_MA: 0 = no action; 1 = threshold 3 (THRS3) is the limit value for
  axis and sign mask reset of MASK1_A. Default 0.
- R_TAM: next condition validation flag. 0 = no valid next condition found;
  1 = valid next condition found and reset. Default 0.
- SITR: 0 = no action; 1 = program flow can be modified by STOP and CONT commands.
  Default 0.
- Bits 4 and 3: reserved.

### 7.42 PR1 (0x5C), read only - SM1 program and reset pointer

| Bit | 7 | 6 | 5 | 4 | 3 | 2 | 1 | 0 |
| --- | --- | --- | --- | --- | --- | --- | --- | --- |
| Name | RP3 | RP2 | RP1 | RP0 | PP3 | PP2 | PP1 | PP0 |

- RP3..RP0: SM1 reset pointer address
- PP3..PP0: SM1 program pointer address

### 7.43 TC1 (0x5D low, 0x5E high), read only

TC1[15:0]: 16-bit unsigned general timer output value for SM1 timing.
Default 0x0000. 0x5D = TC1_7..TC1_0, 0x5E = TC1_15..TC1_8.

### 7.44 OUTS1 (0x5F), read only - SM1 output flags

| Bit | 7 | 6 | 5 | 4 | 3 | 2 | 1 | 0 |
| --- | --- | --- | --- | --- | --- | --- | --- | --- |
| Name | P_X | N_X | P_Y | N_Y | P_Z | N_Z | P_V | N_V |

Each bit: 0 = condition not shown; 1 = condition shown
(X+, X-, Y+, Y-, Z+, Z-, V+, V-).
Reading this register affects the SM1 interrupt function depending on the flags
(used to clear / acknowledge the SM1 interrupt).

### 7.45 ST2_1 .. ST2_16 (0x60 - 0x6F), write only

State machine 2 code registers: 16 x 8-bit, 16-step op-code.
Op-code encoding is not part of this datasheet.

### 7.46 TIM4_2 (0x70), write only

TM[7:0]: 8-bit unsigned general timer for SM2. Default 0x00.

### 7.47 TIM3_2 (0x71), write only

TM[7:0]: 8-bit unsigned general timer for SM2. Default 0x00.

### 7.48 TIM2_2 (0x72 low, 0x73 high), write only

TM[15:0]: 16-bit unsigned general timer for SM2. Default 0x0000.

### 7.49 TIM1_2 (0x74 low, 0x75 high), write only

TM[15:0]: 16-bit unsigned general timer for SM2. Default 0x0000.

### 7.50 THRS2_2 (0x76), write only

THS[7:0]: threshold value for SM2 (signed or unsigned per ABS in SETT2).
Default 0x00.

### 7.51 THRS1_2 (0x77), write only

THS[7:0]: threshold value for SM2. Default 0x00.

### 7.52 DES2 (0x78), write only

D[7:0]: decimation counter value for SM2 operation. Default 0x00.
Allows SM2 to run on decimated time instead of the full ODR.

### 7.53 MASK2_B (0x79), write only - axis and sign mask (swap) for SM2

| Bit | 7 | 6 | 5 | 4 | 3 | 2 | 1 | 0 |
| --- | --- | --- | --- | --- | --- | --- | --- | --- |
| Name | P_X | N_X | P_Y | N_Y | P_Z | N_Z | P_V | N_V |

1 = enabled, 0 = disabled, for X+, X-, Y+, Y-, Z+, Z-, V+, V-.

### 7.54 MASK2_A (0x7A), write only - axis and sign mask (default) for SM2

Identical layout and meaning as MASK2_B.

### 7.55 SETT2 (0x7B), write only - SM2 detection settings

| Bit | 7 | 6 | 5 | 4 | 3 | 2 | 1 | 0 |
| --- | --- | --- | --- | --- | --- | --- | --- | --- |
| Name | P_DET | THR3_SA | ABS | RADI | D_CS | THR3_MA | R_TAM | SITR |

- P_DET: SM2 peak detection. 0 = disabled; 1 = enabled. Default 0.
- THR3_SA: 0 = no action; 1 = threshold 3 limit value for axis and sign mask
  reset (MASK2_B). Default 0.
- ABS: 0 = unsigned thresholds; 1 = signed thresholds. Default 0.
- RADI: 0 = raw data for state machine 2; 1 = diff (filtered) data. Default 0.
- D_CS: 0 = DIFF2 enabled (difference between current data and previous data);
  1 = constant shift enabled (difference between current data and the
  constant values CS_X / CS_Y / CS_Z). Default 0.
- THR3_MA: 0 = no action; 1 = threshold 3 limit value for axis and sign mask
  reset (MASK2_A). Default 0.
- R_TAM: next condition validation flag. 0 = no valid next condition found;
  1 = valid next condition found and reset. Default 0.
- SITR: 0 = no action; 1 = program flow can be modified by STOP and CONT commands.
  Default 0.

### 7.56 PR2 (0x7C), read only - SM2 program and reset pointer

| Bit | 7 | 6 | 5 | 4 | 3 | 2 | 1 | 0 |
| --- | --- | --- | --- | --- | --- | --- | --- | --- |
| Name | RP3 | RP2 | RP1 | RP0 | PP3 | PP2 | PP1 | PP0 |

- RP3..RP0: SM2 reset pointer address
- PP3..PP0: SM2 program pointer address

### 7.57 TC2 (0x7D low, 0x7E high), read only

TC2[15:0]: 16-bit unsigned general timer output value for SM2. Default 0x0000.

### 7.58 OUTS2 (0x7F), read only - SM2 output flags

| Bit | 7 | 6 | 5 | 4 | 3 | 2 | 1 | 0 |
| --- | --- | --- | --- | --- | --- | --- | --- | --- |
| Name | P_X | N_X | P_Y | N_Y | P_Z | N_Z | P_V | N_V |

Each bit: 0 = condition not shown; 1 = condition shown.
Reading this register affects the SM2 interrupt function depending on the flags.

## 8. FIFO operation

The device has a FIFO for each of the three output channels X, Y, Z
(32 sample slots). It reduces host polling: the host can sleep and burst-read
data later. Enable with FIFO_EN (CTRL_REG6), select mode with FMODE (FIFO_CTRL).

Modes:
- Bypass: FIFO off, stays empty, only the first address of each channel is used.
  Output registers behave as normal single-sample registers.
- FIFO: X, Y, Z samples are stored. Watermark interrupt when the programmed level
  is reached. Stops collecting when full.
- Stream: same, but when full the oldest data is discarded as new data arrives.
- Stream-to-FIFO: runs in Stream mode; when a trigger event occurs it switches to
  FIFO mode (keeps the data around the event). Trigger source is the INT2 signal.
- Additional combined modes: Bypass-to-Stream and Bypass-to-FIFO (see FMODE table).

Watermark: WTM_EN (CTRL_REG6) enables use of the WTMP level in FIFO_CTRL.
Interrupt sources routable to INT1: FIFO empty (P1_EMPTY), watermark (P1_WTM),
overrun (P1_OVERRUN). BOOT interrupt can be routed to INT2 (P2_BOOT).
Status is read from FIFO_SRC: WTM, OVRN_FIFO, EMPTY, FSS[4:0].

Data retrieval: read OUT_X, OUT_Y, OUT_Z (0x28..0x2D). In Bypass, FIFO or Stream
mode each read returns the oldest stored X, Y, Z sample; after the read the next
oldest sample is loaded into the output registers. Single reads and burst reads
are both supported. To resynchronize / clear the FIFO, set Bypass mode and then
re-select the desired mode.

## 9. State machines (functional summary)

Two independent state machines (SM1, SM2), each running a user program of up to
16 steps loaded into ST1_x (0x40..0x4F) / ST2_x (0x60..0x6F).
From each state n the machine can branch to state n+1 (NEXT condition true) or to
the reset state (RESET condition true). Reaching an output / stop / continue state
raises an interrupt.

Supported algorithms: gesture recognition, free-fall, wake-up, 4D/6D orientation,
pulse and step counting, click / double-click, shake / double-shake,
face-up / face-down, turn / double-turn.

Program resources per SM:
- thresholds THRS1_x, THRS2_x (plus shared THRS3)
- hysteresis HYST bits in CTRL_REG1 / CTRL_REG2
- axis / sign masks MASKx_A (default) and MASKx_B (swap)
- settings SETT1 / SETT2 (peak detection, signed/unsigned, mask reset, flow control)
- timers TIM1_x, TIM2_x (16-bit), TIM3_x, TIM4_x (8-bit), outputs TC1 / TC2
- long counter LC (shared), decimation DES2 (SM2 only)
- data source selection for SM2: raw or diff (RADI), diff vs constant shift (D_CS)
- status: STAT (SYNC1, SYNC2, SYNCW, INT_SM1, INT_SM2, LONG), PR1 / PR2 pointers,
  OUTS1 / OUTS2 flags, PEAK1 / PEAK2

Enable with SM1_EN / SM2_EN; route interrupts with SM1_PIN / SM2_PIN and
INT1_EN / INT2_EN in CTRL_REG3.
Note: the instruction op-code set is NOT in this datasheet; it is documented in
the ST application note for LIS3DSH state machine programming (AN3393).

## 10. Driver implementation notes (derived, not literal datasheet text)

Register access helpers:
- SPI read: CS low, send (0x80 | addr), read N bytes, CS high.
- SPI write: CS low, send (addr & 0x7F), write N bytes, CS high.
- Multi-byte transfers need ADD_INC = 1 (CTRL_REG6 bit 4, default is 1).
- SPI mode 3 (CPOL=1, CPHA=1), MSB first, max 10 MHz.
- I2C: 7-bit address 0x1D (SEL high) or 0x1E (SEL low); write reg then repeated
  start for read; CS pin must be high.

Typical init sequence:
1. Optional soft reset: CTRL_REG3 (0x23) STRT = 1, or CTRL_REG6 (0x25) BOOT = 1;
   wait for boot completion (BOOT self-clears), then reconfigure.
2. Read WHO_AM_I (0x0F), expect 0x3F.
3. CTRL_REG6 (0x25) = 0x10 (ADD_INC = 1, FIFO off) - this is the default.
4. CTRL_REG5 (0x24): set full scale and anti-aliasing bandwidth.
   Example +/-2 g with 50 Hz AA filter: 0xC0. +/-2 g with 800 Hz BW: 0x00.
   Choose bandwidth <= ODR/2 (800 / 400 / 200 / 50 Hz options).
5. CTRL_REG4 (0x20): ODR + BDU + axis enable.
   Example 100 Hz, BDU = 1, XYZ enabled: 0x67.
   Example 50 Hz, BDU = 1, XYZ enabled: 0x57.
   Example 1.6 kHz, BDU = 1, XYZ enabled: 0x97.
   Power down: ODR = 0000 (e.g. 0x07).
6. Optional interrupts: CTRL_REG3 (0x23).
   DRDY on INT1, active high, pulsed, INT1 enabled: DR_EN|IEA|INT1_EN = 0xC8.
   Latched active low INT1 with SM1: INT1_EN = 0x08, IEA = 0, IEL = 0.
   SM1 to INT2: CTRL_REG1 SM1_PIN = 1 and CTRL_REG3 INT2_EN = 1 (0x10).
7. Wait for the first sample: poll STATUS (0x27) ZYXDA, or STAT (0x18) DRDY,
   or use the DRDY interrupt.
8. Burst read 6 bytes from 0x28: XL, XH, YL, YH, ZL, ZH.
   raw_x = (int16_t)((XH << 8) | XL), same for Y and Z.
   mg = raw * sensitivity (0.06 / 0.12 / 0.18 / 0.24 / 0.73 for 2/4/6/8/16 g).
   g = mg / 1000.
9. Temperature: read OUT_T (0x0C) as int8, 1 LSB per degree (relative value).

Self-test procedure (per datasheet):
1. Configure a stable ODR and +/-2 g, BDU = 1, wait for settling.
2. Average N samples with ST2:ST1 = 00 (normal).
3. Set ST2:ST1 = 01 (positive self-test), wait for settling, average N samples.
4. Difference (ST - normal) should be around 140 mg on X and Y, 590 mg on Z
   (typical values, not guaranteed limits).
5. Return ST2:ST1 = 00. ST2:ST1 = 11 is not allowed.

FIFO usage sketch:
1. FIFO_CTRL (0x2E) = (FMODE << 5) | watermark_level (level 0..31).
2. CTRL_REG6 (0x25): FIFO_EN = 1 (0x40), WTM_EN = 1 (0x20), optional P1_WTM (0x04).
   Example FIFO mode, watermark 16, INT1 watermark: FIFO_CTRL = 0x30 (FMODE=001,
   WTMP=16), CTRL_REG6 = 0x74 (FIFO_EN|WTM_EN|ADD_INC|P1_WTM).
3. On interrupt read FIFO_SRC (0x2F) for FSS, then read that many 6-byte samples
   from 0x28.
4. To flush: set FMODE = 000 (Bypass), then set the desired mode again.

## 11. Datasheet ambiguities / typos to be aware of

- CTRL_REG2 SM2_PIN description prints "INT1" for both values; correct behaviour
  is 0 = INT1, 1 = INT2 (symmetric to SM1_PIN).
- CTRL_REG3 INT1_EN description is labelled "Interrupt 2 enable/disable";
  it actually enables the INT1/DRDY signal.
- CTRL_REG3 IEL: 0 = latched, 1 = pulsed (note this is inverted compared to some
  other ST sensors).
- STAT (0x18) INT_SM1 / INT_SM2 are described with "enable/disable" wording but
  are status flags of the state machine interrupts.
- LC_L default: register map says 0x01, section 7.11 table says 0x00.
  Use 0x0001 as the "counting stopped" default.
- Register map comments swap THRS1_1 / THRS2_1 numbering; both are SM1 thresholds.
- OFF_X/Y/Z, CS_X/Y/Z, THRSx, HYST LSB weights are not specified in the datasheet.
- OUT_T has no specified absolute offset; treat as a relative temperature.
- Addresses not present in the map are reserved; writing them may permanently
  damage the device. Factory calibration registers loaded at boot must not be
  changed (they are restored at power-up).
- ODR codes 1010..1111 and FSCALE codes 101..111 are not documented; do not use.