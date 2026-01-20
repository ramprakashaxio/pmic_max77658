/*
 * max32664c_main.c
 * - Continuous Green LED Probing
 * - Sticky SpO2 Mode (Prevents Zeroing/Flicker)
 * - Robust State Machine with Hysteresis
 */

#include <zephyr/kernel.h>
#include <zephyr/drivers/sensor.h>
#include <zephyr/logging/log.h>

#include "max32664c.h"      /* Driver Header */
#include "max32664c_main.h" /* Public API Header */
#include "app_i2c_lock.h"   /* If you use a global I2C lock */
#include "max77658_main.h"  /* PMIC Software Off API */
#include "data_manager.h"

LOG_MODULE_REGISTER(max32664c_app, LOG_LEVEL_INF);

/* TODO: Implement User-Selectable Polling Logic
 * 
 * Current Implementation: CONTINUOUS MODE ONLY
 * Future Enhancement: Add STANDBY and PERIODIC modes
 * 
 * Flowchart: User-Selectable Polling Logic
 * 
 * graph TD
 *     INIT((Start App Thread)) --> WAIT{Check User Mode}
 * 
 *     %% --- BRANCH 1: STANDBY ---
 *     WAIT -- Mode: STANDBY --> S_SLEEP[Send Sensor Deep Sleep Cmd]
 *     S_SLEEP --> S_WAIT[Wait Indefinitely<br>for Mode Change Signal]
 *     S_WAIT --> WAIT
 * 
 *     %% --- BRANCH 2: CONTINUOUS MONITORING ---
 *     WAIT -- Mode: CONTINUOUS --> C_WAKE[Wake Sensor & Apply Mode 0x00]
 *     C_WAKE --> C_FETCH[Fetch Sample]
 *     C_FETCH --> C_LOG[Log HR / SpO2]
 *     C_LOG --> C_CHECK_SIG{Mode Changed?}
 *     C_CHECK_SIG -- No --> C_DELAY[Sleep 40ms]
 *     C_DELAY --> C_FETCH
 *     C_CHECK_SIG -- Yes --> WAIT
 * 
 *     %% --- BRANCH 3: PERIODIC (30s, 1m, 5m, 10m) ---
 *     WAIT -- Mode: PERIODIC --> P_CALC[Calc Interval Seconds]
 *     P_CALC --> P_WAKE[Wake Sensor <br> MFIO Low 300us]
 *     P_WAKE --> P_INIT[Apply Algo Mode 0x00<br>Reload Coeffs]
 *     P_INIT --> P_TIMER[Start Timeout Timer <br> e.g., 30 sec]
 * 
 *     %% Periodic Measurement Loop
 *     P_TIMER --> P_FETCH[Fetch Sample]
 *     P_FETCH --> P_VALID{Valid Finger &<br>Conf > 90%?}
 *     
 *     P_VALID -- Yes --> P_SAVE[<b>Save/Send Data</b>]
 *     P_SAVE --> P_OFF
 *     
 *     P_VALID -- No --> P_TOUT{Timeout Expired?}
 *     P_TOUT -- No --> P_WAIT_POLL[Sleep 100ms]
 *     P_WAIT_POLL --> P_FETCH
 *     P_TOUT -- Yes --> P_LOG_FAIL[Log: No Finger Detected]
 *     P_LOG_FAIL --> P_OFF
 * 
 *     %% Periodic Sleep Cycle
 *     P_OFF[<b>Turn Off Sensor</b><br>Send Mode IDLE]
 *     P_OFF --> P_HOST_SLEEP[<b>Host Sleep</b><br>Wait for Interval OR Signal]
 *     P_HOST_SLEEP --> P_WAKE_CHECK{Woke by Signal?}
 *     P_WAKE_CHECK -- Yes (User Input) --> WAIT
 *     P_WAKE_CHECK -- No (Timer Expired) --> P_WAKE
 * 
 * Implementation Notes:
 * - Add enum for operation modes: STANDBY, CONTINUOUS, PERIODIC_30S, PERIODIC_1M, PERIODIC_5M, PERIODIC_10M
 * - Add mode selection via sensor attribute or shell command
 * - Implement sensor deep sleep command (set mode to IDLE)
 * - Add wake-up via MFIO pulse (300us low pulse)
 * - Implement timeout mechanism for periodic measurements (30s default)
 * - Add data save/send callback for periodic measurements
 * - Use k_timer for periodic intervals and measurement timeouts
 */

