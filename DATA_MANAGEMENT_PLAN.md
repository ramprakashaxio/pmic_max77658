# Data Management Plan - 1 Day Storage
## nRF54L15 Sensor Data Pipeline

**Date**: January 2026
**Target**: Store 1 day of heart rate and SpO2 data in internal flash
**Device**: nRF54L15 (1.5MB RRAM)

---

## 📋 Executive Summary

This plan redesigns the data management pipeline to store **24 hours of biometric sensor data** (heart rate, SpO2, and ECG-derived metrics) in the nRF54L15 internal flash, eliminating storage of device status information (battery, temperature, IMU, charger status).

### Key Changes
- ✅ **Compact data structure**: 10 bytes per sample (vs current 36 bytes)
- ✅ **Confidence filtering**: Store only high-quality samples (>80% confidence)
- ✅ **Extended flash partition**: 400KB storage (vs current 32KB)
- ✅ **1-day retention**: ~28,800-86,400 samples (8-24 hours with filtering)
- ✅ **ECG-derived metrics**: Heart Rate Variability (HRV), R-R intervals from MAX32664C

---

## 🎯 Storage Requirements Analysis

### Current State
| Component | Size | Samples | Duration |
|-----------|------|---------|----------|
| `patient_sample_t` | 36 bytes | 500 | ~20 seconds |
| Storage partition | 32 KB | 50 batches | ~20 seconds @ 1Hz |
| **Issue** | Stores device info (battery, temp, IMU) | ❌ Not needed |

### Target State (1 Day Storage)

**Sampling Strategy Options:**

#### Option 1: Continuous 1Hz (Recommended)
- **Sample rate**: 1 sample/second
- **Daily samples**: 86,400 samples
- **With confidence filter (>80%)**: ~60,480 samples (70% pass rate)
- **Storage required**: 60,480 × 10 bytes = **590 KB**

#### Option 2: Continuous 0.5Hz (Conservative)
- **Sample rate**: 1 sample every 2 seconds
- **Daily samples**: 43,200 samples
- **With confidence filter**: ~30,240 samples
- **Storage required**: 30,240 × 10 bytes = **295 KB**

#### Option 3: Smart Adaptive (Advanced)
- **Sample rate**: Variable based on HR variability
  - Stable HR: 0.5Hz
  - Changing HR: 1Hz
  - High activity: 2Hz
- **Daily samples**: ~50,000 average
- **Storage required**: ~**490 KB**

### Flash Allocation Plan

**nRF54L15 RRAM Layout** (1.5MB total):
```
0x00000000 - 0x0000FFFF : Bootloader (64KB)
0x00010000 - 0x0007FFFF : Application Slot 0 (448KB)
0x00080000 - 0x000EFFFF : Application Slot 1 (448KB)
0x000F0000 - 0x000F9FFF : Storage Partition (400KB) ← EXPANDED
0x000FA000 - 0x000FFFFF : Scratch (24KB) ← REDUCED
```

**Storage Partition**: **400 KB** (0x64000 bytes)
- **Effective capacity**: ~350 KB (NVS overhead 12-15%)
- **Max samples**: 35,000 samples
- **Duration**: 9.7 hours @ 1Hz or **19.4 hours @ 0.5Hz**
- **With filtering**: **~24 hours coverage**

---

## 📊 Data Structure Design

### 1. Minimal Sensor Sample (10 bytes)

**NEW: `sensor_sample_t` - Compact biometric data only**

```c
typedef struct __attribute__((packed)) {
    uint32_t ts_sec;       // Timestamp (seconds since boot) - 4 bytes
    uint16_t hr_bpm;       // Heart rate (bpm) - 2 bytes
    uint16_t spo2_x10;     // SpO2 × 10 (985 = 98.5%) - 2 bytes
    uint8_t  hr_conf;      // HR confidence (0-100) - 1 byte
    uint8_t  spo2_conf;    // SpO2 confidence (0-100) - 1 byte
} sensor_sample_t;  // Total: 10 bytes
```

