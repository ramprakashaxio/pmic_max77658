/*
 * main.c - Application Entry Point
 */

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/device.h>
#include <zephyr/drivers/i2c.h>
#include <zephyr/sys/util.h>
#include <stdlib.h>

/* Application Libraries */
#include "max77658_main.h"
#include "max32664c_main.h"
#include "max30208_main.h"
#include "lsm6dsv32x_main.h"
#include "app_i2c_lock.h"

/* BLE Store-and-Forward Thread */
void start_ble_thread(void);
K_THREAD_DEFINE(ble_thread_id, 4096, start_ble_thread, NULL, NULL, NULL, 7, 0, 0);

LOG_MODULE_REGISTER(main, CONFIG_LOG_DEFAULT_LEVEL);

/* I2C Device */
#define I2C_NODE DT_NODELABEL(i2c21)
static const struct device *i2c_dev = DEVICE_DT_GET(I2C_NODE);

/* I2C Scanner Function */
static void i2c_scan(void)
{
    uint8_t addr;
    uint8_t cnt = 0;

    LOG_INF("--- I2C Bus Scan ---");
    
    if (!device_is_ready(i2c_dev)) {
        LOG_ERR("I2C device not ready!");
        return;
    }

    k_mutex_lock(&i2c_lock, K_FOREVER);

    for (addr = 0x03; addr <= 0x77; addr++) {
        struct i2c_msg msgs[1];
        uint8_t dst;

        msgs[0].buf = &dst;
        msgs[0].len = 0;
        msgs[0].flags = I2C_MSG_WRITE | I2C_MSG_STOP;

        if (i2c_transfer(i2c_dev, &msgs[0], 1, addr) == 0) {
            LOG_INF("Device found at 0x%02X", addr);
            cnt++;
        }
    }

    k_mutex_unlock(&i2c_lock);
    
    LOG_INF("Found %d device(s) on I2C bus", cnt);
}

int main(void)
{
    int ret;
    
    LOG_INF("--- System Startup ---");

    /* 1. Initialize PMIC Hardware (Protected) */
    LOG_INF("Initializing PMIC...");
    k_mutex_lock(&i2c_lock, K_FOREVER);
    ret = max77658_app_init();
    k_mutex_unlock(&i2c_lock);
    
    if (ret != 0) {
        LOG_ERR("MAX77658 Init Failed! Halting.");
        while (1) { k_msleep(1000); }
    }

    /* 2. Initialize MAX30208 Temperature Sensor */
    LOG_INF("Initializing MAX30208...");
    k_mutex_lock(&i2c_lock, K_FOREVER);
    ret = max30208_app_init();
    k_mutex_unlock(&i2c_lock);
    
    if (ret != 0) {
        LOG_WRN("MAX30208 Init Failed (ret=%d) - sensor disabled", ret);
    }

    /* 3. Start Application Threads */
    /* The internal driver init for max32664c is handled by the sensor thread */
    LOG_INF("Starting Application Threads...");
    max77658_app_start();    /* Starts PMIC thread (Priority 7, 2000ms polling) */
    max32664c_app_start();   /* Starts Sensor thread (Priority 5, 40ms polling) */
    lsm6dsv32x_start();      /* Starts IMU thread (Priority 6, 40ms polling) */

    /* 4. Main thread runs I2C scanner and temperature monitoring */
    LOG_INF("Main thread starting I2C scanner loop.");
    while (1) {
        k_sleep(K_SECONDS(5));

        if (max77658_shutdown_requested()) {
            LOG_WRN("Shutdown requested; skipping diagnostics.");
            continue;
        }

        /* Read MAX30208 temperature */
        if (max30208_app_dev() != NULL) {
            int32_t t_x100 = 0;
            int16_t raw = 0;
            
            k_mutex_lock(&i2c_lock, K_FOREVER);
            ret = max30208_app_read_c_x100(&t_x100, &raw);
            k_mutex_unlock(&i2c_lock);
            
            if (ret == 0) {
                LOG_INF("MAX30208: raw=%d  temp=%d.%02d C",
                        raw, (int)(t_x100/100), (int)abs(t_x100%100));
            } else {
                LOG_ERR("MAX30208 read failed: %d", ret);
            }
        }

        /* I2C bus scan (debug only - disable in production) */
#if defined(NESO_I2C_SCAN_DEBUG)
        i2c_scan();
#endif
    }

    return 0;
}