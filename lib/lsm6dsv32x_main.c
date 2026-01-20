#include "lsm6dsv32x_main.h"
#include "app_i2c_lock.h"
#include "max77658_main.h"   /* for max77658_shutdown_requested() */
#include "data_manager.h"

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/sensor.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/util.h>
#include <errno.h>

LOG_MODULE_REGISTER(lsm6dsv32x_main, LOG_LEVEL_INF);

/* Prefer alias imu0; fallback to nodelabel lsm6dsv32x */
#if DT_NODE_EXISTS(DT_ALIAS(imu0))
#define IMU_NODE DT_ALIAS(imu0)
#elif DT_NODE_EXISTS(DT_NODELABEL(lsm6dsv32x))
#define IMU_NODE DT_NODELABEL(lsm6dsv32x)
#else
#error "No IMU node found. Define alias 'imu0' or node label 'lsm6dsv32x' in overlay."
#endif

static const struct device *g_imu;
static uint32_t g_seq;

/* ---- helpers ---- */
static inline int32_t sv_to_micro(const struct sensor_value *v)
{
    int64_t x = (int64_t)v->val1 * 1000000LL + (int64_t)v->val2;
    if (x > INT32_MAX) x = INT32_MAX;
    if (x < INT32_MIN) x = INT32_MIN;
    return (int32_t)x;
}

static inline uint32_t uabs32(int32_t x)
{
    return (x < 0) ? (uint32_t)(-x) : (uint32_t)x;
}

static uint32_t isqrt_u64(uint64_t x)
{
    uint64_t op = x;
    uint64_t res = 0;
    uint64_t one = 1ULL << 62;

    while (one > op) {
        one >>= 2;
    }
    while (one != 0) {
        if (op >= res + one) {
            op  -= res + one;
            res  = (res >> 1) + one;
        } else {
            res >>= 1;
        }
        one >>= 2;
    }
    return (uint32_t)res;
}

static imu_orientation_t estimate_orientation_raw(int32_t ax_u, int32_t ay_u, int32_t az_u)
{
    uint32_t x = uabs32(ax_u), y = uabs32(ay_u), z = uabs32(az_u);

    if (z >= x && z >= y) {
        return (az_u >= 0) ? IMU_ORIENT_UP : IMU_ORIENT_DOWN;
    } else if (x >= y) {
        return (ax_u >= 0) ? IMU_ORIENT_RIGHT : IMU_ORIENT_LEFT;
    } else {
        return (ay_u >= 0) ? IMU_ORIENT_FORWARD : IMU_ORIENT_BACK;
    }
}

/* ---- feature config ---- */
#define ONE_G_U_MS2        (9806650U)  /* 9.80665 m/s^2 in micro */
#define KICK_THR_U_MS2     (ONE_G_U_MS2 / 4U)      /* ~0.25g */
#define KICK_DEBOUNCE_MS   (200)

#define FREEFALL_THR_U_MS2 (3432328U)   /* ~0.35g */
#define FREEFALL_MIN_MS    (120)
#define IMPACT_THR_U_MS2   (24516625U)  /* ~2.5g */

#define STILL_DYN_U_MS2    (294200U)    /* ~0.03g */
#define STILL_GYRO_U_RADS  (87266U)     /* ~5 dps in micro rad/s */
#define STILL_MIN_MS       (5000)
#define ASLEEP_MIN_MS      (30000)

/* ---- orientation stability filter ---- */
#define ORIENT_STABLE_COUNT        (25)                 /* 25 samples @25Hz = 1s */
#define ORIENT_MAX_DYN_U_MS2       (ONE_G_U_MS2 / 6U)   /* ignore orientation updates during heavy motion */
#define ORIENT_MIN_MAG_U_MS2       (ONE_G_U_MS2 * 7U / 10U)  /* 0.7g */
#define ORIENT_MAX_MAG_U_MS2       (ONE_G_U_MS2 * 13U / 10U) /* 1.3g */
#define ORIENT_DOMINANCE_MIN_U_MS2 (ONE_G_U_MS2 / 2U)   /* dominant axis must exceed ~0.5g */
#define ORIENT_DOMINANCE_DELTA_U_MS2 (ONE_G_U_MS2 / 6U) /* dominant - second must exceed ~0.166g */

