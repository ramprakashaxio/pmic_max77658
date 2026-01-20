#ifndef MAX30208_MAIN_H_
#define MAX30208_MAIN_H_

#include <zephyr/device.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Initialize MAX30208 application wrapper.
 *        Ensures MAX30208 device is ready (driver init succeeded).
 *
 * @return 0 on success, negative errno on failure
 */
int max30208_app_init(void);

/**
 * @brief Read raw temperature sample (LSB = 0.005°C).
 *
 * raw = temp_C / 0.005 = temp_C * 200
 *
 * @param raw_out pointer to receive raw sample
 * @return 0 on success, negative errno on failure
 */
int max30208_app_read_raw(int16_t *raw_out);

/**
 * @brief Read temperature in centi-degC (°C * 100).
 *
 * @param temp_c_x100 output temperature in °C*100
 * @param raw_out_optional optional raw output (can be NULL)
 * @return 0 on success, negative errno on failure
 */
int max30208_app_read_c_x100(int32_t *temp_c_x100, int16_t *raw_out_optional);

/**
 * @brief Get Zephyr device pointer for MAX30208 (NULL if not initialized).
 */
const struct device *max30208_app_dev(void);

#ifdef __cplusplus
}
#endif

#endif /* MAX30208_MAIN_H_ */
