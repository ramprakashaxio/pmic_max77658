// ProtoCentral Electronics (ashwin@protocentral.com)
// SPDX-License-Identifier: Apache-2.0

#define DT_DRV_COMPAT maxim_max30208

#include <zephyr/logging/log.h>
#include <zephyr/pm/device.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/byteorder.h> /* sys_be16_to_cpu */
#include <zephyr/sys/util.h>      /* sign_extend32 */

#include "max30208.h"

LOG_MODULE_REGISTER(MAX30208, CONFIG_SENSOR_LOG_LEVEL);

static int max30208_get_chip_id(const struct device *dev, uint8_t *id)
{
	const struct max30208_config *cfg = dev->config;
	uint8_t v = 0;
	int ret = i2c_reg_read_byte_dt(&cfg->i2c, MAX30208_REG_CHIP_ID, &v);
	if (ret < 0) {
		LOG_ERR("Failed to read chip ID: %d", ret);
		return ret;
	}
	*id = v;
	return 0;
}

static int max30208_config_fifo_rollover(const struct device *dev)
{
	const struct max30208_config *cfg = dev->config;
	uint8_t reg = 0;
	int ret = i2c_reg_read_byte_dt(&cfg->i2c, MAX30208_REG_FIFO_CONFIG2, &reg);
	if (ret < 0) return ret;

	reg |= MAX30208_FIFO_CONFIG_RO;
	ret = i2c_reg_write_byte_dt(&cfg->i2c, MAX30208_REG_FIFO_CONFIG2, reg);
	if (ret < 0) return ret;

	return 0;
}

static int max30208_soft_reset(const struct device *dev)
{
	const struct max30208_config *cfg = dev->config;
	int ret = i2c_reg_write_byte_dt(&cfg->i2c, MAX30208_REG_SYSTEM_CTRL, MAX30208_SYSTEM_CTRL_RESET);
	if (ret < 0) return ret;
	k_msleep(50);
	return 0;
}

static int max30208_read_temp_raw(const struct device *dev, int16_t *raw)
{
	const struct max30208_config *cfg = dev->config;
	uint8_t reg;
	int ret;
	int retries = 10;

	/* Trigger conversion */
	ret = i2c_reg_read_byte_dt(&cfg->i2c, MAX30208_REG_TEMP_SENSOR_SETUP, &reg);
	if (ret < 0) return ret;

	reg |= MAX30208_TEMP_SENSOR_SETUP_CONV;
	ret = i2c_reg_write_byte_dt(&cfg->i2c, MAX30208_REG_TEMP_SENSOR_SETUP, reg);
	if (ret < 0) return ret;

	/* Wait for TEMP_RDY */
	while (retries--) {
		ret = i2c_reg_read_byte_dt(&cfg->i2c, MAX30208_REG_STATUS, &reg);
		if (ret < 0) return ret;

		if (reg & MAX30208_STATUS_TEMP_RDY) break;
		k_msleep(MAX30208_CONV_TIME_MS);
	}

	if (retries < 0) {
		LOG_ERR("Temperature conversion timeout");
		return -ETIMEDOUT;
	}

	/* Determine how many FIFO samples to drain (Linux logic) */
	uint8_t ovf = 0, count = 0;

	ret = i2c_reg_read_byte_dt(&cfg->i2c, MAX30208_REG_FIFO_OVF_CNTR, &ovf);
	if (ret < 0) return ret;

	if (ovf) {
		count = 1;
	} else {
		ret = i2c_reg_read_byte_dt(&cfg->i2c, MAX30208_REG_FIFO_DATA_CNTR, &count);
		if (ret < 0) return ret;

		/* Safety: if counter reads 0 but TEMP_RDY is set, still read 1 sample */
		if (count == 0) count = 1;
	}

	uint16_t sample = 0;
	while (count--) {
		ret = i2c_burst_read_dt(&cfg->i2c, MAX30208_REG_FIFO_DATA,
		                        (uint8_t *)&sample, sizeof(sample));
		if (ret < 0) return ret;

		sample = sys_be16_to_cpu(sample);
	}

	/* Sign-extend 16-bit temperature */
	*raw = (int16_t)sign_extend(sample, 15);
	return 0;
}

static int max30208_sample_fetch(const struct device *dev, enum sensor_channel chan)
{
	struct max30208_data *data = dev->data;

	if (chan != SENSOR_CHAN_ALL && chan != SENSOR_CHAN_AMBIENT_TEMP) {
		return -ENOTSUP;
	}

	return max30208_read_temp_raw(dev, &data->temp_raw);
}

static int max30208_channel_get(const struct device *dev,
                                enum sensor_channel chan,
                                struct sensor_value *val)
{
	const struct max30208_data *data = dev->data;

	if (chan != SENSOR_CHAN_AMBIENT_TEMP) {
		return -ENOTSUP;
	}

	/* temp_uC = raw * 5000 (0.005°C/LSB) */
	int64_t temp_uC = (int64_t)data->temp_raw * (int64_t)MAX30208_UC_PER_LSB;

	val->val1 = (int32_t)(temp_uC / 1000000LL);
	val->val2 = (int32_t)(temp_uC % 1000000LL);
	return 0;
}

static const struct sensor_driver_api max30208_driver_api = {
	.sample_fetch = max30208_sample_fetch,
	.channel_get  = max30208_channel_get,
};

static int max30208_init(const struct device *dev)
{
	const struct max30208_config *cfg = dev->config;
	uint8_t chip_id = 0;
	int ret;

	if (!device_is_ready(cfg->i2c.bus)) {
		LOG_ERR("I2C bus not ready");
		return -ENODEV;
	}

	ret = max30208_get_chip_id(dev, &chip_id);
	if (ret < 0) return ret;

	if (chip_id != MAX30208_CHIP_ID) {
		LOG_ERR("Invalid chip id: 0x%02X (expected 0x%02X)", chip_id, MAX30208_CHIP_ID);
		return -ENODEV;
	}

	/* Match Linux driver behavior */
	ret = max30208_soft_reset(dev);
	if (ret < 0) {
		LOG_ERR("Reset failed: %d", ret);
		return ret;
	}

	ret = max30208_config_fifo_rollover(dev);
	if (ret < 0) {
		LOG_ERR("FIFO config failed: %d", ret);
		return ret;
	}

	LOG_INF("MAX30208 temperature sensor initialized successfully");
	return 0;
}

#ifdef CONFIG_PM_DEVICE
static int max30208_pm_action(const struct device *dev, enum pm_device_action action)
{
	ARG_UNUSED(dev);
	switch (action) {
	case PM_DEVICE_ACTION_RESUME:
	case PM_DEVICE_ACTION_SUSPEND:
		return 0;
	default:
		return -ENOTSUP;
	}
}
#endif

#define MAX30208_DEFINE(inst)                                    \
	static struct max30208_data max30208_data_##inst;            \
	static const struct max30208_config max30208_config_##inst = \
		{ .i2c = I2C_DT_SPEC_INST_GET(inst), };                   \
	PM_DEVICE_DT_INST_DEFINE(inst, max30208_pm_action);          \
	SENSOR_DEVICE_DT_INST_DEFINE(inst,                           \
	                             max30208_init,                  \
	                             PM_DEVICE_DT_INST_GET(inst),    \
	                             &max30208_data_##inst,          \
	                             &max30208_config_##inst,        \
	                             POST_KERNEL,                    \
	                             CONFIG_SENSOR_INIT_PRIORITY,    \
	                             &max30208_driver_api)

DT_INST_FOREACH_STATUS_OKAY(MAX30208_DEFINE);