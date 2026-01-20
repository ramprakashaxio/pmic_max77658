#include <zephyr/types.h>
#include <stddef.h>
#include <string.h>
#include <errno.h>
#include <zephyr/sys/printk.h>
#include <zephyr/sys/byteorder.h>
#include <zephyr/sys/atomic.h>
#include <zephyr/sys/util.h>
#include <zephyr/net_buf.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/bluetooth/conn.h>
#include <zephyr/bluetooth/uuid.h>
#include <zephyr/bluetooth/gatt.h>
#include <zephyr/bluetooth/hci.h>
#include <zephyr/bluetooth/hci_vs.h>

#include "data_manager.h"
#include "storage_manager.h"
#include "max77658_main.h"
#include "ble_app.h"

LOG_MODULE_REGISTER(ble_app, LOG_LEVEL_INF);

/* ---------------- UUID Definitions ---------------- */
#define UUID_BASE_VAL(x) BT_UUID_128_ENCODE(0x12345678, 0x1234, 0x5678, 0x1234, (x))

static struct bt_uuid_128 hope_svc_uuid     = BT_UUID_INIT_128(UUID_BASE_VAL(0x56789abcdef0));
static struct bt_uuid_128 char_vitals_uuid  = BT_UUID_INIT_128(UUID_BASE_VAL(0x56789abcdef1));
static struct bt_uuid_128 char_imu_uuid     = BT_UUID_INIT_128(UUID_BASE_VAL(0x56789abcdef2));
static struct bt_uuid_128 char_temp_uuid    = BT_UUID_INIT_128(UUID_BASE_VAL(0x56789abcdef3));
static struct bt_uuid_128 char_batt_uuid    = BT_UUID_INIT_128(UUID_BASE_VAL(0x56789abcdef4));
static struct bt_uuid_128 char_profile_uuid = BT_UUID_INIT_128(UUID_BASE_VAL(0x56789abcdef5));
static struct bt_uuid_128 char_config_uuid  = BT_UUID_INIT_128(UUID_BASE_VAL(0x56789abcdef6));
static struct bt_uuid_128 char_info_uuid    = BT_UUID_INIT_128(UUID_BASE_VAL(0x56789abcdef7));

/* ---------------- Transmission Structs (Packed) ---------------- */
typedef struct __attribute__((packed)) {
    uint32_t ts_ms;
    uint16_t hr;
    uint16_t spo2;      /* spo2_x10 */
    uint8_t  hr_conf;
    uint8_t  spo2_conf;
    uint8_t  scd;
    uint8_t  algo_mode;
} ble_vitals_t;

typedef struct __attribute__((packed)) {
    uint32_t ts_ms;
    uint16_t kicks;
    uint8_t  orient;
    uint8_t  activity;
    uint8_t  sleep;
    uint32_t events;
} ble_imu_t;

typedef struct __attribute__((packed)) {
    uint32_t ts_ms;
    int16_t  temp_c100;
} ble_temp_t;

typedef struct __attribute__((packed)) {
    uint32_t ts_ms;
    uint16_t mv;
    int16_t  ma;      /* batt_ma_x10 */
    uint8_t  soc;     /* batt_pct */
    uint8_t  chg;     /* chg_present */
} ble_batt_t;

/* Config/Profile Structs */
typedef struct __attribute__((packed)) { uint8_t age; uint8_t height_cm; uint16_t weight_dg; uint8_t gender; } user_profile_t;
typedef struct __attribute__((packed)) { uint8_t op_mode; uint16_t interval_s; uint8_t led_current; } device_config_t;

static user_profile_t current_profile = {0};
static device_config_t current_config = {0, 0, 50};

/* Notification Flags */
#define NOTIFY_VITALS  (1u<<0)
#define NOTIFY_IMU     (1u<<1)
#define NOTIFY_TEMP    (1u<<2)
#define NOTIFY_BATT    (1u<<3)

static atomic_t g_notify_mask  = ATOMIC_INIT(0);
static atomic_t g_ble_stop     = ATOMIC_INIT(0);
static atomic_t g_force_flush  = ATOMIC_INIT(0);

K_SEM_DEFINE(bt_ready_sem, 0, 1);

/* Track active connection */
static struct bt_conn *g_conn;