/* ---- kick history ---- */
#define KICK_BUF_SZ (64)

static struct {
    /* kick */
    uint32_t kick_count;
    int64_t  last_kick_ms;
    int64_t  kick_ts[KICK_BUF_SZ];
    uint8_t  kick_head;  /* next write */
    uint8_t  kick_len;   /* number of valid entries (<=KICK_BUF_SZ) */

    /* freefall/impact */
    bool     in_freefall;
    bool     freefall_reported;
    int64_t  freefall_start_ms;
    int64_t  freefall_recent_ms;  /* last time freefall ended, for FALL_LIKE window */
    bool     impact_reported;

    /* still/asleep */
    int64_t  still_start_ms;
    bool     still_latched;
    int64_t  asleep_latched_ms;
    int8_t   asleep_prev;

    /* orientation filter */
    imu_orientation_t orient_candidate;
    uint8_t           orient_run;
    imu_orientation_t orient_stable;
} s;

static void kick_buf_push(int64_t now_ms)
{
    s.kick_ts[s.kick_head] = now_ms;
    s.kick_head = (uint8_t)((s.kick_head + 1U) % KICK_BUF_SZ);
    if (s.kick_len < KICK_BUF_SZ) {
        s.kick_len++;
    }
}

static void kick_buf_purge_older_than_60s(int64_t now_ms)
{
    while (s.kick_len > 0U) {
        uint8_t oldest = (uint8_t)((s.kick_head + KICK_BUF_SZ - s.kick_len) % KICK_BUF_SZ);
        if ((now_ms - s.kick_ts[oldest]) > 60000) {
            s.kick_len--;
        } else {
            break;
        }
    }
}

/* Returns filtered stable orientation; sets *changed if stable orientation changed */
static imu_orientation_t update_orientation_filter(int32_t ax_u, int32_t ay_u, int32_t az_u,
                                                   uint32_t amag_u, uint32_t dyn_u,
                                                   bool *changed)
{
    *changed = false;

    /* Don’t trust orientation during freefall/impact or strong motion */
    if (dyn_u > ORIENT_MAX_DYN_U_MS2) {
        return s.orient_stable;
    }

    /* Must be roughly around 1g */
    if (amag_u < ORIENT_MIN_MAG_U_MS2 || amag_u > ORIENT_MAX_MAG_U_MS2) {
        return s.orient_stable;
    }

    uint32_t x = uabs32(ax_u), y = uabs32(ay_u), z = uabs32(az_u);

    /* dominance check to avoid “on-edge” jitter */
    uint32_t max1 = x, max2 = 0;
    if (y > max1) { max2 = max1; max1 = y; } else { max2 = y; }
    if (z > max1) { max2 = max1; max1 = z; } else if (z > max2) { max2 = z; }

    if (max1 < ORIENT_DOMINANCE_MIN_U_MS2) {
        return s.orient_stable;
    }
    if ((max1 - max2) < ORIENT_DOMINANCE_DELTA_U_MS2) {
        return s.orient_stable;
    }

    imu_orientation_t raw = estimate_orientation_raw(ax_u, ay_u, az_u);

    if (raw == s.orient_candidate) {
        if (s.orient_run < 255U) s.orient_run++;
    } else {
        s.orient_candidate = raw;
        s.orient_run = 1U;
    }

    if (s.orient_run >= ORIENT_STABLE_COUNT) {
        if (s.orient_stable != raw) {
            imu_orientation_t prev = s.orient_stable;
            s.orient_stable = raw;

            /* Only report change if previous stable was valid */
            if (prev != IMU_ORIENT_UNKNOWN) {
                *changed = true;
            }
        }
        /* keep run saturated */
        s.orient_run = ORIENT_STABLE_COUNT;
    }

    return s.orient_stable;
}