/* Device Tree Reference */
#define MAX32664C_NODE DT_NODELABEL(max32664c)
static const struct device *sensor = DEVICE_DT_GET(MAX32664C_NODE);

/* Thread Configuration */
#define SENSOR_APP_STACK_SIZE   2048
#define SENSOR_APP_PRIORITY     5

/* * TIMING & THRESHOLD CONFIGURATION 
 * - FAST_ENTRY_COUNT: 10 samples (~0.4s) of high confidence to Enter SpO2
 * - LONG_EXIT_COUNT:  125 samples (~5s) of continuous "No Contact" to Exit SpO2
 * - STABLE_CONFIDENCE_THR: Minimum HR confidence (0-100%) required to switch
 */
#define FAST_ENTRY_COUNT        10
#define LONG_EXIT_COUNT         125
#define STABLE_CONFIDENCE_THR   80    

K_THREAD_STACK_DEFINE(sensor_app_stack, SENSOR_APP_STACK_SIZE);
struct k_thread sensor_app_thread;

/* Application States */
enum app_state {
    STATE_A_IDLE,
    STATE_B_PROBING,       /* Green LED, Low Power */
    STATE_C_STABILIZING,   /* Green LED, Verifying Signal */
    STATE_D_MEASURING,     /* Red/IR LED, SpO2 Mode */
    STATE_E_COOLDOWN       /* Wait state after finger removal */
};

/* Helper to convert state enum to string for logs */
static const char *state_str(enum app_state s) {
    switch(s) {
        case STATE_A_IDLE:      return "IDLE";
        case STATE_B_PROBING:   return "PROBING";
        case STATE_C_STABILIZING: return "STABILIZING";
        case STATE_D_MEASURING:   return "MEASURING";
        case STATE_E_COOLDOWN:    return "COOLDOWN";
        default: return "?";
    }
}

/* Helper function to switch modes safely with delays */
static int app_set_algo_mode(int mode_val)
{
    struct sensor_value mode = {
        .val1 = MAX32664C_OP_MODE_ALGO_AEC_EXT, /* Always re-apply Device Mode */
        .val2 = mode_val                        /* Target Algorithm (2 or 6) */
    };
    
    int ret = sensor_attr_set(sensor, SENSOR_CHAN_ALL, SENSOR_ATTR_MAX32664C_OP_MODE, &mode);
    
    /* CRITICAL DELAY: Hardware Settle Time 
     * The sensor takes ~250ms to switch internal buffers and AFE.
     * Reading too soon causes I2C -5 errors.
     */
    k_msleep(250);
    
    return ret;
}