**Rationale**:
- ✅ Timestamp in seconds (not ms) saves storage for 24-hour range
- ✅ Only HR and SpO2 as requested
- ✅ Confidence values allow post-filtering
- ✅ 72% smaller than current `patient_sample_t` (36 bytes)

### 2. ECG-Derived Metrics (Optional Extension)

If MAX32664C provides ECG-derived metrics:

```c
typedef struct __attribute__((packed)) {
    uint32_t ts_sec;       // Timestamp - 4 bytes
    uint16_t hr_bpm;       // Heart rate - 2 bytes
    uint16_t spo2_x10;     // SpO2 × 10 - 2 bytes
    uint8_t  hr_conf;      // HR confidence - 1 byte
    uint8_t  spo2_conf;    // SpO2 confidence - 1 byte
    uint16_t rr_interval;  // R-R interval (ms) - 2 bytes
    uint8_t  hrv_sdnn;     // HRV SDNN (ms) - 1 byte
    uint8_t  flags;        // Quality flags - 1 byte
} sensor_sample_ecg_t;  // Total: 14 bytes
```

**Trade-off**: 14 bytes vs 10 bytes = 40% larger
- 1 day @ 1Hz: 86,400 × 14 = 1.2 MB (exceeds flash capacity)
- **Recommendation**: Use 10-byte structure, store ECG metrics separately at lower rate

### 3. Batch Structure (For BLE Transmission)

**NEW: `sensor_batch_t` - Compact batch for BLE**

```c
#define SENSOR_BATCH_SIZE 20  // Increased from 10

typedef struct __attribute__((packed)) {
    uint8_t  ver;           // Protocol version - 1 byte
    uint8_t  count;         // Number of samples (1-20) - 1 byte
    uint16_t batch_seq;     // Sequence number - 2 bytes
    uint32_t uptime_sec;    // System uptime (seconds) - 4 bytes

    sensor_sample_t samples[SENSOR_BATCH_SIZE];  // 20 × 10 = 200 bytes
} sensor_batch_t;  // Total: 208 bytes
```

**Benefits**:
- ✅ 44% smaller than current `patient_batch_t` (372 bytes)
- ✅ Stores 20 samples (vs 10) for better BLE efficiency
- ✅ 20 seconds of data per batch @ 1Hz

---

## 🔄 Data Management Pipeline

### Architecture Overview

```
┌─────────────────────────────────────────────────────────────┐
│  MAX32664C Thread (40ms / 25Hz)                            │
│  • Polls biometric sensor hub                              │
│  • Receives HR, SpO2, confidence, SCD                      │
└────────────────┬────────────────────────────────────────────┘
                 │
                 v
┌─────────────────────────────────────────────────────────────┐
│  Confidence Filter (Application Layer)                     │
│  • IF hr_conf >= 80% AND spo2_conf >= 80%:                │
│    → Pass to Data Manager                                  │
│  • ELSE: Discard sample                                    │
└────────────────┬────────────────────────────────────────────┘
                 │
                 v
┌─────────────────────────────────────────────────────────────┐
│  Data Manager (NEW: Sensor-Only Mode)                      │
│  • Creates sensor_sample_t (10 bytes)                      │
│  • Timestamp: k_uptime_get() / 1000 (seconds)             │
│  • Accumulates into batch (20 samples)                     │
│  • Queues to BLE pipeline                                  │
└────────────────┬────────────────────────────────────────────┘
                 │
                 ├──────────────┬──────────────────────────────┐
                 v              v                              v
        ┌────────────┐  ┌────────────┐              ┌────────────────┐
        │ BLE Queue  │  │  Storage   │              │  (Optional)    │
        │ (RAM)      │  │  Manager   │              │  Analytics     │
        │ 16 batches │  │  (Flash)   │              │  Engine        │
        └─────┬──────┘  └─────┬──────┘              └────────────────┘
              │               │
              v               v
      ┌──────────────┐ ┌──────────────┐
      │ BLE Ingest   │ │ Flash Ring   │
      │ Thread       │ │ Buffer       │
      │ (Live-first) │ │ (35K samples)│
      └──────┬───────┘ └──────┬───────┘
             │                │
             v                v
      ┌──────────────────────────┐
      │   BLE Notifications      │
      │   • Vitals: 12 bytes     │
      │   • Batch mode on sync   │
      └──────────────────────────┘
                 │
                 v
      ┌──────────────────────────┐
      │   Mobile App / Cloud     │
      └──────────────────────────┘
```