/* Optional: set IMU ODR (best effort) */
int lsm6dsv32x_set_odr_hz(uint16_t hz)
{
    if (!g_imu || !device_is_ready(g_imu)) return -ENODEV;

    struct sensor_value odr = { .val1 = (int32_t)hz, .val2 = 0 };

    int r1 = sensor_attr_set(g_imu, SENSOR_CHAN_ACCEL_XYZ, SENSOR_ATTR_SAMPLING_FREQUENCY, &odr);
    int r2 = sensor_attr_set(g_imu, SENSOR_CHAN_GYRO_XYZ,  SENSOR_ATTR_SAMPLING_FREQUENCY, &odr);

    /* Ignore ENOTSUP if driver doesn’t implement it */
    if (r1 == -ENOTSUP) r1 = 0;
    if (r2 == -ENOTSUP) r2 = 0;

    return (r1 != 0) ? r1 : r2;
}

void lsm6dsv32x_prepare_for_shutdown(void)
{
    /* Best effort: drop ODR to minimum to save power */
    (void)lsm6dsv32x_set_odr_hz(1);
}

/* ---- public API ---- */
int lsm6dsv32x_init(void)
{
    g_imu = DEVICE_DT_GET(IMU_NODE);
    if (!device_is_ready(g_imu)) {
        LOG_ERR("IMU device not ready");
        return -ENODEV;
    }

    /* Production: set hardware ODR around ~52Hz (good for kicks), app can sample at 25Hz */
    (void)lsm6dsv32x_set_odr_hz(52);

    g_seq = 0;

    s.kick_count = 0;
    s.last_kick_ms = 0;
    s.kick_head = 0;
    s.kick_len = 0;

    s.in_freefall = false;
    s.freefall_reported = false;
    s.freefall_start_ms = 0;
    s.freefall_recent_ms = -100000; /* long ago */
    s.impact_reported = false;

    s.still_start_ms = 0;
    s.still_latched = false;
    s.asleep_latched_ms = 0;
    s.asleep_prev = -1;

    s.orient_candidate = IMU_ORIENT_UNKNOWN;
    s.orient_run = 0;
    s.orient_stable = IMU_ORIENT_UNKNOWN;

    LOG_INF("IMU init OK: %s", g_imu->name);
    return 0;
}

