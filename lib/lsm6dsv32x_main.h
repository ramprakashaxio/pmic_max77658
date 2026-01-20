#ifndef LSM6DSV32X_MAIN_H_
#define LSM6DSV32X_MAIN_H_

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Event bitmask (keep existing bits stable) */
#define IMU_EVT_KICK           (1u << 0)
#define IMU_EVT_FREEFALL       (1u << 1)
#define IMU_EVT_IMPACT         (1u << 2)
#define IMU_EVT_FALL_LIKE      (1u << 3)
#define IMU_EVT_STILL          (1u << 4)
#define IMU_EVT_ORIENT_CHANGE  (1u << 5)

/* New */
#define IMU_EVT_ASLEEP_CHANGE  (1u << 6)

/* Orientation enum */
typedef enum {
    IMU_ORIENT_UNKNOWN = 0,
    IMU_ORIENT_UP,
    IMU_ORIENT_DOWN,
    IMU_ORIENT_LEFT,
    IMU_ORIENT_RIGHT,
    IMU_ORIENT_FORWARD,
    IMU_ORIENT_BACK
} imu_orientation_t;

/* Output sample */
typedef struct {
    uint32_t sample_seq;

    int32_t acc_u_ms2[3];   /* micro m/s^2 */
    int32_t gyr_u_rads[3];  /* micro rad/s */

    uint8_t  activity_level;      /* 0..3 */
    uint32_t kick_count;
    uint16_t kick_rate_per_min;

    int8_t   asleep_like;         /* 0/1 */
    int8_t   orientation;         /* imu_orientation_t as int */
    int8_t   freefall_event;      /* 0/1 (edge) */
    int8_t   fall_like_event;     /* 0/1 (edge) */

    uint32_t events;
} lsm6dsv32x_sample_t;

/* Core API */
int lsm6dsv32x_init(void);
int lsm6dsv32x_measure(lsm6dsv32x_sample_t *out);

/* Optional ODR */
int lsm6dsv32x_set_odr_hz(uint16_t hz);

/* NEW: IMU thread inside module */
int  lsm6dsv32x_start(void);
void lsm6dsv32x_stop(void);

/* Optional: best-effort low power before ship-mode */
void lsm6dsv32x_prepare_for_shutdown(void);

#ifdef __cplusplus
}
#endif

#endif /* LSM6DSV32X_MAIN_H_ */
