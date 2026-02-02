# Data Manager & Storage Documentation

## Overview

The Data Manager (`data_manager.c`) is the central aggregation point for all sensor data in the Neso Monitor firmware. It collects data from multiple sources (MAX32664C vitals sensor, LSM6DSV32X IMU, TMP117 temperature sensor, MAX77658 fuel gauge) and packages them into structured batches for transmission over BLE and future cloud connectivity.

---

## 💾 Data Storage & Memory

### Sample Size
- **One `patient_sample_t`**: **36 bytes** (packed, no padding)
  - Breakdown: ts_ms(4) + vitals(12) + temp(2) + battery(6) + IMU(12) = 36 bytes
- **One `patient_batch_t`**: **372 bytes**
  - Header: 12 bytes (ver, count, algo_mode, rsv0, batch_seq, uptime_ms)
  - Samples: 10 × 36 bytes = 360 bytes

### Internal Flash Storage (nRF54L15)

**Partition Configuration:**
```
Storage Partition: 32 KB (0x8000 bytes)
Location: 0xF0000 - 0xF7FFF (in flash memory map)
Label: "storage"
```

**Storage Capacity:**
- **Effective capacity**: ~15 KB usable data (NVS wear-leveling overhead ~17KB)
- **Maximum batches**: 50 batches (configured in `storage_manager.c`)
- **Buffer duration (continuous mode)**: **~20 seconds** @ 1 Hz vitals
- **Buffer duration (periodic mode)**: **~50 seconds** @ 1 sample/batch

**Calculation:**
```
50 batches × 372 bytes = 18,600 bytes (~18.1 KB raw data)
NVS overhead: ~2-3 KB metadata + wear leveling space
Total: fits comfortably in 32 KB partition
```

**Storage Behavior:**
- **Type**: Ring buffer (circular queue)
- **Overflow**: Oldest data automatically dropped when full
- **Persistence**: Data survives power cycles and reboots
- **Thread-safety**: Mutex-protected read/write operations
- **Flash wear endurance**: ~10 years @ 24/7 disconnected use

**Data Retention Scenarios:**

| Scenario | Duration | Batches | Total Samples |
|----------|----------|---------|---------------|
| **Continuous mode** (1 Hz, 10 samples/batch) | ~20 seconds | 50 | 500 samples |
| **Periodic mode** (1 sample/batch) | ~50 seconds | 50 | 50 samples |
| **Connection loss during operation** | Up to 20 sec | 50 max | Auto-overflow |
| **Power off then restart** | Infinite | Preserved | Until next boot |

**Memory Allocation Summary:**

| Component | Size | Purpose |
|-----------|------|---------|
| **BLE Queue (RAM)** | 16 batches × 372 bytes = **~6 KB** | Fast BLE transmission buffer |
| **Cloud Queue (RAM)** | 4 batches × 372 bytes = **~1.5 KB** | Future WiFi/cellular buffer |
| **Flash (NVS)** | 32 KB partition | Persistent offline storage |
| **Total buffering** | **~39.5 KB** | Combined RAM + Flash |

**Flash Write Frequency:**
- **When connected**: 0 writes/sec (live streaming, no storage)
- **When disconnected**: ~1 write/sec (continuous mode)
- **Flash lifetime**: >100,000 write cycles per sector
- **Expected lifespan**: >10 years with wear leveling

---

## 📊 Data Structures

### `patient_sample_t` (36 bytes, packed)

One timestamped reading containing all sensor values:

```c
typedef struct __attribute__((packed)) {
    uint32_t ts_ms;        // Timestamp (k_uptime_get_32()) - 4 bytes

    /* ---- MAX32664C Vitals ---- */
    uint16_t hr_bpm;       // Heart rate - 2 bytes
    uint16_t spo2_x10;     // SpO2 × 10 (985 = 98.5%) - 2 bytes
    uint16_t rr_x10;       // Respiration rate × 10 - 2 bytes
    uint8_t  hr_conf;      // HR confidence (0-100) - 1 byte
    uint8_t  spo2_conf;    // SpO2 confidence (0-100) - 1 byte
    uint8_t  scd;          // Skin contact (3 = ON_SKIN) - 1 byte
    uint8_t  rsv0;         // Reserved - 1 byte

    /* ---- MAX30208 Temperature ---- */
    int16_t  temp_c_x100;  // Temperature × 100 (2514 = 25.14°C) - 2 bytes
                           // INT16_MIN = invalid

    /* ---- MAX77658 Battery ---- */
    uint8_t  batt_pct;     // State of charge (0-100%) - 1 byte
    uint8_t  chg_present;  // Charger connected (0/1) - 1 byte
    uint16_t batt_mv;      // Battery voltage (mV) - 2 bytes
    int16_t  batt_ma_x10;  // Current × 10 (signed, mA) - 2 bytes

    /* ---- LSM6DSV32X IMU ---- */
    int8_t   orientation;      // Roll/prone/supine - 1 byte
    uint8_t  activity_level;   // Activity (0-3) - 1 byte
    uint16_t kick_count;       // Cumulative kicks - 2 bytes
    uint8_t  kick_rate_per_min;// Kicks per minute - 1 byte
    uint8_t  asleep_like;      // Sleep detected (0/1) - 1 byte
    uint32_t imu_events;       // Event bitmask - 4 bytes
    uint16_t alerts;           // Alert bitmask - 2 bytes
    uint8_t  valid_flags;      // VF_* flags - 1 byte
    uint8_t  rsv1;             // Reserved - 1 byte
} patient_sample_t;  // Total: 36 bytes
```

**Valid Flags:**
```c
#define VF_VITALS   (1U << 0)  // HR/SpO2/RR/Conf/SCD valid
#define VF_TEMP     (1U << 1)  // Temperature valid
#define VF_BATT     (1U << 2)  // Battery data valid
#define VF_IMU      (1U << 3)  // IMU data valid
```

**Alert Flags:**
```c
#define AL_ROLLOVER   (1U << 0)  // Baby rolled over
#define AL_FALL_LIKE  (1U << 1)  // Fall detected
#define AL_FREEFALL   (1U << 2)  // Freefall detected
#define AL_IMPACT     (1U << 3)  // Impact detected
```

### `patient_batch_t` (372 bytes, packed)

A batch of 1-10 samples with metadata:

```c
typedef struct __attribute__((packed)) {
    uint8_t  ver;           // Protocol version (PAYLOAD_VER = 1)
    uint8_t  count;         // Number of samples (1-10)
    uint8_t  algo_mode;     // MAX32664C algorithm mode
    uint8_t  rsv0;          // Reserved
    
    uint32_t batch_seq;     // Sequence number (increments per batch)
    uint32_t uptime_ms;     // System uptime snapshot
    
    patient_sample_t samples[10];  // Array of samples
} patient_batch_t;  // Total: 12 + (10 × 36) = 372 bytes
```

---

## 🔄 Architecture

### Message Queues

Two separate queues prevent slow cloud uploads from blocking fast BLE transmission:

```c
K_MSGQ_DEFINE(batch_q,  sizeof(patient_batch_t), 4, 4);   // Cloud queue
K_MSGQ_DEFINE(ble_msgq, sizeof(patient_batch_t), 16, 4);  // BLE queue
```

- **Cloud Queue**: 4 batches deep (future WiFi/cellular)
- **BLE Queue**: 16 batches deep (absorbs bursts during flash writes)

### Data Flow

```
┌─────────────────────────────────────────────────────────┐
│  Sensor Threads (MAX32664C, LSM6DSV32X, etc)           │
└──────────┬──────────────────────────────────────────────┘
           │
           v
┌──────────────────────────────────────────────────────────┐
│  Data Manager (Aggregation & Batching)                   │
│  • Caches slow sensor data (temp, battery, IMU)         │
│  • Triggered by vitals (1 Hz)                           │
│  • Assembles batches (1-10 samples)                     │
└──────────┬───────────────────────────────────────────────┘
           │
           ├─────> batch_q (Cloud) ──> Future WiFi/Cellular
           │
           └─────> ble_msgq ──────────> BLE Ingest Thread
                                              │
                          ┌───────────────────┴────────────────┐
                          │                                    │
                          v                                    v
                   [Connected?]                         [Disconnected]
                          │                                    │
                          v                                    v
                   Live BLE TX                      Storage Manager (NVS)
                   (notify)                         [Ring Buffer: 50 batches]
                                                           │
                                                           v
                                                    [Reconnect] ──> Flush Loop
```