/* ---------------- Connection callbacks ---------------- */
static void conn_set(struct bt_conn *conn)
{
    if (g_conn) {
        bt_conn_unref(g_conn);
        g_conn = NULL;
    }
    if (conn) {
        g_conn = bt_conn_ref(conn);
    }
}

static bool ble_ready_to_tx(void)
{
    return (g_conn != NULL) &&
           (atomic_get(&g_ble_stop) == 0) &&
           (atomic_get(&g_notify_mask) != 0);
}

static void set_tx_power_max(struct bt_conn *conn)
{
    struct net_buf *buf;
    struct bt_hci_cp_vs_write_tx_power_level *cp;
    uint16_t handle;
    int err;

    err = bt_hci_get_conn_handle(conn, &handle);
    if (err) {
        LOG_WRN("Failed to get conn handle: %d", err);
        return;
    }

    buf = bt_hci_cmd_create(BT_HCI_OP_VS_WRITE_TX_POWER_LEVEL, sizeof(*cp));
    if (!buf) {
        LOG_WRN("Failed to allocate HCI cmd buffer");
        return;
    }

    cp = net_buf_add(buf, sizeof(*cp));
    cp->handle = sys_cpu_to_le16(handle);
    cp->handle_type = BT_HCI_VS_LL_HANDLE_TYPE_CONN;
    cp->tx_power_level = 8; /* +8 dBm */

    err = bt_hci_cmd_send_sync(BT_HCI_OP_VS_WRITE_TX_POWER_LEVEL, buf, NULL);
    if (err) {
        LOG_WRN("TX power set failed: %d", err);
    } else {
        LOG_INF("TX power requested: +8 dBm (handle %u)", handle);
    }
}

static void connected(struct bt_conn *conn, uint8_t conn_err)
{
    if (conn_err) {
        LOG_WRN("BLE connect failed (err %u)", conn_err);
        return;
    }
    conn_set(conn);
    LOG_INF("BLE connected. RSSI: Checking...");

    /* 1. Force Max Power immediately */
    set_tx_power_max(conn);

    /* 2. Request 2M PHY (Better battery efficiency, good range) */
    /* Commented out - bt_conn_le_phy_update not available in this build */
    /*
    const struct bt_conn_le_phy_param phy_param = {
        .options = BT_CONN_LE_PHY_OPT_NONE,
        .pref_tx_phy = BT_GAP_LE_PHY_2M,
        .pref_rx_phy = BT_GAP_LE_PHY_2M,
    };
    bt_conn_le_phy_update(conn, &phy_param);
    */

    /* 3. Update Connection Params (50ms interval) */
    struct bt_le_conn_param conn_param = {
        .interval_min = 40,   /* 50ms */
        .interval_max = 60,   /* 75ms */
        .latency      = 0,
        .timeout      = 400   /* 4s timeout prevents random drops */
    };
    int rc = bt_conn_le_param_update(conn, &conn_param);
    if (rc) {
        LOG_WRN("Conn param update failed: %d", rc);
    }
}

static void disconnected(struct bt_conn *conn, uint8_t reason)
{
    ARG_UNUSED(conn);
    LOG_WRN("BLE disconnected (reason 0x%02x)", reason);

    /* CCC will likely be reset by client; ensure we stop TX immediately */
    atomic_set(&g_notify_mask, 0);
    atomic_set(&g_force_flush, 0);
    conn_set(NULL);
}

static struct bt_conn_cb conn_callbacks = {
    .connected = connected,
    .disconnected = disconnected,
};

/* ---------------- CCC Callbacks ---------------- */
void ble_prepare_for_shutdown(void)
{
    atomic_set(&g_ble_stop, 1);
    atomic_set(&g_notify_mask, 0);
    atomic_set(&g_force_flush, 0);

    (void)bt_le_adv_stop();
    conn_set(NULL);

    LOG_WRN("BLE: prepare_for_shutdown");
}

static void update_notify_mask(uint16_t value, uint32_t bit)
{
    if (value == BT_GATT_CCC_NOTIFY) {
        atomic_or(&g_notify_mask, (atomic_val_t)bit);
        atomic_set(&g_force_flush, 1);
    } else {
        atomic_and(&g_notify_mask, (atomic_val_t)~bit);
    }

    LOG_INF("Notify mask now: 0x%02x", (unsigned)atomic_get(&g_notify_mask));
}

