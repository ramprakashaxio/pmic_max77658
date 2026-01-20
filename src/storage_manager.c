#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/flash.h>
#include <zephyr/fs/nvs.h>
#include <zephyr/logging/log.h>
#include <zephyr/storage/flash_map.h>
#include <zephyr/devicetree.h>
#include <errno.h>
#include <string.h>

#include "storage_manager.h"

LOG_MODULE_REGISTER(storage_mgr, LOG_LEVEL_INF);

static struct nvs_fs fs;

#define STORAGE_NODE DT_NODE_BY_FIXED_PARTITION_LABEL(storage)

/* * PARTITION: 32KB (0x8000)
 * SAFE CAPACITY: ~15KB of raw data (due to NVS wear-leveling overhead)
 * 15KB / ~260 bytes per batch = ~57 batches.
 * Rounding down to 50 for safety.
 */
#define MAX_STORED_BATCHES 50  /* Approx 20 seconds of buffer */

enum {
    STORAGE_PART_BYTES = DT_REG_SIZE(STORAGE_NODE)
};

#define KEY_WRITE_IDX  1
#define KEY_READ_IDX   2
#define KEY_DATA_BASE  100

static uint16_t write_idx;
static uint16_t read_idx;
static bool storage_ready;

static struct k_mutex st_lock;

/* Helper to wrap index */
static inline uint16_t next_idx(uint16_t idx)
{
    return (uint16_t)((idx + 1U) % (uint16_t)MAX_STORED_BATCHES);
}

uint16_t storage_pending_count(void)
{
    uint16_t pending;

    k_mutex_lock(&st_lock, K_FOREVER);
    if (write_idx >= read_idx) {
        pending = (uint16_t)(write_idx - read_idx);
    } else {
        pending = (uint16_t)((uint16_t)MAX_STORED_BATCHES - read_idx + write_idx);
    }
    k_mutex_unlock(&st_lock);

    return pending;
}

bool storage_has_data(void)
{
    bool has;

    k_mutex_lock(&st_lock, K_FOREVER);
    has = (read_idx != write_idx);
    k_mutex_unlock(&st_lock);

    return has;
}

int storage_init(void)
{
    int rc;
    struct flash_pages_info info;

    k_mutex_init(&st_lock);
    storage_ready = false;
    write_idx = 0;
    read_idx = 0;

    fs.flash_device = DEVICE_DT_GET(DT_CHOSEN(zephyr_flash_controller));
    if (!device_is_ready(fs.flash_device)) {
        LOG_ERR("Flash device not ready");
        return -ENODEV;
    }

    fs.offset = DT_REG_ADDR(STORAGE_NODE);

    rc = flash_get_page_info_by_offs(fs.flash_device, fs.offset, &info);
    if (rc) {
        LOG_ERR("Unable to get page info: %d", rc);
        return rc;
    }

    fs.sector_size = info.size;
    fs.sector_count = STORAGE_PART_BYTES / info.size;

    if (fs.sector_count < 2) {
        LOG_ERR("Storage partition too small: bytes=%d sector=%d count=%d",
                STORAGE_PART_BYTES, (int)fs.sector_size, (int)fs.sector_count);
        return -EINVAL;
    }

    rc = nvs_mount(&fs);
    if (rc) {
        LOG_ERR("NVS mount failed: %d (check partition label/size)", rc);
        return rc;
    }

    /* Restore indices */
    if (nvs_read(&fs, KEY_WRITE_IDX, &write_idx, sizeof(write_idx)) <= 0) {
        write_idx = 0;
    }
    if (nvs_read(&fs, KEY_READ_IDX, &read_idx, sizeof(read_idx)) <= 0) {
        read_idx = 0;
    }

    /* Clamp indices to current ring size */
    write_idx %= (uint16_t)MAX_STORED_BATCHES;
    read_idx  %= (uint16_t)MAX_STORED_BATCHES;

    storage_ready = true;

    LOG_INF("Storage: partition=%dB page=%dB sectors=%d, batch_size=%uB, ring=%d",
            STORAGE_PART_BYTES, (int)fs.sector_size, (int)fs.sector_count,
            (unsigned)sizeof(patient_batch_t), (int)MAX_STORED_BATCHES);

    LOG_INF("Storage: pending=%u (read=%u write=%u)", storage_pending_count(), read_idx, write_idx);

    return 0;
}

int storage_save_batch(const patient_batch_t *batch)
{
    int rc;
    uint16_t saved_at;
    uint16_t next_write;

    if (!storage_ready) return -EACCES;

    k_mutex_lock(&st_lock, K_FOREVER);

    next_write = next_idx(write_idx);

    /* Overflow: drop oldest */
    if (next_write == read_idx) {
        read_idx = next_idx(read_idx);
        (void)nvs_write(&fs, KEY_READ_IDX, &read_idx, sizeof(read_idx));
    }

    saved_at = write_idx;

    rc = nvs_write(&fs, KEY_DATA_BASE + saved_at, batch, sizeof(patient_batch_t));
    if (rc < 0) {
        k_mutex_unlock(&st_lock);
        LOG_ERR("NVS write failed: %d", rc);
        return rc;
    }

    write_idx = next_write;
    (void)nvs_write(&fs, KEY_WRITE_IDX, &write_idx, sizeof(write_idx));

    k_mutex_unlock(&st_lock);
    return 0;
}

int storage_peek_next_batch(patient_batch_t *batch)
{
    int rc;

    if (!storage_ready) return -EACCES;

    /* Don’t hold lock longer than needed. */
    k_mutex_lock(&st_lock, K_FOREVER);

    if (read_idx == write_idx) {
        k_mutex_unlock(&st_lock);
        return -ENODATA;
    }

    rc = nvs_read(&fs, KEY_DATA_BASE + read_idx, batch, sizeof(patient_batch_t));
    if (rc <= 0) {
        /* Corrupt entry: skip it so we don’t get stuck forever */
        LOG_WRN("NVS read failed at idx=%u, skipping entry", read_idx);
        read_idx = next_idx(read_idx);
        (void)nvs_write(&fs, KEY_READ_IDX, &read_idx, sizeof(read_idx));
        k_mutex_unlock(&st_lock);
        return -EIO;
    }

    k_mutex_unlock(&st_lock);
    return 0;
}

int storage_drop_next_batch(void)
{
    if (!storage_ready) return -EACCES;

    k_mutex_lock(&st_lock, K_FOREVER);

    if (read_idx == write_idx) {
        k_mutex_unlock(&st_lock);
        return -ENODATA;
    }

    read_idx = next_idx(read_idx);
    (void)nvs_write(&fs, KEY_READ_IDX, &read_idx, sizeof(read_idx));

    k_mutex_unlock(&st_lock);
    return 0;
}