---

## 🔧 API Reference

### Initialization

```c
void data_manager_init(void);
```
- Initializes mutexes and queues
- Resets batch counters
- Must be called before any other data manager functions

### Configuration

```c
void data_manager_set_batch_target(uint8_t target_count);
```
- Sets batch size (1-10 samples)
- **10 samples**: Continuous mode (flush every 10 seconds @ 1 Hz)
- **1 sample**: Periodic mode (flush immediately)

### Sensor Updates (Cached)

These functions cache latest values from slow sensors:

```c
void data_manager_update_temp(int16_t temp_c_x100, bool valid);
void data_manager_update_batt(uint8_t batt_pct, uint16_t batt_mv, 
                              int16_t batt_ma_x10, uint8_t chg_present, bool valid);
void data_manager_update_imu(int8_t orientation, uint8_t activity_level,
                             uint16_t kick_count, uint8_t kick_rate_per_min,
                             uint8_t asleep_like, uint32_t imu_events,
                             uint16_t alerts, bool valid);
```

**Behavior:**
- Thread-safe (mutex-protected)
- Non-blocking
- Values merged into samples when vitals arrive

### Vitals Push (Triggers Batching)

```c
void data_manager_push_vitals(uint8_t algo_mode,
                              uint16_t hr_bpm, uint8_t hr_conf,
                              uint16_t spo2_x10, uint8_t spo2_conf,
                              uint16_t rr_x10, uint8_t scd);
```

**Called by**: MAX32664C thread (1 Hz in continuous mode)

**Behavior:**
1. Creates new `patient_sample_t` with timestamp
2. Merges cached temp/battery/IMU values
3. Adds sample to current batch
4. If `batch.count >= g_target_count`, finalizes and queues batch
5. Pushes to both `batch_q` and `ble_msgq`

### Consumer Functions

```c
int data_manager_get_batch(patient_batch_t *batch);      // Cloud (future)
int data_manager_get_ble_batch(patient_batch_t *batch);  // BLE
```

**Behavior:**
- Blocking wait (`K_FOREVER`)
- Returns 0 on success
- Copies batch data to provided buffer

---

## 🔒 Thread Safety

### Mutex Protection

All cached sensor data is protected by `dm_lock`:

```c
static struct k_mutex dm_lock;

k_mutex_lock(&dm_lock, K_FOREVER);
// ... access latest.temp_c_x100, latest.batt_pct, etc ...
k_mutex_unlock(&dm_lock);
```

### Queue Discipline

- **Producers**: Sensor threads (via `data_manager_push_vitals`)
- **Consumers**: BLE thread, Cloud thread (future)
- **Overflow**: Drops batch with warning log

---

## 📈 Batching Logic

### Batch Assembly

```c
static patient_batch_t current_batch;
static uint8_t g_target_count = BATCH_SIZE;  // Default: 10
```

**Process:**
1. Each vitals sample increments `current_batch.count`
2. When `count >= g_target_count`:
   - Assign sequence number (`g_batch_seq++`)
   - Snapshot system uptime
   - Push to both queues
   - Reset `current_batch.count = 0`

### Finalization

```c
static void finalize_and_queue_batch(uint8_t flushed_count)
{
    current_batch.ver      = PAYLOAD_VER;
    current_batch.batch_seq = g_batch_seq++;
    current_batch.uptime_ms = k_uptime_get_32();
    
    k_msgq_put(&batch_q, &current_batch, K_NO_WAIT);
    k_msgq_put(&ble_msgq, &current_batch, K_NO_WAIT);
    
    current_batch.count = 0;  // Reset
}
```

---

## 🛠️ Storage Manager Integration

### Storage Functions (from `storage_manager.c`)

```c
// Save batch to persistent storage (NVS)
int storage_save_batch(const patient_batch_t *batch);

// Peek oldest batch (non-destructive read)
int storage_peek_next_batch(patient_batch_t *batch);

// Delete oldest batch (after successful transmission)
int storage_drop_next_batch(void);

// Query functions
bool storage_has_data(void);              // Returns: read_idx != write_idx
uint16_t storage_pending_count(void);     // Returns: batches waiting
```