static void ccc_vitals_changed(const struct bt_gatt_attr *attr, uint16_t value) { ARG_UNUSED(attr); update_notify_mask(value, NOTIFY_VITALS); }
static void ccc_imu_changed(const struct bt_gatt_attr *attr, uint16_t value)    { ARG_UNUSED(attr); update_notify_mask(value, NOTIFY_IMU); }
static void ccc_temp_changed(const struct bt_gatt_attr *attr, uint16_t value)   { ARG_UNUSED(attr); update_notify_mask(value, NOTIFY_TEMP); }
static void ccc_batt_changed(const struct bt_gatt_attr *attr, uint16_t value)   { ARG_UNUSED(attr); update_notify_mask(value, NOTIFY_BATT); }

/* Read/Write Handlers */
static ssize_t read_profile(struct bt_conn *conn, const struct bt_gatt_attr *attr,
                            void *buf, uint16_t len, uint16_t offset)
{
    return bt_gatt_attr_read(conn, attr, buf, len, offset, &current_profile, sizeof(current_profile));
}

static ssize_t write_profile(struct bt_conn *conn, const struct bt_gatt_attr *attr,
                             const void *buf, uint16_t len, uint16_t offset, uint8_t flags)
{
    ARG_UNUSED(conn); ARG_UNUSED(attr); ARG_UNUSED(offset); ARG_UNUSED(flags);
    if (len != sizeof(user_profile_t)) return BT_GATT_ERR(BT_ATT_ERR_INVALID_ATTRIBUTE_LEN);
    memcpy(&current_profile, buf, len);
    return len;
}

static ssize_t read_config(struct bt_conn *conn, const struct bt_gatt_attr *attr,
                           void *buf, uint16_t len, uint16_t offset)
{
    return bt_gatt_attr_read(conn, attr, buf, len, offset, &current_config, sizeof(current_config));
}

static ssize_t write_config(struct bt_conn *conn, const struct bt_gatt_attr *attr,
                            const void *buf, uint16_t len, uint16_t offset, uint8_t flags)
{
    ARG_UNUSED(conn); ARG_UNUSED(attr); ARG_UNUSED(offset); ARG_UNUSED(flags);
    if (len != sizeof(device_config_t)) return BT_GATT_ERR(BT_ATT_ERR_INVALID_ATTRIBUTE_LEN);
    memcpy(&current_config, buf, len);
    return len;
}

/* ---------------- GATT Service ---------------- */
BT_GATT_SERVICE_DEFINE(hope_svc,
    BT_GATT_PRIMARY_SERVICE(&hope_svc_uuid),

    /* 1. Vitals (Attr: 2) */
    BT_GATT_CHARACTERISTIC(&char_vitals_uuid.uuid, BT_GATT_CHRC_NOTIFY, BT_GATT_PERM_NONE, NULL, NULL, NULL),
    BT_GATT_CCC(ccc_vitals_changed, BT_GATT_PERM_READ | BT_GATT_PERM_WRITE),

    /* 2. IMU (Attr: 5) */
    BT_GATT_CHARACTERISTIC(&char_imu_uuid.uuid, BT_GATT_CHRC_NOTIFY, BT_GATT_PERM_NONE, NULL, NULL, NULL),
    BT_GATT_CCC(ccc_imu_changed, BT_GATT_PERM_READ | BT_GATT_PERM_WRITE),

    /* 3. Temp (Attr: 8) */
    BT_GATT_CHARACTERISTIC(&char_temp_uuid.uuid, BT_GATT_CHRC_NOTIFY, BT_GATT_PERM_NONE, NULL, NULL, NULL),
    BT_GATT_CCC(ccc_temp_changed, BT_GATT_PERM_READ | BT_GATT_PERM_WRITE),

    /* 4. Batt (Attr: 11) */
    BT_GATT_CHARACTERISTIC(&char_batt_uuid.uuid, BT_GATT_CHRC_NOTIFY, BT_GATT_PERM_NONE, NULL, NULL, NULL),
    BT_GATT_CCC(ccc_batt_changed, BT_GATT_PERM_READ | BT_GATT_PERM_WRITE),

    /* 5. Profile (RW) */
    BT_GATT_CHARACTERISTIC(&char_profile_uuid.uuid, BT_GATT_CHRC_READ | BT_GATT_CHRC_WRITE,
                           BT_GATT_PERM_READ | BT_GATT_PERM_WRITE, read_profile, write_profile, NULL),

    /* 6. Config (RW) */
    BT_GATT_CHARACTERISTIC(&char_config_uuid.uuid, BT_GATT_CHRC_READ | BT_GATT_CHRC_WRITE,
                           BT_GATT_PERM_READ | BT_GATT_PERM_WRITE, read_config, write_config, NULL),

    /* 7. Info (RO) */
    BT_GATT_CHARACTERISTIC(&char_info_uuid.uuid, BT_GATT_CHRC_READ, BT_GATT_PERM_READ, NULL, NULL, NULL)
);

