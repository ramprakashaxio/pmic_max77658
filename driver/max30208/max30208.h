/*
 * Copyright (c) 2017, NXP
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef ZEPHYR_DRIVERS_SENSOR_MAX30208_MAX30208_H_
#define ZEPHYR_DRIVERS_SENSOR_MAX30208_MAX30208_H_

#include <zephyr/drivers/sensor.h>
#include <zephyr/drivers/i2c.h>

/* Register Definitions */
#define MAX30208_REG_STATUS             0x00
#define MAX30208_STATUS_TEMP_RDY        BIT(0)

#define MAX30208_REG_INT_ENABLE         0x01
#define MAX30208_REG_FIFO_WR_PTR        0x04
#define MAX30208_REG_FIFO_RD_PTR        0x05
#define MAX30208_REG_FIFO_OVF_CNTR      0x06
#define MAX30208_REG_FIFO_DATA_CNTR     0x07
#define MAX30208_REG_FIFO_DATA          0x08
#define MAX30208_REG_FIFO_CONFIG1       0x09
#define MAX30208_REG_FIFO_CONFIG2       0x0A
#define MAX30208_FIFO_CONFIG_RO         BIT(1)

#define MAX30208_REG_SYSTEM_CTRL        0x0C
#define MAX30208_SYSTEM_CTRL_RESET      0x01

#define MAX30208_REG_ALARM_HIGH_MSB     0x10
#define MAX30208_REG_ALARM_HIGH_LSB     0x11
#define MAX30208_REG_ALARM_LOW_MSB      0x12
#define MAX30208_REG_ALARM_LOW_LSB      0x13

#define MAX30208_REG_TEMP_SENSOR_SETUP  0x14
#define MAX30208_TEMP_SENSOR_SETUP_CONV BIT(0)

#define MAX30208_REG_GPIO_SETUP         0x20
#define MAX30208_REG_GPIO_CTRL          0x21

#define MAX30208_REG_TEMP_DATA_MSB      0x31
#define MAX30208_REG_TEMP_DATA_LSB      0x32

#define MAX30208_REG_CHIP_ID            0xFF
#define MAX30208_CHIP_ID                0x30

/* Conversion time (ms) */
#define MAX30208_CONV_TIME_MS           50

/* MAX30208 temperature scale: 0.005°C per LSB */
#define MAX30208_UC_PER_LSB             5000  /* 5000 micro-°C = 0.005°C */

struct max30208_config {
	struct i2c_dt_spec i2c;
};

struct max30208_data {
	int16_t temp_raw;
	float temperature;
};

#endif /* ZEPHYR_DRIVERS_SENSOR_MAX30208_MAX30208_H_ */