### Usage Pattern (BLE Ingest Thread)

```c
while (1) {
    patient_batch_t batch;
    
    // Wait for batch from data manager
    data_manager_get_ble_batch(&batch);
    
    // Try live transmission first
    if (ble_ready_to_tx()) {
        int err = ble_process_batch(&batch);
        if (err == 0) {
            continue;  // Success, no storage needed
        }
    }
    
    // Fallback: store to flash
    storage_save_batch(&batch);
}
```

### Catch-Up Sync

```c
while (storage_has_data() && ble_ready_to_tx()) {
    patient_batch_t batch;
    
    storage_peek_next_batch(&batch);  // Non-destructive read
    
    int err = ble_process_batch(&batch);
    if (err == 0) {
        storage_drop_next_batch();    // Delete on success
    } else {
        break;  // Keep data, retry later
    }
}
```

---

## 📝 Configuration

### Build Configuration (`prj.conf`)

```ini
# Non-Volatile Storage (NVS)
CONFIG_NVS=y
CONFIG_FLASH=y
CONFIG_FLASH_MAP=y
```

### Device Tree (`bl54l15_dvk_nrf54l15_cpuapp.overlay`)

```dts
storage_partition: partition@F0000 {
    label = "storage";
    reg = <0x000F0000 0x00008000>;  /* 32 KB */
};
```

### Code Configuration

```c
#define BATCH_SIZE   10       // Max samples per batch
#define PAYLOAD_VER  1        // Protocol version
#define MAX_STORED_BATCHES 50 // Flash storage capacity
```

---

## 🧪 Testing & Validation

### Verify Sample Size

```c
#include <stdio.h>
#include "data_manager.h"

void test_sizes(void) {
    printf("patient_sample_t: %zu bytes\n", sizeof(patient_sample_t));  // 36
    printf("patient_batch_t: %zu bytes\n", sizeof(patient_batch_t));    // 372
}
```

### Verify Storage Capacity

```c
void test_storage(void) {
    uint32_t total = MAX_STORED_BATCHES * sizeof(patient_batch_t);
    printf("Storage capacity: %u batches = %u bytes\n", 
           MAX_STORED_BATCHES, total);  // 50 batches = 18,600 bytes
}
```

### Monitor Queue Depth

```c
// Check if queues are backing up
uint32_t ble_depth = k_msgq_num_used_get(&ble_msgq);
if (ble_depth > 12) {
    LOG_WRN("BLE queue depth: %u/16", ble_depth);
}
```

---

## 🐛 Troubleshooting

### Queue Full Warnings

**Symptom:** `"BLE queue full, dropping batch"`

**Causes:**
- BLE transmission slower than sensor sampling
- Flash writes blocking BLE thread
- Connection unstable

**Solutions:**
- Increase `ble_msgq` depth (currently 16)
- Optimize BLE transmission pacing
- Reduce vitals sampling rate

### Invalid Sensor Data

**Symptom:** `temp_c_x100 == INT16_MIN`

**Causes:**
- Sensor not initialized
- I2C communication failure
- `valid` flag not set

**Solutions:**
- Check sensor init logs
- Verify `data_manager_update_*()` called with `valid=true`

### Storage Overflow

**Symptom:** `storage_pending_count() == 50`

**Behavior:**
- Oldest data automatically dropped
- Normal in long disconnections (>20 seconds)

**Solutions:**
- Increase `MAX_STORED_BATCHES` (requires more flash)
- Reduce batch size (`g_target_count = 1`)
- Improve connection stability

---

## 📚 Related Documentation

- **BLE Pipeline**: [`docs/BLE_DATA_PIPELINE.md`](docs/BLE_DATA_PIPELINE.md)
- **Storage Manager**: [`src/storage_manager.c`](src/storage_manager.c)
- **BLE Application**: [`src/ble_app.c`](src/ble_app.c)
- **Sensor Libraries**: [`lib/max32664c_main.c`](lib/max32664c_main.c)

---

**Last Updated**: January 2026  
**Firmware Version**: BM_ESO_V1_0  
**Author**: Neso Monitor Development Team