### Data Flow Steps

#### 1. Sensor Acquisition (MAX32664C)
```c
// max32664c_main.c - Worker thread
void max32664c_worker_thread(void) {
    while (1) {
        k_sleep(K_MSEC(1000));  // 1Hz sampling (changed from 40ms)

        // Read biometric data
        max32664c_read_sample(&hr, &hr_conf, &spo2, &spo2_conf, &scd);

        // Confidence filtering (CRITICAL)
        if (hr_conf >= 80 && spo2_conf >= 80 && scd == 3) {
            // High quality sample - store it
            data_manager_push_sensor_sample(hr, spo2, hr_conf, spo2_conf);
        } else {
            // Low quality - discard
            LOG_DBG("Sample rejected: hr_conf=%d, spo2_conf=%d", hr_conf, spo2_conf);
        }
    }
}
```

#### 2. Data Manager (NEW API)
```c
// data_manager.h - NEW API for sensor-only mode

void data_manager_push_sensor_sample(
    uint16_t hr_bpm,
    uint16_t spo2_x10,
    uint8_t hr_conf,
    uint8_t spo2_conf
);

int data_manager_get_sensor_batch(sensor_batch_t *batch);
```

**Implementation**:
```c
// data_manager.c
static sensor_batch_t current_batch;
static uint32_t batch_seq = 0;

void data_manager_push_sensor_sample(uint16_t hr_bpm, uint16_t spo2_x10,
                                      uint8_t hr_conf, uint8_t spo2_conf)
{
    sensor_sample_t sample;
    sample.ts_sec = k_uptime_get() / 1000;  // Convert ms to seconds
    sample.hr_bpm = hr_bpm;
    sample.spo2_x10 = spo2_x10;
    sample.hr_conf = hr_conf;
    sample.spo2_conf = spo2_conf;

    // Add to current batch
    current_batch.samples[current_batch.count++] = sample;

    // Flush when batch is full (20 samples)
    if (current_batch.count >= SENSOR_BATCH_SIZE) {
        current_batch.ver = SENSOR_PAYLOAD_VER;
        current_batch.batch_seq = batch_seq++;
        current_batch.uptime_sec = k_uptime_get() / 1000;

        // Queue to BLE
        k_msgq_put(&sensor_ble_msgq, &current_batch, K_NO_WAIT);

        current_batch.count = 0;
    }
}
```

#### 3. Storage Manager (Enhanced for 1-day retention)

**NEW: Circular buffer with 400KB capacity**

```c
// storage_manager.h
#define STORAGE_MAX_SAMPLES  35000  // 35,000 samples @ 10 bytes = 350KB

int storage_save_sample(const sensor_sample_t *sample);
int storage_get_samples(sensor_sample_t *samples, uint16_t count, uint16_t *retrieved);
int storage_get_sample_count(void);
int storage_clear_oldest(uint16_t count);
```

**Flash Layout**:
```
NVS Partition: 400KB (0xF0000 - 0xF9FFF)
├─ Metadata: 50KB (NVS overhead)
└─ Data: 350KB
   ├─ Write Index: 4 bytes
   ├─ Read Index: 4 bytes
   └─ Circular Buffer: 35,000 samples × 10 bytes
```