static const struct bt_data ad[] = {
    BT_DATA_BYTES(BT_DATA_FLAGS, (BT_LE_AD_GENERAL | BT_LE_AD_NO_BREDR)),
    BT_DATA(BT_DATA_NAME_COMPLETE, "Neso Monitor", (sizeof("Neso Monitor") - 1)),
};

static const struct bt_data sd[] = {
    BT_DATA_BYTES(BT_DATA_UUID128_ALL,
        0xf0, 0xde, 0xbc, 0x9a, 0x78, 0x56, 0x34, 0x12,
        0x78, 0x56, 0x34, 0x12, 0x78, 0x56, 0x34, 0x12),
};

static void bt_ready(int err)
{
    if (err) {
        LOG_ERR("BLE init failed (err %d)", err);
        return;
    }

    bt_conn_cb_register(&conn_callbacks);

    err = bt_le_adv_start(BT_LE_ADV_CONN, ad, ARRAY_SIZE(ad), sd, ARRAY_SIZE(sd));
    if (err) {
        LOG_ERR("Advertising failed (err %d)", err);
        return;
    }

    LOG_INF("Advertising started");
    k_sem_give(&bt_ready_sem);
}

/* ---------------- Logic: Send ---------------- */

#define BURST_INTERVAL_MS 10000
#define SAMPLE_PACING_MS  12   /* between samples */
#define FIELD_PACING_MS   2    /* between fields (vitals/imu/temp/batt) */
#define MAX_BATCHES_PER_FLUSH 10  /* Limit catch-up to avoid hogging radio */

/* Retry on -ENOMEM (controller buffers full) */
static int send_safe(const struct bt_gatt_attr *attr, const void *data, uint16_t len)
{
    for (int tries = 0; tries < 25; tries++) {
        if (!ble_ready_to_tx()) return -EACCES;

        /* Use g_conn explicitly to avoid race conditions with NULL */
        int err = bt_gatt_notify(g_conn, attr, data, len);
        if (err == 0) return 0;

        if (err == -ENOMEM) {
            k_sleep(K_MSEC(10));
            continue;
        }

        /* -ENOTCONN etc */
        return err;
    }

    return -ETIMEDOUT;
}

static int ble_process_batch(const patient_batch_t *b)
{
    uint32_t mask = atomic_get(&g_notify_mask);

    /* 1) Vitals: send all samples (high-rate channel) */
    if (mask & NOTIFY_VITALS) {
        for (int i = 0; i < b->count; i++) {
            if (!ble_ready_to_tx()) return -EACCES;

            const patient_sample_t *s = &b->samples[i];

            ble_vitals_t v = {
                .ts_ms = s->ts_ms,
                .hr = s->hr_bpm,
                .spo2 = s->spo2_x10,
                .hr_conf = s->hr_conf,
                .spo2_conf = s->spo2_conf,
                .scd = s->scd,
                .algo_mode = b->algo_mode
            };

            int err = send_safe(&hope_svc.attrs[2], &v, sizeof(v));
            if (err) return err;

            k_sleep(K_MSEC(10)); /* pacing between vitals samples */
        }
    }

    /* Latest sample for slow channels */
    const patient_sample_t *last = &b->samples[b->count - 1];

    /* 2) IMU: once per batch */
    if (mask & NOTIFY_IMU) {
        ble_imu_t im = {
            .ts_ms = last->ts_ms,
            .kicks = last->kick_count,
            .orient = (uint8_t)last->orientation,
            .activity = last->activity_level,
            .sleep = last->asleep_like,
            .events = last->imu_events
        };
        int err = send_safe(&hope_svc.attrs[5], &im, sizeof(im));
        if (err) return err;
        k_sleep(K_MSEC(5));
    }

    /* 3) TEMP: once per batch */
    if (mask & NOTIFY_TEMP) {
        ble_temp_t t = { .ts_ms = last->ts_ms, .temp_c100 = last->temp_c_x100 };
        int err = send_safe(&hope_svc.attrs[8], &t, sizeof(t));
        if (err) return err;
        k_sleep(K_MSEC(5));
    }

    /* 4) BATT: once per batch */
    if (mask & NOTIFY_BATT) {
        ble_batt_t bat = {
            .ts_ms = last->ts_ms,
            .mv = last->batt_mv,
            .ma = last->batt_ma_x10,
            .soc = last->batt_pct,
            .chg = last->chg_present
        };
        int err = send_safe(&hope_svc.attrs[11], &bat, sizeof(bat));
        if (err) return err;
    }

    return 0;
}

