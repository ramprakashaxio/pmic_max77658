# SPDX-License-Identifier: Apache-2.0

cmake_minimum_required(VERSION 3.20.0)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(pmic_2512_nrf54)

# Application sources
target_sources(app PRIVATE 
    src/main.c
    src/pmic_gpio.c
    src/data_manager.c
    src/storage_manager.c
    src/ble_app.c
    lib/max77658_main.c
    lib/max32664c_main.c
    lib/max30208_main.c
    lib/lsm6dsv32x_main.c
    lib/comman/app_i2c_lock.c
    driver/pmic/max77658.c
    driver/pmic/bsp.c
    # MAX30208 temperature sensor driver
    driver/max30208/max30208.c
    # MAX32664C driver sources
    driver/max32664c/max32664c.c
    driver/max32664c/max32664c_acc.c
    driver/max32664c/max32664c_bl.c
    driver/max32664c/max32664c_init.c
    driver/max32664c/max32664c_worker.c
)

# Conditionally add MAX32664C interrupt support
if(CONFIG_MAX32664C_USE_INTERRUPT)
    target_sources(app PRIVATE driver/max32664c/max32664c_interrupt.c)
endif()

# Conditionally add PMIC module sources
if(CONFIG_PMIC_MAX77658_PM)
    target_sources(app PRIVATE driver/pmic/max77658_pm.c)
endif()

if(CONFIG_PMIC_MAX77658_FG)
    target_sources(app PRIVATE driver/pmic/max77658_fg.c)
endif()

# Include directories
target_include_directories(app PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/driver/pmic
    ${CMAKE_CURRENT_SOURCE_DIR}/driver/max32664c
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${CMAKE_CURRENT_SOURCE_DIR}/lib
    ${CMAKE_CURRENT_SOURCE_DIR}/lib/comman
)