/* Main Sensor Thread Loop */
void sensor_app_thread_entry(void *p1, void *p2, void *p3)
{
    struct sensor_value hr, spo2, skin, rr;
    enum app_state current_state = STATE_A_IDLE;
    int ret;
    
    /* Logic Counters */
    int confidence_counter = 0;      /* For entering SpO2 */
    int finger_missing_counter = 0;  /* For exiting SpO2 (Debounce) */
    int stabilization_grace_period = 0; /* Startup grace period */
    int cooldown_timer = 0;
    int64_t probing_start_ms = 0;    /* Timeout tracker for PROBING state */

    ARG_UNUSED(p1); ARG_UNUSED(p2); ARG_UNUSED(p3);

    /* 1. Wait for Driver Readiness */
    if (!device_is_ready(sensor)) { 
        LOG_ERR("MAX32664C Device not ready!"); 
        return; 
    }
    
    /* Wait for sensor boot (rails to stabilize) */
    k_msleep(3000); 
    LOG_INF("MAX32664C App Started. Initializing...");

    /* 2. Initial Mode Set (Retry Loop) */
    while (1) {
        /* Start in Continuous Heart Rate (Green LED) */
        ret = app_set_algo_mode(MAX32664C_ALGO_MODE_CONT_HR);
        if (ret == 0) {
            LOG_INF("Mode Set Success! Starting Loop.");
            current_state = STATE_B_PROBING;
            probing_start_ms = k_uptime_get();
            break;
        }
        LOG_WRN("Waiting for sensor response... (err %d)", ret);
        k_msleep(1000);
    }

    /* 3. Main Polling Loop */
    while (1) {
        /* A. Fetch Data from Driver FIFO */
        ret = sensor_sample_fetch(sensor);
        
        if (ret < 0) {
            /* If fetch fails (sensor busy/rebooting), wait and retry */
            k_msleep(50);
            continue;
        }
        
        /* B. Get All Vitals */
        sensor_channel_get(sensor, SENSOR_CHAN_MAX32664C_SKIN_CONTACT, &skin);
        sensor_channel_get(sensor, SENSOR_CHAN_MAX32664C_HEARTRATE, &hr);
        sensor_channel_get(sensor, SENSOR_CHAN_MAX32664C_BLOOD_OXYGEN_SATURATION, &spo2);
        sensor_channel_get(sensor, SENSOR_CHAN_MAX32664C_RESPIRATION_RATE, &rr);

        /* --- NEW: Push Data in ALL States --- */
        /* Use 10 samples -> 1 batch for continuous streaming */
        data_manager_set_batch_target(BATCH_SIZE); 

        /* Pass 'current_state' or a specific mode flag as the first arg if you want to track state context */
        data_manager_push_vitals(
            (uint8_t)MAX32664C_ALGO_MODE_CONT_HR_CONT_SPO2, /* Or dynamically: (current_state == STATE_D_MEASURING ? 2 : 1) */
            (uint16_t)hr.val1,   (uint8_t)hr.val2,           /* HR + HR Conf */
            (uint16_t)spo2.val1, (uint8_t)spo2.val2,         /* SpO2 + SpO2 Conf */
            (uint16_t)rr.val1,   (uint8_t)skin.val1
        );
        /* ------------------------------------ */

        /* C. State Machine Logic */
        switch (current_state) {

        /* --- STATE B: PROBING (Green LED) --- */
        case STATE_B_PROBING:
        {
            /* If finger detected, proceed normally */
            if (skin.val1 == 3) { /* 3 = ON_SKIN */
                LOG_INF(">>> FINGER DETECTED! Transition to STABILIZING");
                current_state = STATE_C_STABILIZING;
                confidence_counter = 0;
                break;
            }

            /* No finger: if PROBING > 5min, power off */
            int64_t elapsed = k_uptime_get() - probing_start_ms;
            if (elapsed >= 300000) {
                LOG_WRN("No finger for 5 Minutes. Requesting PMIC SOFTWARE OFF (wake via CHGIN).");

                /* Request PMIC shutdown (PMIC thread will execute it safely) */
                max77658_request_software_off("SCD no-contact 5min");

                /* After requesting shutdown, just idle (PMIC will cut power soon) */
                while (1) { k_msleep(1000); }
            }

            k_msleep(100);
        }
            break;

        /* --- STATE C: STABILIZING (Green LED - Waiting for Signal Lock) --- */
        case STATE_C_STABILIZING:
            LOG_INF("[STABLE] SCD:%d | HR:%d (Conf:%d%%)", skin.val1, hr.val1, hr.val2);

            if (skin.val1 != 3) {
                /* If finger removed immediately, go back to probing */
                LOG_WRN("<<< LOST SKIN (Immediate). Back to PROBING.");
                current_state = STATE_B_PROBING;
                probing_start_ms = k_uptime_get();
                confidence_counter = 0;
            } 
            else if (hr.val2 >= STABLE_CONFIDENCE_THR) {
                /* Count consecutive GOOD frames with high confidence */
                confidence_counter++;
                
                /* FAST ENTRY: If we have 10 good frames (~0.4s), switch to SpO2 */
                if (confidence_counter >= FAST_ENTRY_COUNT) {
                    LOG_INF(">>> FAST LOCK! Switching to SpO2 Mode (Sticky Mode).");
                    
                    /* 1. Perform Mode Switch (Stop -> Config -> Start) */
                    app_set_algo_mode(MAX32664C_ALGO_MODE_CONT_HR_CONT_SPO2); 
                    
                    /* 2. Wait for hardware blackout/calibration */
                    k_msleep(500); 
                    
                    /* 3. Setup Measurement State */
                    stabilization_grace_period = 75; /* 3s grace for startup */
                    finger_missing_counter = 0;
                    current_state = STATE_D_MEASURING;
                }
            } else {
                /* Confidence dropped, reset counter */
                confidence_counter = 0;
            }
            k_msleep(40);
            break;

        /* --- STATE D: MEASURING (Red/IR LEDs On - Sticky SpO2 Mode) --- */
        case STATE_D_MEASURING:
        {
            /* Note: We NEVER re-apply AEC/Mode here to avoid zeroing readings.
             * We stay "stuck" in this mode until the user really leaves.
             */
            
            /* PATCH: Parse 0.1% units correctly */
            /* val1 = 985 -> 98.5% */
            int spo2_int = spo2.val1 / 10;
            int spo2_dec = spo2.val1 % 10;

            LOG_INF("[MEASURE] HR:%d | SpO2: %d.%d%% (Conf:%d%%) | SCD:%d | Miss:%d",
                    hr.val1, 
                    spo2_int, spo2_dec, 
                    spo2.val2, /* val2 is Confidence */
                    skin.val1,
                    finger_missing_counter);

            /* Grace Period: Ignore SCD glitches immediately after mode switch */
            if (stabilization_grace_period > 0) {
                stabilization_grace_period--;
            } 
            else {
                /* Sticky Logic: Only exit if finger is gone for a LONG time */
                if (skin.val1 != 3) {
                    finger_missing_counter++;
                    
                    /* EXIT CONDITION: 5 Seconds of CONTINUOUS NO CONTACT */
                    if (finger_missing_counter > LONG_EXIT_COUNT) {
                        LOG_WRN("<<< FINGER GONE FOR 5s. Resetting to Green Mode.");
                        
                        /* Switch back to Green Mode */
                        app_set_algo_mode(MAX32664C_ALGO_MODE_CONT_HR);
                        k_msleep(500); // Allow switch to settle
                        
                        current_state = STATE_E_COOLDOWN;
                        cooldown_timer = 0;
                        finger_missing_counter = 0;
                    }
                } else {
                    /* Finger detected! Reset counter immediately. 
                     * This keeps us "Latched" in SpO2 mode even if signal is weak. 
                     */
                    finger_missing_counter = 0;
                }
            }
            k_msleep(40);
        }
            break;
        
        /* --- STATE E: COOLDOWN (Wait before re-probing) --- */
        case STATE_E_COOLDOWN:
            LOG_INF("[COOLDOWN] Waiting... (%d/50)", cooldown_timer);
            cooldown_timer++;
            
            /* Wait 2 seconds before probing again */
            if (cooldown_timer > 50) {
                LOG_INF(">>> COOLDOWN COMPLETE. Returning to PROBING.");
                current_state = STATE_B_PROBING;
                probing_start_ms = k_uptime_get();
            }
            k_msleep(40);
            break;
            
        case STATE_A_IDLE:
             current_state = STATE_B_PROBING;
             probing_start_ms = k_uptime_get();
             break;
        }
    }
}

/* Function to spawn the thread */
void max32664c_app_start(void)
{
    k_thread_create(&sensor_app_thread, sensor_app_stack,
            K_THREAD_STACK_SIZEOF(sensor_app_stack),
            sensor_app_thread_entry,
            NULL, NULL, NULL,
            SENSOR_APP_PRIORITY, 0, K_NO_WAIT);
    k_thread_name_set(&sensor_app_thread, "sensor_app");
    LOG_INF("Sensor App Thread Created (Priority: %d)", SENSOR_APP_PRIORITY);
}

/* Optional Legacy Functions */
int max32664c_app_init(void) { return 0; }
void max32664c_app_process_events(void) { }