static void ble_flush_loop(void)
{
    static patient_batch_t b; /* avoid stack pressure */
    int sent = 0;

    if (!ble_ready_to_tx()) return;

    LOG_INF("BLE flush start: pending=%u", (unsigned)storage_pending_count());

    while (ble_ready_to_tx() && storage_has_data() && sent < MAX_BATCHES_PER_FLUSH) {
        if (storage_peek_next_batch(&b) != 0) {
            break;
        }

        int err = ble_process_batch(&b);
        if (err == 0) {
            (void)storage_drop_next_batch(); /* delete only on success */
            sent++;

            /* give time for supervision/control traffic */
            k_sleep(K_MSEC(30));
        } else {
            LOG_WRN("BLE send failed (%d). Stop flush (keep data).", err);
            break;
        }

        k_yield();
    }

    if (sent > 0) {
        LOG_INF("BLE flush done: sent=%d remaining=%u", sent, (unsigned)storage_pending_count());
    }
}

/* ---------------- Ingest Thread ---------------- */
#define INGEST_STACK 4096
K_THREAD_STACK_DEFINE(ingest_stack, INGEST_STACK);
static struct k_thread ingest_thread;

static void ingest_fn(void *a, void *b, void *c)
{
    ARG_UNUSED(a); ARG_UNUSED(b); ARG_UNUSED(c);

    static patient_batch_t in;

    while (1) {
        if (data_manager_get_ble_batch(&in) == 0) {

            /* If link ready, try LIVE TX first */
            if (ble_ready_to_tx()) {
                int err = ble_process_batch(&in);
                if (err == 0) {
                    continue; /* live sent, don't store */
                }
                /* If TX failed, fall back to storage */
                LOG_WRN("Live TX failed (%d), storing", err);
            }

            /* Not connected / not subscribed / TX failed -> store */
            (void)storage_save_batch(&in);
        }
    }
}

void start_ble_thread(void)
{
    if (storage_init() != 0) {
        LOG_ERR("Storage init failed. BLE buffering will not work.");
    }

    int err = bt_enable(bt_ready);
    if (err) {
        LOG_ERR("bt_enable failed: %d", err);
        return;
    }
    k_sem_take(&bt_ready_sem, K_FOREVER);

    k_thread_create(&ingest_thread, ingest_stack, K_THREAD_STACK_SIZEOF(ingest_stack),
                    ingest_fn, NULL, NULL, NULL,
                    6, 0, K_NO_WAIT);
    k_thread_name_set(&ingest_thread, "ble_ingest");

    int64_t last_burst = k_uptime_get();

    while (1) {
        if (max77658_shutdown_requested() || atomic_get(&g_ble_stop)) {
            k_sleep(K_SECONDS(1));
            continue;
        }

        if (ble_ready_to_tx()) {
            bool force = atomic_cas(&g_force_flush, 1, 0);
            int64_t now = k_uptime_get();

            if (force || ((now - last_burst) >= BURST_INTERVAL_MS)) {
                ble_flush_loop();
                last_burst = k_uptime_get();
            }
        }

        k_sleep(K_MSEC(500));
    }
}