int lsm6dsv32x_measure(lsm6dsv32x_sample_t *out)
{
    if (!out) return -EINVAL;
    if (!g_imu || !device_is_ready(g_imu)) return -ENODEV;

    int rc = sensor_sample_fetch(g_imu);
    if (rc) return rc;

    struct sensor_value a[3], g[3];
    rc = sensor_channel_get(g_imu, SENSOR_CHAN_ACCEL_XYZ, a);
    if (rc) return rc;
    rc = sensor_channel_get(g_imu, SENSOR_CHAN_GYRO_XYZ, g);
    if (rc) return rc;

    int32_t ax = sv_to_micro(&a[0]);
    int32_t ay = sv_to_micro(&a[1]);
    int32_t az = sv_to_micro(&a[2]);

    int32_t gx = sv_to_micro(&g[0]);
    int32_t gy = sv_to_micro(&g[1]);
    int32_t gz = sv_to_micro(&g[2]);

    uint64_t asq = (uint64_t)((int64_t)ax * ax) + (uint64_t)((int64_t)ay * ay) + (uint64_t)((int64_t)az * az);
    uint64_t gsq = (uint64_t)((int64_t)gx * gx) + (uint64_t)((int64_t)gy * gy) + (uint64_t)((int64_t)gz * gz);

    uint32_t amag = isqrt_u64(asq); /* micro m/s^2 */
    uint32_t gmag = isqrt_u64(gsq); /* micro rad/s */

    int64_t now = k_uptime_get();

    uint32_t dyn = (amag > ONE_G_U_MS2) ? (amag - ONE_G_U_MS2) : (ONE_G_U_MS2 - amag);

    uint32_t events = 0;

    /* ---------------- Orientation: stable + confidence gated ---------------- */
    bool orient_changed = false;
    imu_orientation_t orient = update_orientation_filter(ax, ay, az, amag, dyn, &orient_changed);
    if (orient_changed) {
        events |= IMU_EVT_ORIENT_CHANGE;
    }

    /* ---------------- Kick ---------------- */
    if (dyn > KICK_THR_U_MS2 && (now - s.last_kick_ms) > KICK_DEBOUNCE_MS) {
        s.kick_count++;
        s.last_kick_ms = now;
        events |= IMU_EVT_KICK;
        kick_buf_push(now);
    }

    kick_buf_purge_older_than_60s(now);
    uint16_t kick_rate = s.kick_len; /* kicks in last 60s */

    /* ---------------- Free-fall + impact => fall-like ---------------- */
    bool freefall_event = false;
    bool fall_like_event = false;

    /* ---- Freefall episode ---- */
    if (amag < FREEFALL_THR_U_MS2) {
        if (!s.in_freefall) {
            s.in_freefall = true;
            s.freefall_start_ms = now;
            s.freefall_reported = false;
        } else {
            if (!s.freefall_reported && (now - s.freefall_start_ms) >= FREEFALL_MIN_MS) {
                s.freefall_reported = true;
                freefall_event = true;         /* edge */
                events |= IMU_EVT_FREEFALL;
                s.freefall_recent_ms = now;    /* remember for fall-like */
            }
        }
    } else {
        /* leaving freefall: if it lasted long enough, mark it as "recent" */
        if (s.in_freefall && (now - s.freefall_start_ms) >= FREEFALL_MIN_MS) {
            s.freefall_recent_ms = now;
        }
        s.in_freefall = false;
        s.freefall_reported = false;
    }

    /* ---- Impact (edge) ---- */
    if (amag > IMPACT_THR_U_MS2) {
        if (!s.impact_reported) {
            s.impact_reported = true;
            events |= IMU_EVT_IMPACT;

            if ((now - s.freefall_recent_ms) < 1000) {
                fall_like_event = true;
                events |= IMU_EVT_FALL_LIKE;
            }
        }
    } else {
        s.impact_reported = false;
    }

    /* ---------------- Stillness / asleep-like ---------------- */
    bool still_candidate = (dyn < STILL_DYN_U_MS2 && gmag < STILL_GYRO_U_RADS);

    bool still = false;
    if (still_candidate) {
        if (s.still_start_ms == 0) s.still_start_ms = now;

        if ((now - s.still_start_ms) >= STILL_MIN_MS) {
            still = true;

            /* STILL event only once on entry */
            if (!s.still_latched) {
                s.still_latched = true;
                events |= IMU_EVT_STILL;
                s.asleep_latched_ms = now; /* start asleep timer at still entry */
            }
        }
    } else {
        s.still_start_ms = 0;
        s.still_latched = false;
        s.asleep_latched_ms = 0;
    }

    int8_t asleep_like = 0;
    if (still && s.asleep_latched_ms != 0) {
        if ((now - s.asleep_latched_ms) >= ASLEEP_MIN_MS) {
            asleep_like = 1;
        }
    }

    if (s.asleep_prev != -1 && asleep_like != s.asleep_prev) {
        events |= IMU_EVT_ASLEEP_CHANGE;
    }
    s.asleep_prev = asleep_like;

    /* ---------------- Activity level (0..3) ---------------- */
    uint8_t activity = 0;
    if (dyn < STILL_DYN_U_MS2 && gmag < STILL_GYRO_U_RADS) {
        activity = 0;
    } else if (dyn < (ONE_G_U_MS2 / 10U)) {
        activity = 1;
    } else if (dyn < (ONE_G_U_MS2 / 4U)) {
        activity = 2;
    } else {
        activity = 3;
    }

    /* Fill output */
    out->sample_seq = g_seq++;

    out->acc_u_ms2[0] = ax;
    out->acc_u_ms2[1] = ay;
    out->acc_u_ms2[2] = az;

    out->gyr_u_rads[0] = gx;
    out->gyr_u_rads[1] = gy;
    out->gyr_u_rads[2] = gz;

    out->activity_level = activity;
    out->kick_count = s.kick_count;
    out->kick_rate_per_min = kick_rate;

    out->asleep_like = asleep_like;
    out->orientation = (int8_t)orient;
    out->freefall_event = (int8_t)freefall_event;
    out->fall_like_event = (int8_t)fall_like_event;

    out->events = events;

    return 0;
}

