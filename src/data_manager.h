#ifndef DATA_MANAGER_H
#define DATA_MANAGER_H

#include <zephyr/kernel.h>
#include <stdint.h>
#include <stdbool.h>

#define BATCH_SIZE   10
#define PAYLOAD_VER  1

/* -------- Valid flags (what fields are valid in this sample) -------- */
#define VF_VITALS   (1U << 0)  /* HR/SpO2/RR/Conf/SCD valid */
#define VF_TEMP     (1U << 1)
#define VF_BATT     (1U << 2)
#define VF_IMU      (1U << 3)

/* -------- Alert bits (optional, app generated) -------- */
#define AL_ROLLOVER   (1U << 0)
#define AL_FALL_LIKE  (1U << 1)
#define AL_FREEFALL   (1U << 2)
#define AL_IMPACT     (1U << 3)

/* One timestamped reading */
typedef struct __attribute__((packed)) {
    uint32_t ts_ms;        /* k_uptime_get_32() */

    /* ---- MAX32664C ---- */
    uint16_t hr_bpm;       /* hr.val1 */
    uint16_t spo2_x10;     /* spo2.val1 : 985 => 98.5% */
    uint16_t rr_x10;       /* rr.val1 (scaled if needed) */
    uint8_t  hr_conf;      /* hr.val2 0..100 */
    uint8_t  spo2_conf;    /* spo2.val2 0..100 (Separate value) */
    uint8_t  scd;          /* skin.val1 (3 = ON_SKIN) */
    uint8_t  rsv0;

    /* ---- MAX30208 ---- */
    int16_t  temp_c_x100;  /* 2514 => 25.14C, INT16_MIN if invalid */

    /* ---- MAX77658 FG ---- */
    uint8_t  batt_pct;     /* 0..100 */
    uint8_t  chg_present;  /* 0/1 (CHGIN valid) */
    uint16_t batt_mv;      /* AvgVcell in mV (0 if unknown) */
    int16_t  batt_ma_x10;  /* signed current in 0.1mA */

    /* ---- IMU summary ---- */
    int8_t   orientation;      /* imu_orientation_t (cast to int8) */
    uint8_t  activity_level;   /* 0..3 */
    uint16_t kick_count;
    uint8_t  kick_rate_per_min;
    uint8_t  asleep_like;      /* 0/1 */
    uint32_t imu_events;       /* IMU_EVT_* bitmask */
    uint16_t alerts;           /* AL_* bitmask */
    uint8_t  valid_flags;      /* VF_* */
    uint8_t  rsv1;
} patient_sample_t;

/* A batch of samples (count can be 1..10) */
typedef struct __attribute__((packed)) {
    uint8_t  ver;           /* PAYLOAD_VER */
    uint8_t  count;         /* 1..10 */
    uint8_t  algo_mode;     /* algo mode value from MAX32664C */
    uint8_t  rsv0;

    uint32_t batch_seq;     /* increments each batch */
    uint32_t uptime_ms;     /* snapshot */

    patient_sample_t samples[BATCH_SIZE];
} patient_batch_t;

void data_manager_init(void);

/* Set batching threshold: 10 for continuous, 1 for periodic */
void data_manager_set_batch_target(uint8_t target_count);

/* Update “latest” values coming from other threads */
void data_manager_update_temp(int16_t temp_c_x100, bool valid);
void data_manager_update_batt(uint8_t batt_pct, uint16_t batt_mv, int16_t batt_ma_x10,
                              uint8_t chg_present, bool valid);
void data_manager_update_imu(int8_t orientation, uint8_t activity_level,
                             uint16_t kick_count, uint8_t kick_rate_per_min,
                             uint8_t asleep_like, uint32_t imu_events,
                             uint16_t alerts, bool valid);

/* Push one vitals sample tick (MAX32664C thread calls this) */
void data_manager_push_vitals(uint8_t algo_mode,
                              uint16_t hr_bpm, uint8_t hr_conf,
                              uint16_t spo2_x10, uint8_t spo2_conf,
                              uint16_t rr_x10, uint8_t scd);

/* Consumers */
int data_manager_get_batch(patient_batch_t *batch);      /* HTTP/WiFi */
int data_manager_get_ble_batch(patient_batch_t *batch);  /* BLE */

#endif