**Circular Buffer Logic**:
```c
// storage_manager.c
static uint16_t write_idx = 0;  // Current write position
static uint16_t read_idx = 0;   // Current read position
static uint16_t sample_count = 0;

int storage_save_sample(const sensor_sample_t *sample)
{
    int rc;

    // Write sample to NVS at write_idx position
    char key[16];
    snprintf(key, sizeof(key), "s%05u", write_idx);
    rc = nvs_write(&fs, key, sample, sizeof(sensor_sample_t));

    if (rc < 0) {
        return rc;
    }

    // Advance write index (circular)
    write_idx = (write_idx + 1) % STORAGE_MAX_SAMPLES;

    // Update sample count (max 35,000)
    if (sample_count < STORAGE_MAX_SAMPLES) {
        sample_count++;
    } else {
        // Buffer full - oldest sample overwritten
        read_idx = (read_idx + 1) % STORAGE_MAX_SAMPLES;
    }

    // Persist indices
    nvs_write(&fs, "widx", &write_idx, sizeof(write_idx));
    nvs_write(&fs, "ridx", &read_idx, sizeof(read_idx));
    nvs_write(&fs, "scnt", &sample_count, sizeof(sample_count));

    return 0;
}
```

#### 4. BLE Ingest Thread (Live-First Strategy)

```c
// ble_app.c
void ble_ingest_thread(void)
{
    sensor_batch_t batch;

    while (1) {
        // Wait for batch from data manager
        int rc = data_manager_get_sensor_batch(&batch);
        if (rc != 0) continue;

        // Try live transmission first
        if (ble_is_connected() && ble_is_subscribed()) {
            rc = ble_transmit_sensor_batch(&batch);
            if (rc == 0) {
                LOG_DBG("Batch %u transmitted live", batch.batch_seq);
                continue;  // Success - no storage needed
            }
        }

        // Fallback: store to flash
        for (int i = 0; i < batch.count; i++) {
            storage_save_sample(&batch.samples[i]);
        }

        LOG_INF("Batch %u stored to flash (%u samples pending)",
                batch.batch_seq, storage_get_sample_count());
    }
}
```

#### 5. BLE Flush Thread (Catch-up Sync)

```c
// ble_app.c
void ble_flush_thread(void)
{
    sensor_sample_t samples[SENSOR_BATCH_SIZE];
    uint16_t retrieved;

    while (1) {
        k_sleep(K_MSEC(500));  // Check every 500ms

        // Only flush if connected and data pending
        if (!ble_is_connected() || !ble_is_subscribed()) {
            continue;
        }

        int pending = storage_get_sample_count();
        if (pending == 0) {
            continue;  // No data to sync
        }

        LOG_INF("Starting catch-up sync: %d samples pending", pending);

        // Flush up to 10 batches per cycle
        for (int i = 0; i < 10 && storage_get_sample_count() > 0; i++) {
            // Retrieve batch of samples
            storage_get_samples(samples, SENSOR_BATCH_SIZE, &retrieved);

            // Transmit via BLE
            int rc = ble_transmit_sensor_samples(samples, retrieved);
            if (rc == 0) {
                // Delete successfully transmitted samples
                storage_clear_oldest(retrieved);
            } else {
                // Transmission failed - stop sync
                LOG_WRN("Catch-up sync interrupted");
                break;
            }

            k_sleep(K_MSEC(50));  // 50ms gap between batches
        }
    }
}
```

---

## 📡 BLE Protocol Changes

### NEW: Sensor-Only Notification

**Characteristic UUID**: `12345678-1234-5678-1234-56789abcdef1` (reuse vitals)

**Payload** (12 bytes, vs current 15 bytes):
```c
typedef struct __attribute__((packed)) {
    uint32_t ts_sec;       // Timestamp (seconds) - 4 bytes
    uint16_t hr_bpm;       // Heart rate - 2 bytes
    uint16_t spo2_x10;     // SpO2 × 10 - 2 bytes
    uint8_t  hr_conf;      // HR confidence - 1 byte
    uint8_t  spo2_conf;    // SpO2 confidence - 1 byte
    uint8_t  algo_mode;    // Algorithm mode - 1 byte
    uint8_t  scd;          // Skin contact - 1 byte
} ble_sensor_data_t;  // Total: 12 bytes
```