/* --- IMU Thread --- */
#define IMU_THREAD_STACK 1024
#define IMU_THREAD_PRIO  6

K_THREAD_STACK_DEFINE(s_imu_stack, IMU_THREAD_STACK);
static struct k_thread s_imu_thread;
static bool s_thread_running;

static void imu_thread_fn(void *a, void *b, void *c)
{
    ARG_UNUSED(a); ARG_UNUSED(b); ARG_UNUSED(c);

    lsm6dsv32x_sample_t s;

    while (1) {

        if (max77658_shutdown_requested()) {
            k_sleep(K_MSEC(500));
            continue;
        }

        k_mutex_lock(&i2c_lock, K_FOREVER);
        int rc = lsm6dsv32x_measure(&s);
        k_mutex_unlock(&i2c_lock);

        int sleep_ms = 40; /* default active */

        if (rc == 0) {

            uint16_t alerts = 0;
            if (s.orientation == IMU_ORIENT_DOWN) alerts |= AL_ROLLOVER;
            if (s.events & IMU_EVT_FREEFALL)  alerts |= AL_FREEFALL;
            if (s.events & IMU_EVT_IMPACT)    alerts |= AL_IMPACT;
            if (s.events & IMU_EVT_FALL_LIKE) alerts |= AL_FALL_LIKE;

            data_manager_update_imu((int8_t)s.orientation,
                                    (uint8_t)s.activity_level,
                                    (uint16_t)s.kick_count,
                                    (uint8_t)s.kick_rate_per_min,
                                    (uint8_t)s.asleep_like,
                                    (uint32_t)s.events,
                                    alerts,
                                    true);

            if (s.events & IMU_EVT_ORIENT_CHANGE) {
                LOG_INF("POSTURE: %d", (int)s.orientation);
                if (s.orientation == IMU_ORIENT_DOWN) {
                    LOG_WRN("ALERT: Baby rolled onto stomach! (confirm axis mapping)");
                }
            }

            if (s.events & IMU_EVT_KICK) {
                LOG_INF("KICK: Count=%u Rate=%u/min",
                        (unsigned int)s.kick_count,
                        (unsigned int)s.kick_rate_per_min);
            }

            if (s.events & IMU_EVT_FALL_LIKE) {
                LOG_ERR("CRITICAL: Drop/Fall detected!");
            } else {
                if (s.events & IMU_EVT_FREEFALL) LOG_WRN("FREEFALL detected");
                if (s.events & IMU_EVT_IMPACT)   LOG_WRN("IMPACT detected");
            }

            if (s.events & IMU_EVT_ASLEEP_CHANGE) {
                LOG_INF("STATUS: Baby is now %s",
                        s.asleep_like ? "Asleep" : "Awake/Active");
            }

            if (s.events & IMU_EVT_STILL) {
                LOG_DBG("STILL: entered stillness state");
            }

            sleep_ms = s.asleep_like ? 200 : 40;

        } else {
            /* Back off if read fails, avoid using stale state */
            sleep_ms = 100;
        }

        k_sleep(K_MSEC(sleep_ms));
    }
}

int lsm6dsv32x_start(void)
{
    if (s_thread_running) {
        return 0;
    }

    /* Ensure init done (safe to call multiple times) */
    int rc = lsm6dsv32x_init();
    if (rc < 0) {
        LOG_ERR("IMU init failed in start(): %d", rc);
        return rc;
    }

    k_thread_create(&s_imu_thread, s_imu_stack, IMU_THREAD_STACK,
                    imu_thread_fn, NULL, NULL, NULL,
                    IMU_THREAD_PRIO, 0, K_NO_WAIT);
    k_thread_name_set(&s_imu_thread, "imu_thread");

    s_thread_running = true;
    return 0;
}

void lsm6dsv32x_stop(void)
{
    /* Optional: implement later if you want to abort thread */
    /* For now, keep it simple: not stopping threads at runtime */
    s_thread_running = false;
}
