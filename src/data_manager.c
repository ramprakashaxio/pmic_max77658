#include "data_manager.h"
#include <zephyr/logging/log.h>
#include <string.h>
#include <limits.h>

LOG_MODULE_REGISTER(data_mgr, LOG_LEVEL_INF);

/* Two separate queues so slow Wi-Fi doesn't block fast BLE */
/* BLE depth increased so ingest thread can absorb bursts while flash/flush happens */
K_MSGQ_DEFINE(batch_q,  sizeof(patient_batch_t), 4, 4);
K_MSGQ_DEFINE(ble_msgq, sizeof(patient_batch_t), 16, 4);

static patient_batch_t current_batch;
static uint32_t g_batch_seq;
static uint8_t  g_target_count = BATCH_SIZE;

static struct k_mutex dm_lock;

/* latest caches written by other threads */
static struct {
    int16_t  temp_c_x100;
    bool     temp_valid;

    uint8_t  batt_pct;
    uint16_t batt_mv;
    int16_t  batt_ma_x10;
    uint8_t  chg_present;
    bool     batt_valid;

    int8_t   orientation;
    uint8_t  activity_level;
    uint16_t kick_count;
    uint8_t  kick_rate_per_min;
    uint8_t  asleep_like;
    uint32_t imu_events;
    uint16_t alerts;
    bool     imu_valid;
} latest;

static void finalize_and_queue_batch(uint8_t flushed_count)
{
    current_batch.ver      = PAYLOAD_VER;
    current_batch.batch_seq = g_batch_seq++;
    current_batch.uptime_ms = k_uptime_get_32();

    /* push to both consumers */
    if (k_msgq_put(&batch_q, &current_batch, K_NO_WAIT) != 0) {
        LOG_WRN("Cloud queue full, dropping batch");
    }
    if (k_msgq_put(&ble_msgq, &current_batch, K_NO_WAIT) != 0) {
        LOG_WRN("BLE queue full, dropping batch");
    }

    LOG_INF("Batch queued: count=%u seq=%u algo=%u",
            flushed_count, (unsigned)current_batch.batch_seq, current_batch.algo_mode);

    /* reset */
    current_batch.count = 0;
}

void data_manager_init(void)
{
    k_mutex_init(&dm_lock);
    memset(&latest, 0, sizeof(latest));

    memset(&current_batch, 0, sizeof(current_batch));
    current_batch.ver = PAYLOAD_VER;
    current_batch.count = 0;

    g_batch_seq = 0;
    g_target_count = BATCH_SIZE;
}

void data_manager_set_batch_target(uint8_t target_count)
{
    if (target_count < 1) target_count = 1;
    if (target_count > BATCH_SIZE) target_count = BATCH_SIZE;
    g_target_count = target_count;
    LOG_INF("Batch target set to %u", g_target_count);
}

void data_manager_update_temp(int16_t temp_c_x100, bool valid)
{
    k_mutex_lock(&dm_lock, K_FOREVER);
    latest.temp_c_x100 = temp_c_x100;
    latest.temp_valid  = valid;
    k_mutex_unlock(&dm_lock);
}

void data_manager_update_batt(uint8_t batt_pct, uint16_t batt_mv, int16_t batt_ma_x10,
                              uint8_t chg_present, bool valid)
{
    k_mutex_lock(&dm_lock, K_FOREVER);
    latest.batt_pct     = batt_pct;
    latest.batt_mv      = batt_mv;
    latest.batt_ma_x10  = batt_ma_x10;
    latest.chg_present  = chg_present;
    latest.batt_valid   = valid;
    k_mutex_unlock(&dm_lock);
}

void data_manager_update_imu(int8_t orientation, uint8_t activity_level,
                             uint16_t kick_count, uint8_t kick_rate_per_min,
                             uint8_t asleep_like, uint32_t imu_events,
                             uint16_t alerts, bool valid)
{
    k_mutex_lock(&dm_lock, K_FOREVER);
    latest.orientation       = orientation;
    latest.activity_level    = activity_level;
    latest.kick_count        = kick_count;
    latest.kick_rate_per_min = kick_rate_per_min;
    latest.asleep_like       = asleep_like;
    latest.imu_events        = imu_events;
    latest.alerts            = alerts;
    latest.imu_valid         = valid;
    k_mutex_unlock(&dm_lock);
}

void data_manager_push_vitals(uint8_t algo_mode,
                              uint16_t hr_bpm, uint8_t hr_conf,
                              uint16_t spo2_x10, uint8_t spo2_conf,
                              uint16_t rr_x10, uint8_t scd)
{
    patient_sample_t s;
    memset(&s, 0, sizeof(s));

    s.ts_ms     = k_uptime_get_32();
    s.hr_bpm    = hr_bpm;
    s.hr_conf   = hr_conf;
    s.spo2_x10  = spo2_x10;
    s.spo2_conf = spo2_conf; /* Stored separately here */
    s.rr_x10    = rr_x10;
    s.scd       = scd;

    s.valid_flags |= VF_VITALS;

    /* Merge cached values (no I2C here) */
    k_mutex_lock(&dm_lock, K_FOREVER);

    if (latest.temp_valid) {
        s.temp_c_x100 = latest.temp_c_x100;
        s.valid_flags |= VF_TEMP;
    } else {
        s.temp_c_x100 = INT16_MIN;
    }

    if (latest.batt_valid) {
        s.batt_pct    = latest.batt_pct;
        s.batt_mv     = latest.batt_mv;
        s.batt_ma_x10 = latest.batt_ma_x10;
        s.chg_present = latest.chg_present;
        s.valid_flags |= VF_BATT;
    }

    if (latest.imu_valid) {
        s.orientation       = latest.orientation;
        s.activity_level    = latest.activity_level;
        s.kick_count        = latest.kick_count;
        s.kick_rate_per_min = latest.kick_rate_per_min;
        s.asleep_like       = latest.asleep_like;
        s.imu_events        = latest.imu_events;
        s.alerts            = latest.alerts;
        s.valid_flags |= VF_IMU;
    }

    k_mutex_unlock(&dm_lock);

    /* Add into current batch */
    current_batch.algo_mode = algo_mode;

    if (current_batch.count < BATCH_SIZE) {
        current_batch.samples[current_batch.count++] = s;
    } else {
        /* should never happen; reset logic safety */
        current_batch.count = 0;
        current_batch.samples[current_batch.count++] = s;
    }

    /* Flush policy */
    if (current_batch.count >= g_target_count) {
        finalize_and_queue_batch(current_batch.count);
    }
}

int data_manager_get_batch(patient_batch_t *batch)
{
    return k_msgq_get(&batch_q, batch, K_FOREVER);
}

int data_manager_get_ble_batch(patient_batch_t *batch)
{
    return k_msgq_get(&ble_msgq, batch, K_FOREVER);
}