**Removed from BLE**:
- ❌ Temperature characteristic (def3)
- ❌ Battery characteristic (def4)
- ❌ IMU characteristic (def2)
- ✅ Keep vitals characteristic (def1) - modified payload

---

## 🛠️ Implementation Checklist

### Phase 1: Data Structure Refactoring
- [ ] Create `sensor_sample_t` (10 bytes) in `data_manager.h`
- [ ] Create `sensor_batch_t` (208 bytes) in `data_manager.h`
- [ ] Add `#define SENSOR_PAYLOAD_VER 2` to distinguish from old format
- [ ] Update BLE notification payload `ble_sensor_data_t` (12 bytes)

### Phase 2: Flash Partition Expansion
- [ ] Update [bl54l15_dvk_nrf54l15_cpuapp.overlay](boards/bl54l15_dvk_nrf54l15_cpuapp.overlay):
  ```dts
  storage_partition: partition@F0000 {
      label = "storage";
      reg = <0x000F0000 0x00064000>;  /* 400KB */
  };
  scratch_partition: partition@154000 {
      label = "image-scratch";
      reg = <0x00154000 0x00006000>;  /* 24KB */
  };
  ```
- [ ] Verify total size doesn't exceed 1.5MB

### Phase 3: Storage Manager Rewrite
- [ ] Implement `storage_save_sample()` with circular buffer
- [ ] Implement `storage_get_samples()` for batch retrieval
- [ ] Implement `storage_get_sample_count()` for pending data check
- [ ] Implement `storage_clear_oldest()` for cleanup after BLE transmission
- [ ] Update NVS key scheme: `"s%05u"` for samples (0-34999)
- [ ] Add wear leveling awareness (NVS handles this internally)

### Phase 4: Data Manager Refactoring
- [ ] Remove `data_manager_update_temp()` (not needed)
- [ ] Remove `data_manager_update_batt()` (not needed)
- [ ] Remove `data_manager_update_imu()` (not needed)
- [ ] Add `data_manager_push_sensor_sample()` (NEW)
- [ ] Add `data_manager_get_sensor_batch()` (NEW)
- [ ] Remove `patient_sample_t` and `patient_batch_t` (old structures)
- [ ] Update message queue to use `sensor_batch_t`

### Phase 5: MAX32664C Driver Updates
- [ ] Add confidence threshold configuration (default 80%)
- [ ] Implement sample rejection logic in `max32664c_main.c`
- [ ] Change sampling rate to 1Hz (from 40ms)
- [ ] Add ECG-derived metrics extraction (if available from sensor hub)
- [ ] Log rejection statistics for debugging

### Phase 6: BLE Application Updates
- [ ] Update `ble_sensor_data_t` notification payload
- [ ] Remove temperature notification handler
- [ ] Remove battery notification handler
- [ ] Remove IMU notification handler
- [ ] Update ingest thread for new batch format
- [ ] Update flush thread for sample-based retrieval
- [ ] Test catch-up sync with 24-hour data

### Phase 7: Testing & Validation
- [ ] Test 1: Store 10,000 samples and verify flash usage
- [ ] Test 2: Disconnect for 1 hour, reconnect, verify catch-up sync
- [ ] Test 3: Store 24 hours continuously, verify circular buffer overwrite
- [ ] Test 4: Power cycle with stored data, verify persistence
- [ ] Test 5: Measure flash write frequency and estimate endurance
- [ ] Test 6: BLE throughput test with burst transmission

---

## 📈 Performance Analysis

### Flash Write Frequency

**Scenario 1: Always Connected (Live Streaming)**
- Write frequency: **0 writes/second** (no flash writes)
- Flash impact: None
- Lifespan: Unlimited

