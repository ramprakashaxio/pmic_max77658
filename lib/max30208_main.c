#include "max30208_main.h"
#include "data_manager.h"

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/init.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/sensor.h>
#include <zephyr/sys/util.h>
#include <limits.h>

LOG_MODULE_REGISTER(max30208_app, LOG_LEVEL_INF);

/* MAX30208 GPIO0 -> P1.08
 * MAX30208 GPIO1 -> P1.09
 * Force both LOW => I2C address = 0x50
 */
#define MAX30208_ADDR_PORT_NODE DT_NODELABEL(gpio1)
#define MAX30208_ADDR_GPIO0_PIN 8
#define MAX30208_ADDR_GPIO1_PIN 9

/* MAX30208 RAW scale: 0.005°C / LSB
 * raw = temp_C / 0.005 = temp_C * 200
 */
#define MAX30208_UC_PER_LSB 5000  /* 0.005°C = 5000 micro°C */

static const struct device *s_max30208;

static int max30208_force_addr_pins_low(void)
{
#if DT_NODE_HAS_STATUS(MAX30208_ADDR_PORT_NODE, okay)
	const struct device *gpio1 = DEVICE_DT_GET(MAX30208_ADDR_PORT_NODE);

	if (!device_is_ready(gpio1)) {
		LOG_WRN("GPIO1 not ready yet; address pins may not be forced");
		return 0;
	}

	(void)gpio_pin_configure(gpio1, MAX30208_ADDR_GPIO0_PIN, GPIO_OUTPUT_LOW);
	(void)gpio_pin_configure(gpio1, MAX30208_ADDR_GPIO1_PIN, GPIO_OUTPUT_LOW);
	k_busy_wait(50);
#else
	LOG_WRN("gpio1 node not found/okay; cannot force MAX30208 addr pins");
#endif
	return 0;
}

/* Run very early */
SYS_INIT(max30208_force_addr_pins_low, POST_KERNEL, 1);

const struct device *max30208_app_dev(void)
{
	return s_max30208;
}

int max30208_app_init(void)
{
#if !DT_HAS_COMPAT_STATUS_OKAY(maxim_max30208)
	LOG_ERR("No devicetree node: compatible 'maxim,max30208' status okay");
	return -ENODEV;
#else
	(void)max30208_force_addr_pins_low();

	s_max30208 = DEVICE_DT_GET_ONE(maxim_max30208);
	if (!s_max30208) {
		LOG_ERR("DEVICE_DT_GET_ONE(maxim_max30208) returned NULL");
		return -ENODEV;
	}

	if (!device_is_ready(s_max30208)) {
		LOG_ERR("MAX30208 device not ready (driver init likely failed)");
		return -ENODEV;
	}

	/* Quick comms test */
	int ret = sensor_sample_fetch_chan(s_max30208, SENSOR_CHAN_AMBIENT_TEMP);
	if (ret < 0) {
		LOG_ERR("MAX30208 sample_fetch failed: %d", ret);
		return ret;
	}

	LOG_INF("MAX30208 app init OK (address forced to 0x50 via P1.08/P1.09 LOW)");
	return 0;
#endif
}

int max30208_app_read_c_x100(int32_t *temp_c_x100, int16_t *raw_out_optional)
{
	if (!temp_c_x100) {
		data_manager_update_temp(INT16_MIN, false);
		return -EINVAL;
	}
	if (!s_max30208) {
		data_manager_update_temp(INT16_MIN, false);
		return -ENODEV;
	}

	(void)max30208_force_addr_pins_low();

	int ret = sensor_sample_fetch_chan(s_max30208, SENSOR_CHAN_AMBIENT_TEMP);
	if (ret < 0) {
		LOG_ERR("sample_fetch failed: %d", ret);
		data_manager_update_temp(INT16_MIN, false);
		return ret;
	}

	struct sensor_value v;
	ret = sensor_channel_get(s_max30208, SENSOR_CHAN_AMBIENT_TEMP, &v);
	if (ret < 0) {
		LOG_ERR("channel_get failed: %d", ret);
		data_manager_update_temp(INT16_MIN, false);
		return ret;
	}

	/* v.val1 = integer degC, v.val2 = micro-degC */
	*temp_c_x100 = (int32_t)(v.val1 * 100) + (int32_t)(v.val2 / 10000);

	if (raw_out_optional) {
		/* Convert temp -> raw using 0.005°C/LSB */
		int64_t temp_uC = ((int64_t)v.val1 * 1000000LL) + (int64_t)v.val2;
data_manager_update_temp((int16_t)(*temp_c_x100), true);

	
		/* rounding to nearest */
		if (temp_uC >= 0) temp_uC += (MAX30208_UC_PER_LSB / 2);
		else              temp_uC -= (MAX30208_UC_PER_LSB / 2);

		*raw_out_optional = (int16_t)(temp_uC / MAX30208_UC_PER_LSB);
	}

	return 0;
}

int max30208_app_read_raw(int16_t *raw_out)
{
	if (!raw_out) return -EINVAL;

	int32_t t_x100 = 0;
	return max30208_app_read_c_x100(&t_x100, raw_out);
}