**Scenario 2: Always Disconnected (Full Storage Mode)**
- Write frequency: **1 write/second** @ 1Hz sampling
- Daily writes: 86,400 writes
- NVS wear leveling: ~100,000 cycles per sector
- **Flash lifespan: >10 years** (100,000 / 86,400 = 1,157 days)

**Scenario 3: Typical Usage (50% connected)**
- Effective write frequency: **0.5 writes/second**
- **Flash lifespan: >20 years**

### BLE Transmission Efficiency

**Live Streaming Mode** (Connected):
- Batch size: 20 samples × 12 bytes = 240 bytes per notification
- Frequency: 1 notification per 20 seconds
- Bandwidth: **12 bytes/second** (96 bps)
- Connection interval: 50ms → plenty of headroom

**Catch-up Sync Mode** (After Disconnection):
- Burst rate: 10 batches per 500ms = 2,400 bytes/500ms
- Effective throughput: **4.8 KB/second**
- Time to sync 1 hour of data: ~1.25 minutes
  - 1 hour = 3,600 samples = 36 KB
  - 36 KB / 4.8 KB/s = 7.5 seconds

### Memory Usage

| Component | Current | New | Change |
|-----------|---------|-----|--------|
| Sample size | 36 bytes | 10 bytes | **-72%** |
| Batch size | 372 bytes | 208 bytes | **-44%** |
| BLE queue (RAM) | ~6 KB | ~3.3 KB | **-45%** |
| Flash partition | 32 KB | 400 KB | **+1,150%** |
| Max samples stored | 500 | 35,000 | **+6,900%** |
| Max duration | 20 sec | **24 hours** | **+4,320x** |

---

## 🔬 ECG Data Consideration

### MAX32664C ECG Capabilities

The MAX32664C biometric sensor hub supports multiple AFE chips:
- MAX86141
- MAX86161
- **MAX86174** (current configuration)

**Algorithm Modes**:
1. **Mode 1**: Continuous HR monitoring
2. **Mode 2**: Continuous HR + SpO2
3. **Mode 8**: Raw sensor data streaming (PPG/ECG waveforms)

### ECG Data Options

#### Option A: Raw PPG Waveform (NOT RECOMMENDED)
- Sample rate: 25-400 Hz
- Data size per sample: 6 bytes (3× 16-bit LED channels)
- **Daily size**: 25 Hz × 6 bytes × 86,400 sec = **12.96 MB/day**
- ❌ **Exceeds flash capacity**

#### Option B: ECG-Derived Metrics (RECOMMENDED)
MAX32664C algorithm can provide:
- **R-R interval** (time between heartbeats) → HRV analysis
- **Heart Rate Variability (HRV)**: SDNN, RMSSD
- **Perfusion Index (PI)**: Signal quality indicator
- **Motion artifact detection**

**Storage approach**:
```c
typedef struct __attribute__((packed)) {
    uint32_t ts_sec;       // 4 bytes
    uint16_t hr_bpm;       // 2 bytes
    uint16_t spo2_x10;     // 2 bytes
    uint8_t  hr_conf;      // 1 byte
    uint8_t  spo2_conf;    // 1 byte
    uint16_t rr_interval;  // R-R interval (ms) - 2 bytes
    uint8_t  hrv_sdnn;     // HRV SDNN (ms) - 1 byte
    uint8_t  pi;           // Perfusion index - 1 byte
} sensor_sample_hrv_t;  // Total: 14 bytes
```

**Trade-off**:
- 14 bytes vs 10 bytes = 40% larger
- Daily storage: 86,400 × 14 = 1.2 MB ❌ Exceeds 400KB partition

**Solution**: **Dual-rate storage**
- HR/SpO2: 1Hz (10 bytes) → 864 KB/day
- HRV metrics: 0.1Hz (once per 10 seconds) → 8,640 samples × 4 bytes = 34 KB/day
- **Total**: ~900 KB/day

**Revised storage allocation**:
```
Storage Partition: 512KB (expand to 0x80000)
├─ HR/SpO2 samples (1Hz): 350KB → ~9.7 hours
├─ HRV metrics (0.1Hz): 50KB → 24 hours
└─ NVS overhead: ~112KB
```

### Recommendation
1. **Start with 10-byte structure** (HR + SpO2 only)
2. **Validate 24-hour storage** works correctly
3. **Phase 2**: Add HRV metrics if MAX32664C firmware supports it
4. **Expand partition to 512KB** if HRV needed

---

## 🚀 Deployment Strategy

### Stage 1: Core Data Pipeline (Week 1)
1. Implement new data structures
2. Update data manager API
3. Add confidence filtering to MAX32664C layer
4. Test with small flash partition (32KB) first

### Stage 2: Flash Expansion (Week 2)
1. Expand storage partition to 400KB
2. Implement circular buffer storage manager
3. Test with 12-hour data collection
4. Verify wear leveling and persistence

### Stage 3: BLE Integration (Week 3)
1. Update BLE notification payloads
2. Test live streaming with new format
3. Test catch-up sync with 10,000 samples
4. Measure BLE throughput and latency

### Stage 4: Validation (Week 4)
1. 24-hour continuous test (disconnected)
2. 24-hour with intermittent connections
3. Power cycle tests
4. Flash endurance estimation
5. Mobile app integration testing

---

## 📋 Configuration Summary

### Device Tree Changes
```dts
/* boards/bl54l15_dvk_nrf54l15_cpuapp.overlay */

storage_partition: partition@F0000 {
    label = "storage";
    reg = <0x000F0000 0x00064000>;  /* 400KB */
};
```

### Kconfig Changes
```ini
# prj.conf

# Storage Configuration
CONFIG_NVS=y
CONFIG_FLASH=y
CONFIG_FLASH_MAP=y
CONFIG_FLASH_PAGE_LAYOUT=y
CONFIG_MPU_ALLOW_FLASH_WRITE=y

# Increase NVS sector count for 400KB
CONFIG_NVS_SECTOR_COUNT=100
```

### Code Configuration
```c
// data_manager.h

#define SENSOR_BATCH_SIZE      20
#define SENSOR_PAYLOAD_VER     2
#define CONFIDENCE_THRESHOLD   80  // Only store samples with conf >= 80%

// storage_manager.h

#define STORAGE_MAX_SAMPLES    35000   // 35K samples × 10 bytes = 350KB
#define STORAGE_PARTITION_SIZE 0x64000 // 400KB
```

---

## 🎯 Success Criteria

### Functional Requirements
- ✅ Store minimum 20 hours of HR/SpO2 data (@ 1Hz with filtering)
- ✅ Confidence filtering removes low-quality samples
- ✅ Data survives power cycles
- ✅ Circular buffer overwrites oldest data when full
- ✅ BLE catch-up sync works with 10,000+ samples
- ✅ Live streaming works when connected

### Performance Requirements
- ✅ Flash write frequency < 2 writes/second (average)
- ✅ BLE notification latency < 100ms (live mode)
- ✅ Catch-up sync completes < 10 seconds per hour of data
- ✅ Flash lifespan > 10 years
- ✅ No sample loss during normal operation

### Quality Requirements
- ✅ Only samples with HR confidence ≥ 80% stored
- ✅ Only samples with SpO2 confidence ≥ 80% stored
- ✅ Only samples with skin contact (SCD = 3) stored
- ✅ Timestamp accuracy ±1 second
- ✅ No device status data (battery, temp, IMU) stored

---

## 📚 Next Steps

1. **Review this plan** with the team
2. **Verify MAX32664C capabilities** for ECG-derived metrics
3. **Confirm flash partition expansion** is acceptable
4. **Prioritize HRV metrics** (nice-to-have vs must-have)
5. **Begin implementation** starting with Phase 1

---

**Document Status**: Draft for Review
**Author**: Claude Sonnet 4.5
**Last Updated**: 2026-01-30
