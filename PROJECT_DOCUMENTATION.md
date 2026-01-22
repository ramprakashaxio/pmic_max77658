# Neso Monitor - Comprehensive Project Documentation

## Table of Contents
1. [Project Overview](#project-overview)
2. [Hardware Architecture](#hardware-architecture)
3. [Software Architecture](#software-architecture)
4. [Build Environment](#build-environment)
5. [Configuration](#configuration)
6. [API Reference](#api-reference)
7. [BLE Protocol](#ble-protocol)
8. [Power Management](#power-management)
9. [Build & Flash Instructions](#build--flash-instructions)
10. [Testing & Validation](#testing--validation)
11. [Troubleshooting](#troubleshooting)

---

## Project Overview

**Neso Monitor** is a wearable health monitoring system built on the Nordic nRF54L15 (ARM Cortex-M33) using nRF Connect SDK v3.1.0. The system provides real-time vital signs monitoring with Bluetooth Low Energy (BLE) connectivity and intelligent power management.

### Key Features
- ✅ **Multi-Sensor Integration**: Heart rate, SpO2, temperature, IMU, battery monitoring
- ✅ **BLE Store-and-Forward**: Reliable data delivery with offline buffering
- ✅ **Advanced Power Management**: Ship mode, software off, charger-only boot
- ✅ **Real-Time Processing**: 25Hz vitals sampling with intelligent batching
- ✅ **Non-Volatile Storage**: NVS-based data persistence
- ✅ **Thread-Safe Design**: Multi-threaded architecture with I2C arbitration

### Target Boards
- **Primary**: `bl54l15_dvk_nrf54l15_cpuapp` (BL54L15 DVK)
- **Testing**: `nrf52840dk/nrf52840` (nRF52840 DK)
- **Supported**: nRF5340, nRF54L15 family

---

## Hardware Architecture

### Block Diagram
```
┌─────────────────────────────────────────────────────────────┐
│                       nRF54L15 MCU                          │
│  ┌──────────────────────────────────────────────────────┐   │
│  │  ARM Cortex-M33 @ 128MHz                            │   │
│  │  - 1.5MB Flash                                       │   │
│  │  - 256KB RAM                                         │   │
│  │  - Bluetooth 5.4 LE                                  │   │
│  └──────────────────────────────────────────────────────┘   │
└─────────────────────────────────────────────────────────────┘
           │                                          │
           │ I2C (100kHz)                            │ UART
           │                                          │
    ┌──────┴─────────┬─────────────┬─────────────────┘
    │                │             │
┌───▼────┐    ┌──────▼─────┐  ┌───▼─────┐
│MAX32664C│    │  MAX77658  │  │LSM6DSV32X│
│Biosensor│    │    PMIC    │  │   IMU    │
│Hub      │    │            │  │          │
└─────────┘    └────────────┘  └──────────┘
    │                │
┌───▼────┐    ┌──────▼─────┐
│MAX86174│    │ Fuel Gauge │
│PPG AFE │    │ Charger    │
└────────┘    └────────────┘
```

### I2C Bus Configuration
| Device | Address | Function | Driver |
|--------|---------|----------|--------|
| MAX32664C | 0x55 | Biometric Sensor Hub (HR, SpO2, HRV, RR) | Custom |
| MAX77658 (PM) | 0x48 | Power Management | Custom |
| MAX77658 (FG) | 0x36 | Fuel Gauge | Custom |
| TMP117 | 0x48/0x49 | High-Precision Temperature Sensor | Custom |
| MAX30208 | 0x50 | Digital Temperature Sensor (Legacy) | Custom |
| MAX30001 | 0x54 | ECG/BioZ AFE (RR, Stress) | Custom |
| LSM6DSV32X | 0x6B | 6-Axis IMU (Motion, Fall, Steps) | Zephyr Driver |

### GPIO Pin Assignments (BL54L15 DVK)

#### I2C (TWIM21)
- **SDA**: P1.11
- **SCL**: P1.12

#### UART Console (UART20)
- **TX**: P1.02 (NFC1 released as GPIO)
- **RX**: P1.03 (NFC2 released as GPIO)

#### MAX32664C Control
- **HUB_MFIO**: P1.13 (Interrupt, Open-drain, Pull-up)
- **HUB_RST**: P0.04 (Active-low reset)
- **OE**: P1.14 (Output enable for level shifter)

#### PMIC Control
- **nEN**: P1.07 (Power enable, Hi-Z input)
- **nIRQ**: P1.06 (Interrupt, Pull-up, Active-low)
- **nRST**: P1.05 (Reset monitor, Input only)

---

## Sensor Parameters & Specifications

### 1. LSM6DSV32X - 6-Axis IMU (Motion Analysis)

**Hardware**: ST Microelectronics LSM6DSV32X  
**Interface**: I2C @ 0x6B  
**Driver**: Zephyr native sensor API

#### Output Parameters

| Parameter | Type | Range | Resolution | Update Rate |
|-----------|------|-------|------------|-------------|
| **Accelerometer X/Y/Z** | int16 | ±4g | 0.122 mg/LSB | 26 Hz |
| **Gyroscope X/Y/Z** | int16 | ±500 dps | 17.5 mdps/LSB | On-demand |
| **Step Count** | uint32 | 0 - 2³² | 1 step | Real-time |
| **Orientation** | enum | 7 states | - | 2s hysteresis |
| **Activity Level** | enum | 4 levels | - | 2s window |
| **Fall Detection** | bool | - | - | <100ms latency |
| **Freefall Duration** | uint16 | 0-65535 ms | 1 ms | Event-driven |
| **Impact Force** | float | 0-32g | 0.1g | Peak detect |
| **Tilt Angle** | int16 | -180° to +180° | 0.1° | 100ms |
| **Sedentary Time** | uint32 | 0 - 2³² sec | 1s | Continuous |
| **Gesture Recognition** | uint8 | 16 gestures | - | Event-driven |

#### Derived Metrics

| Metric | Formula | Purpose |
|--------|---------|---------|
| **RMS Acceleration** | √(x² + y² + z²) / window | Activity classification |
| **Gravity Vector** | Low-pass filter (0.1 Hz) | Orientation detection |
| **Jerk** | dAccel / dt | Fall detection |
| **Angular Velocity** | Gyro magnitude | Rotation detection |
| **Step Cadence** | Steps / minute | Gait analysis |
| **Energy Expenditure** | Activity × weight × time | Calorie estimation |

#### Configuration Parameters

```c
/* Accelerometer */
#define IMU_ACCEL_ODR          26      // Hz (low power)
#define IMU_ACCEL_RANGE        4       // ±4g
#define IMU_ACCEL_FILTER       LPF2    // Low-pass filter
#define IMU_ACCEL_BW           ODR/4   // Bandwidth

/* Motion Thresholds */
#define THRESHOLD_IDLE         0.05g   // Noise floor
#define THRESHOLD_LIGHT        0.3g    // Normal movement
#define THRESHOLD_MODERATE     1.0g    // Active movement
#define THRESHOLD_VIGOROUS     2.0g    // Exercise
#define THRESHOLD_FREEFALL     0.1g    // All axes
#define THRESHOLD_IMPACT       8.0g    // High-g event

/* Step Detection */
#define STEP_THRESHOLD         1.2g    // Vertical acceleration
#define STEP_MIN_DURATION      100ms   // Minimum step time
#define STEP_MAX_DURATION      2000ms  // Maximum step time
#define STEP_DEBOUNCE          200ms   // Between steps

/* Fall Detection */
#define FALL_FREEFALL_TIME     300ms   // Min freefall duration
#define FALL_IMPACT_THRESHOLD  6.0g    // Impact force
#define FALL_POST_IMPACT_TIME  1000ms  // Recovery window
```

#### Event Flags

```c
#define IMU_EVT_FREEFALL       (1U << 0)  // <0.1g on all axes >300ms
#define IMU_EVT_IMPACT         (1U << 1)  // >8g detected
#define IMU_EVT_ROLLOVER       (1U << 2)  // Orientation flip >90°
#define IMU_EVT_PROLONGED_SIT  (1U << 3)  // >2 hours sedentary
#define IMU_EVT_STEP_DETECTED  (1U << 4)  // Step registered
#define IMU_EVT_GESTURE        (1U << 5)  // Recognized gesture
#define IMU_EVT_TAP_SINGLE     (1U << 6)  // Single tap detected
#define IMU_EVT_TAP_DOUBLE     (1U << 7)  // Double tap detected
#define IMU_EVT_SHAKE          (1U << 8)  // Shake gesture
#define IMU_EVT_TILT           (1U << 9)  // Significant tilt
```

---

### 2. MAX32664C + MAX86174 - PPG Biosensor Hub

**Hardware**: Maxim MAX32664C (sensor hub) + MAX86174 (optical AFE)  
**Interface**: I2C @ 0x55  
**Driver**: Custom application-specific

#### Output Parameters

| Parameter | Type | Range | Resolution | Update Rate | Confidence |
|-----------|------|-------|------------|-------------|------------|
| **Heart Rate (HR)** | uint16 | 30-220 BPM | 1 BPM | 25 Hz | 0-100% |
| **SpO2** | uint16 | 70-100% | 0.1% | 25 Hz | 0-100% |
| **Respiration Rate (RR)** | uint16 | 4-60 BrPM | 0.1 BrPM | 25 Hz | 0-100% |
| **Heart Rate Variability (HRV)** | uint16 | 10-200 ms | 1 ms | 1 Hz | Calculated |
| **R-R Interval** | uint16 | 300-2000 ms | 1 ms | Per beat | High |
| **Perfusion Index (PI)** | uint16 | 0.1-20% | 0.01% | 25 Hz | - |
| **Skin Contact Detection** | uint8 | 0-3 | - | 25 Hz | Hardware |
| **Signal Quality Index (SQI)** | uint8 | 0-100 | 1 | 25 Hz | Calculated |
| **Algorithm State** | uint8 | 0-7 | - | 25 Hz | - |
| **LED Current (Auto)** | uint8 | 10-150 mA | 1 mA | AGC | Per LED |

#### Advanced Metrics (Calculated)

| Metric | Calculation | Clinical Significance | Update Rate |
|--------|-------------|----------------------|-------------|
| **RMSSD** | √(Σ(RRᵢ₊₁ - RRᵢ)² / N) | HRV time-domain | 1 min |
| **SDNN** | σ(RR intervals) | Overall HRV | 5 min |
| **pNN50** | Count(|RRᵢ₊₁ - RRᵢ| > 50ms) / N | Parasympathetic activity | 1 min |
| **LF Power** | FFT (0.04-0.15 Hz) | Sympathetic + PNS | 5 min |
| **HF Power** | FFT (0.15-0.4 Hz) | Parasympathetic activity | 5 min |
| **LF/HF Ratio** | LF / HF | Autonomic balance | 5 min |
| **Stress Index** | AMo / (2×MxDMn×Mo) | Baevsky's method | 5 min |

#### SpO2 Calibration Coefficients

```c
/* Maxim Standard Coefficients (×100,000) */
#define SPO2_COEFF_A    159584      // 1.5958422
#define SPO2_COEFF_B    -3465966    // -34.6596622
#define SPO2_COEFF_C    11268987    // 112.6898759

/* Formula: SpO2 = A×R² + B×R + C */
/* Where R = (AC_Red/DC_Red) / (AC_IR/DC_IR) */
```

#### LED Configuration

```c
/* LED Current Settings (mA) */
#define LED_MIN_CURRENT     10      // Minimum (low perfusion)
#define LED_DEFAULT_CURRENT 50      // Normal operation
#define LED_MAX_CURRENT     100     // Maximum (dark skin)

/* AGC Parameters */
#define AGC_TARGET_ADC      32768   // 50% of 16-bit range
#define AGC_TOLERANCE       ±10%    // Acceptable deviation
#define AGC_STEP_SIZE       5mA     // Current adjustment
#define AGC_UPDATE_RATE     1Hz     // Adjustment frequency
```

#### Algorithm Modes

```c
typedef enum {
    MODE_INIT           = 0,    // Initializing
    MODE_ACQUIRING      = 1,    // Searching for signal
    MODE_CALCULATING    = 2,    // Computing vitals
    MODE_VALID          = 3,    // Valid data output
    MODE_LOW_SIGNAL     = 4,    // Poor signal quality
    MODE_OFF_SKIN       = 5,    // No contact detected
    MODE_ERROR          = 6,    // Hardware fault
    MODE_MOTION         = 7     // Excessive motion
} ppg_algo_mode_t;
```

---

### 3. MAX30001 - ECG/BioZ AFE

**Hardware**: Maxim MAX30001G (analog front-end)  
**Interface**: SPI (future) or I2C bridge @ 0x54  
**Driver**: Custom driver (in development)

#### Output Parameters

| Parameter | Type | Range | Resolution | Update Rate | Purpose |
|-----------|------|-------|------------|-------------|---------|
| **ECG Waveform** | int24 | ±2 mV | 0.488 µV | 256 Hz | Raw signal |
| **R-R Interval (ECG)** | uint16 | 300-2000 ms | 1 ms | Per beat | HRV analysis |
| **Heart Rate (ECG)** | uint16 | 30-220 BPM | 1 BPM | 1 Hz | Primary metric |
| **Respiration Rate** | uint16 | 4-60 BrPM | 0.1 BrPM | 0.25 Hz | BioZ-derived |
| **BioZ Impedance** | uint16 | 10-200 Ω | 0.1 Ω | 32 Hz | Respiration |
| **QRS Duration** | uint16 | 60-120 ms | 4 ms | Per beat | Cardiac health |
| **P-Wave Amplitude** | int16 | 0-300 µV | 1 µV | Per beat | Atrial activity |
| **T-Wave Amplitude** | int16 | 100-600 µV | 1 µV | Per beat | Repolarization |
| **ST Segment Deviation** | int16 | ±500 µV | 1 µV | Per beat | Ischemia detection |
| **Lead-Off Detection** | bool | - | - | 16 Hz | Contact quality |

#### Clinical Metrics (Derived)

| Metric | Calculation | Clinical Use | Update Rate |
|--------|-------------|--------------|-------------|
| **HRV (SDNN)** | σ(RR intervals) | Autonomic function | 5 min |
| **Stress Level** | LF/HF + HR trend | Mental stress | 1 min |
| **Arrhythmia Score** | QRS variability + ectopy | Cardiac events | Real-time |
| **Respiratory Sinus Arrhythmia** | HR modulation by breathing | Vagal tone | 1 min |
| **Cardiac Output (est.)** | SV × HR (BioZ-derived) | Hemodynamics | 10s |
| **Systolic Time Intervals** | LVET, PEP from BioZ | Contractility | Per beat |

#### ECG Configuration

```c
/* Sampling & Filtering */
#define ECG_SAMPLE_RATE     256     // Hz (Nyquist for 128 Hz BW)
#define ECG_GAIN            20      // V/V (amplification)
#define ECG_HIGHPASS_CUTOFF 0.5     // Hz (AC coupling)
#define ECG_LOWPASS_CUTOFF  40      // Hz (noise reduction)
#define ECG_NOTCH_FILTER    50/60   // Hz (mains rejection)

/* R-Peak Detection */
#define R_THRESHOLD_FACTOR  0.6     // × mean peak amplitude
#define R_REFRACTORY_PERIOD 200     // ms (avoid double-detect)
#define R_MIN_INTERVAL      300     // ms (max 200 BPM)
#define R_MAX_INTERVAL      2000    // ms (min 30 BPM)

/* Arrhythmia Detection */
#define ECTOPY_RR_DEVIATION 20%     // Premature beat threshold
#define AFIB_RR_IRREGULARITY 25%    // Irregular rhythm
#define BRADYCARDIA_HR      50      // BPM
#define TACHYCARDIA_HR      110     // BPM
```

#### Stress Index Calculation

```c
/* Baevsky's Stress Index */
// SI = AMo / (2 × MxDMn × Mo)
// AMo  = Amplitude of mode (most frequent RR)
// MxDMn = Max - Min RR interval
// Mo   = Mode RR interval
// SI > 150 = High stress
// SI < 50  = Low stress, good recovery
```

---

### 4. MAX77658 - Power Management IC

**Hardware**: Maxim MAX77658 (PMIC + Fuel Gauge + Charger)  
**Interface**: I2C @ 0x48 (PM), 0x36 (FG)  
**Driver**: Custom (4700+ lines, PROTECTED)

#### Output Parameters

| Parameter | Type | Range | Resolution | Update Rate | Source |
|-----------|------|-------|------------|-------------|--------|
| **Battery Voltage** | uint16 | 2.5-4.5V | 1.25 mV | 2s | Fuel Gauge |
| **Battery Current** | int16 | ±500 mA | 0.1 mA | 2s | Fuel Gauge |
| **State of Charge (SoC)** | uint8 | 0-100% | 0.5% | 2s | Coulomb counting |
| **Time to Empty (TTE)** | uint16 | 0-1440 min | 1 min | 2s | Predicted |
| **Time to Full (TTF)** | uint16 | 0-240 min | 1 min | 2s | Charging only |
| **Charge Cycles** | uint16 | 0-65535 | 1 cycle | Per cycle | Persistent |
| **Die Temperature** | int16 | -40 to +85°C | 0.1°C | 2s | Internal ADC |
| **CHGIN Voltage** | uint16 | 0-6V | 25 mV | 2s | Charger detect |
| **Charger Current** | uint16 | 0-500 mA | 10 mA | 2s | Charging status |
| **Charger State** | uint8 | 8 states | - | 2s | Hardware FSM |
| **LDO/SBB Voltage** | uint16 | 0.8-5.5V | 12.5 mV | On-demand | Regulators |

#### Fuel Gauge Calibration

```c
/* Battery Specification */
#define DESIGN_CAPACITY_MAH  350    // Nominal capacity
#define RSENSE_MOHM          10     // Current sense resistor
#define EMPTY_VOLTAGE_MV     3000   // Cutoff voltage
#define FULL_VOLTAGE_MV      4200   // Charge termination
#define RECHARGE_VOLTAGE_MV  3900   // Auto-restart charging

/* Coulomb Counting */
#define COULOMB_RESOLUTION   5.0    // µVh (per LSB)
#define SOC_UPDATE_RATE      2000   // ms
#define SOC_ACCURACY_TYPICAL ±5     // % (after calibration)
```

#### Power Rails Configuration

```c
/* Voltage Rails */
#define SBB0_VOLTAGE_MV     1800    // Core logic (nRF54L15)
#define SBB1_VOLTAGE_MV     3300    // Peripherals (sensors)
#define SBB2_VOLTAGE_MV     5000    // Boosted rail (MAX32664C)

/* Current Limits */
#define SBB0_ILIM_MA        500     // Peak current
#define SBB1_ILIM_MA        300     // Sensor total
#define SBB2_ILIM_MA        100     // PPG AFE
```

#### Charger States

```c
typedef enum {
    CHG_OFF             = 0,    // No charger connected
    CHG_PREQUALIFICATION = 1,   // Trickle charge (<3.0V)
    CHG_FAST_CC         = 2,    // Constant current (0.5C)
    CHG_FAST_CV         = 3,    // Constant voltage (4.2V)
    CHG_TOP_OFF         = 4,    // Trickle (<10mA)
    CHG_DONE            = 5,    // Charge complete
    CHG_TIMER_FAULT     = 6,    // Timeout (3 hours)
    CHG_TEMP_FAULT      = 7     // Over-temperature
} charger_state_t;
```

#### Battery Health Metrics

| Metric | Calculation | Threshold | Action |
|--------|-------------|-----------|--------|
| **State of Health (SoH)** | (Current capacity / Design capacity) × 100% | <80% | Degraded battery |
| **Impedance Rise** | ΔV / ΔI at load | >50Ω | Internal resistance |
| **Self-Discharge Rate** | ΔSoC / Δtime (standby) | >5%/day | Replace battery |
| **Cycle Count** | Full charge cycles | >500 | Expected EOL |

---

### 5. TMP117 - High-Precision Temperature Sensor

**Hardware**: Texas Instruments TMP117  
**Interface**: I2C @ 0x48 (alt: 0x49)  
**Driver**: Custom (replaces MAX30208)

#### Output Parameters

| Parameter | Type | Range | Resolution | Accuracy | Update Rate |
|-----------|------|-------|------------|----------|-------------|
| **Temperature** | int16 | -55 to +150°C | 0.0078°C | ±0.1°C | 1 Hz |
| **Temperature Offset** | int16 | ±256°C | 0.0078°C | - | Calibration |
| **Alert Status** | bool | - | - | - | Interrupt-driven |

#### Configuration

```c
/* Conversion Settings */
#define TMP117_CONVERSION_CYCLE  1000   // ms (1 Hz)
#define TMP117_AVG_SAMPLES       8      // Averaging
#define TMP117_RESOLUTION        0.0078 // °C per LSB

/* Alert Thresholds */
#define TEMP_ALERT_LOW_C         15.0   // Hypothermia warning
#define TEMP_ALERT_HIGH_C        39.0   // Fever warning
#define TEMP_ALERT_HYSTERESIS    0.5    // °C

/* Calibration */
#define TEMP_OFFSET_SKIN         -2.0   // Skin vs core offset
#define TEMP_AMBIENT_COMP        0.1    // Ambient correction
```

#### Clinical Temperature Ranges

| Condition | Range (°C) | Alert Level |
|-----------|------------|-------------|
| **Hypothermia** | <35.0 | Critical |
| **Normal (Core)** | 36.5 - 37.5 | Normal |
| **Normal (Skin)** | 32.0 - 34.0 | Normal |
| **Low-Grade Fever** | 37.5 - 38.0 | Warning |
| **Fever** | 38.0 - 39.0 | Alert |
| **High Fever** | 39.0 - 40.0 | Critical |
| **Hyperthermia** | >40.0 | Emergency |

---

### 6. Sensor Data Integration Matrix

| Data Source | Primary Use | Backup Source | Fusion Strategy |
|-------------|-------------|---------------|-----------------|
| **HR (PPG)** | Continuous monitoring | HR (ECG) | PPG preferred, ECG validates |
| **HR (ECG)** | Clinical accuracy | HR (PPG) | ECG preferred when available |
| **RR (PPG)** | Non-invasive | RR (ECG BioZ) | PPG for convenience |
| **RR (ECG)** | Gold standard | RR (PPG) | ECG preferred for accuracy |
| **HRV** | PPG R-R intervals | ECG R-R | Both sources averaged |
| **Temp (TMP117)** | High precision | PMIC die temp | TMP117 with ambient compensation |
| **Temp (PMIC)** | Fallback only | TMP117 | Used if TMP117 fails |
| **Activity** | IMU accelerometer | Step count | IMU is primary motion source |
| **Steps** | IMU pedometer | - | Sole source |
| **Fall** | IMU (accel + gyro) | - | Multi-axis algorithm |
| **Stress** | ECG HRV + HR trend | PPG HRV | ECG more reliable |
| **SoC** | Fuel gauge | - | Sole authoritative source |

---

## Software Architecture

### Thread Architecture
```
Priority │ Thread Name       │ Stack │ Period  │ Purpose
─────────┼───────────────────┼───────┼─────────┼────────────────────────
   5     │ max32664c_sensor  │ 4096  │  40ms   │ Vitals sampling (25Hz)
   6     │ lsm6dsv32x_imu    │ 2048  │  40ms   │ IMU data processing
   7     │ max77658_pmic     │ 4096  │ 2000ms  │ Battery & power monitor
   7     │ ble_thread        │ 4096  │ 500ms   │ BLE connection manager
   6     │ ble_ingest        │ 4096  │ blocking│ Data ingestion & TX
   7     │ main              │ 4096  │ 5000ms  │ I2C scanner & temp poll
```

### Data Flow Pipeline
```
┌──────────────┐
│ MAX32664C    │ 25Hz Vitals Samples
│ (Sensor Hub) │
└──────┬───────┘
       │
       ▼
┌──────────────────┐
│ data_manager     │ Batch Assembly (10 samples)
│ (Aggregator)     │ + Latest temp/battery/IMU
└──────┬───────────┘
       │
       ▼
┌──────────────┐
│ ble_ingest   │ Live TX (Priority)
│ (Thread 6)   │ If connected & subscribed
└──────┬───────┘
       │
       │ (if TX fails or not connected)
       ▼
┌──────────────┐
│ storage_mgr  │ NVS Persistent Storage
│ (NVS)        │ Max 100 batches (~10KB)
└──────┬───────┘
       │
       │ (periodic flush every 10s)
       ▼
┌──────────────┐
│ ble_flush    │ Catch-up transmission
│ (Background) │ 10 batches per cycle
└──────────────┘
```

### Module Responsibilities

#### 1. **MAX32664C (Sensor Hub)**
- Hardware: MAX32664C + MAX86174 PPG AFE
- Sampling: 25Hz (40ms interval)
- Outputs: HR (BPM), SpO2 (%), RR, Confidence, SCD
- Thread: `max32664c_sensor` (Priority 5)
- File: `driver/max32664c/max32664c.c`

#### 2. **MAX77658 (PMIC)**
- Hardware: Integrated fuel gauge + charger
- Functions: Rail control, battery monitoring, power states
- Thread: `max77658_pmic` (Priority 7, 2s interval)
- Files: 
  - `driver/pmic/max77658_pm.c` (4700+ lines, **PROTECTED**)
  - `driver/pmic/max77658_fg.c` (1000+ lines, **PROTECTED**)

#### 3. **LSM6DSV32X (IMU)**
- Hardware: 6-axis accelerometer + gyroscope
- Features: Orientation, activity, kick detection
- Sampling: 25Hz
- Thread: `lsm6dsv32x_imu` (Priority 6)
- File: `lib/lsm6dsv32x_main.c`

#### 4. **MAX30208 (Temperature)**
- Hardware: Digital temperature sensor (±0.1°C)
- Sampling: Every 5 seconds (main thread)
- Resolution: 0.01°C (int16_t × 100)
- File: `lib/max30208_main.c`

#### 5. **Data Manager**
- Aggregates multi-source data into batches
- Batch size: 10 vitals samples + latest slow channels
- Thread-safe queue with atomic operations
- File: `src/data_manager.c`

#### 6. **BLE Application**
- Store-and-forward architecture
- Live TX prioritized, falls back to NVS storage
- Notification-based GATT service
- Retry logic for -ENOMEM handling
- File: `src/ble_app.c`

#### 7. **Storage Manager**
- NVS-based circular buffer
- Capacity: 100 batches (~10KB)
- Atomic operations for concurrency
- File: `src/storage_manager.c`

---

## Build Environment

### Requirements
- **SDK**: nRF Connect SDK v3.1.0
- **Toolchain**: Nordic toolchain c1a76fddb2
- **Python**: Embedded in toolchain
- **West**: Nordic's meta-tool
- **Board**: nrf52840dk/nrf52840 or bl54l15_dvk_nrf54l15_cpuapp

### Directory Structure
```
C:\ncs\v3.1.0\                       # SDK Root
├── nrf\                              # nRF libraries
├── zephyr\                           # Zephyr RTOS
├── modules\                          # Third-party modules
└── toolchains\c1a76fddb2\            # Compiler toolchain
    └── opt\bin\python.exe            # West Python

C:\nrf_workspace\pmic_2512_nrf54\     # Project Root
├── CMakeLists.txt                    # Build configuration
├── prj.conf                          # Kconfig settings
├── src\                              # Application code
│   ├── main.c                        # Entry point
│   ├── ble_app.c                     # BLE logic
│   ├── data_manager.c                # Data aggregation
│   └── storage_manager.c             # NVS persistence
├── lib\                              # High-level drivers
│   ├── max77658_main.c
│   ├── max32664c_main.c
│   ├── max30208_main.c
│   └── lsm6dsv32x_main.c
├── driver\                           # Low-level drivers
│   ├── pmic\                         # MAX77658 (PROTECTED)
│   ├── max32664c\                    # Biosensor hub
│   └── max30208\                     # Temperature
├── boards\                           # Board-specific overlays
│   └── bl54l15_dvk_nrf54l15_cpuapp.overlay
└── build_1\                          # Build output
    └── zephyr\zephyr.hex             # Flashable binary
```

---

## Configuration

### Kconfig Options (prj.conf)

#### Clock Configuration
```properties
# Use internal RC oscillator (frees P0.00/P0.01 for GPIO)
CONFIG_CLOCK_CONTROL_NRF_K32SRC_RC=y
CONFIG_CLOCK_CONTROL_NRF_K32SRC_RC_CALIBRATION=y
```

#### I2C & Peripherals
```properties
CONFIG_I2C=y
CONFIG_SPI=y
CONFIG_GPIO=y
CONFIG_SENSOR=y
```

#### Logging
```properties
CONFIG_LOG=y
CONFIG_LOG_MODE_DEFERRED=y
CONFIG_LOG_BUFFER_SIZE=16384
CONFIG_LOG_DEFAULT_LEVEL=3           # Info level
CONFIG_I2C_LOG_LEVEL_DBG=y           # Debug I2C issues
CONFIG_SENSOR_LOG_LEVEL_DBG=y        # Debug sensor data
```

#### PMIC Configuration
```properties
CONFIG_PMIC_MAX77658=y
CONFIG_PMIC_MAX77658_PM=y            # Power management
CONFIG_PMIC_MAX77658_FG=y            # Fuel gauge
CONFIG_PMIC_MAX77658_FG_DESIGN_CAP=350  # 350mAh battery
CONFIG_PMIC_MAX77658_FG_RSENSE=10    # 10mΩ sense resistor
```

#### Bluetooth
```properties
CONFIG_BT=y
CONFIG_BT_PERIPHERAL=y
CONFIG_BT_DEVICE_NAME="Neso Monitor"
CONFIG_BT_MAX_CONN=1
CONFIG_BT_LL_SOFTDEVICE=y            # Use Nordic controller

# Buffers (prevents -ENOMEM)
CONFIG_BT_BUF_ACL_RX_SIZE=251
CONFIG_BT_BUF_ACL_TX_SIZE=251
CONFIG_BT_BUF_ACL_TX_COUNT=8
CONFIG_BT_CONN_TX_MAX=12
CONFIG_BT_L2CAP_TX_MTU=247
CONFIG_BT_L2CAP_TX_BUF_COUNT=8

# TX Power control
CONFIG_BT_HCI=y
CONFIG_BT_CTLR_TX_PWR_DYNAMIC_CONTROL=y
```

#### Non-Volatile Storage
```properties
CONFIG_FLASH=y
CONFIG_NVS=y
CONFIG_SETTINGS=y
CONFIG_MPU_ALLOW_FLASH_WRITE=y
```

#### Memory
```properties
CONFIG_HEAP_MEM_POOL_SIZE=16384      # 16KB heap
CONFIG_MAIN_STACK_SIZE=4096          # 4KB main stack
```

### Device Tree Overlay (boards/bl54l15_dvk_nrf54l15_cpuapp.overlay)

#### MAX32664C Configuration
```dts
max32664c: max32664c@55 {
    compatible = "maxim,max32664c";
    reg = <0x55>;
    
    /* GPIO */
    reset-gpios = <&gpio0 4 GPIO_ACTIVE_LOW>;
    mfio-gpios  = <&gpio1 13 (GPIO_ACTIVE_LOW | GPIO_OPEN_DRAIN | GPIO_PULL_UP)>;
    
    /* SpO2 Calibration (Maxim coefficients × 100,000) */
    spo2-calib = <159584 (-3465966) 11268987>;
    
    /* Sampling Configuration */
    hr-config = <4 1>;                # 4-sample average, 1× decimation
    spo2-config = <4 1>;
    use-max86174;                     # Use MAX86174 AFE
    
    /* Motion Detection */
    motion-time = <5000>;             # 5s motion timeout
    motion-threshold = <100>;
    
    /* LED Configuration */
    min-integration-time = "58_7us";
    min-sampling-rate = "25sps_avg1";
    max-integration-time = "117_3us";
    max-sampling-rate = "400sps_avg16";
    report-period = <40>;             # 40ms = 25Hz
    led-current = <50 50 50 50>;      # 50mA per LED
};
```

---

## API Reference

### PMIC (MAX77658)

#### Initialization
```c
int max77658_app_init(void);
```
- Initializes I2C, GPIO, rails, charger, fuel gauge
- Enforces charger-only boot policy
- **Returns**: 0 on success, negative on failure

#### Thread Start
```c
void max77658_app_start(void);
```
- Spawns background monitoring thread (2s interval)
- Logs battery state, temperature, charger status

#### Power Management
```c
void max77658_enter_software_off(void);
```
- **Wake Sources**: CHGIN + nEN button
- **Use Case**: Normal user shutdown
- **Current**: ~10-50µA

```c
void max77658_enter_ship_mode(void);
```
- **Wake Source**: CHGIN only (charger insertion)
- **Use Case**: Factory shipping, long-term storage
- **Current**: <5µA

```c
void max77658_request_software_off(const char *reason);
```
- Thread-safe shutdown request (atomic)
- Sets flag checked by all threads

```c
bool max77658_shutdown_requested(void);
```
- Returns true if shutdown was requested
- Threads should periodically check this

### Biosensor Hub (MAX32664C)

#### Initialization
```c
int max32664c_app_init(void);
```
- Probes device, loads firmware if needed
- Configures AGC, LED currents, sampling rate
- **Returns**: 0 on success, negative on failure

#### Thread Start
```c
void max32664c_app_start(void);
```
- Spawns sensor polling thread (25Hz)
- Feeds data to `data_manager`

### Data Manager

#### Initialization
```c
void data_manager_init(void);
```
- Initializes internal queues and batch counters

#### Update Functions
```c
void data_manager_push_vitals(uint8_t algo_mode,
                              uint16_t hr_bpm, uint8_t hr_conf,
                              uint16_t spo2_x10, uint8_t spo2_conf,
                              uint16_t rr_x10, uint8_t scd);
```
- Called by MAX32664C thread at 25Hz
- Accumulates samples into batches

```c
void data_manager_update_temp(int16_t temp_c_x100, bool valid);
void data_manager_update_batt(uint8_t pct, uint16_t mv, int16_t ma_x10,
                              uint8_t chg, bool valid);
void data_manager_update_imu(int8_t orient, uint8_t activity,
                             uint16_t kicks, uint8_t kick_rate,
                             uint8_t asleep, uint32_t events,
                             uint16_t alerts, bool valid);
```
- Called by respective sensor threads
- Updates "latest" values appended to each batch

#### Consumer Functions
```c
int data_manager_get_ble_batch(patient_batch_t *batch);
```
- Blocking call (waits for new batch)
- Used by BLE ingest thread
- Returns 0 on success with filled batch

### BLE Application

#### Thread Start
```c
void start_ble_thread(void);
```
- Initializes BLE stack
- Spawns ingest + flush threads
- Starts advertising

#### Shutdown Coordination
```c
void ble_prepare_for_shutdown(void);
```
- Stops advertising
- Disables notifications
- Disconnects active connections

---

## BLE Protocol

### Service UUID
**Primary Service**: `12345678-1234-5678-1234-56789abcdef0`

### Characteristics

| UUID Suffix | Name | Properties | Description |
|-------------|------|------------|-------------|
| ...def1 | Vitals | NOTIFY | HR, SpO2, RR, Confidence, SCD |
| ...def2 | IMU | NOTIFY | Orientation, Activity, Kicks |
| ...def3 | Temperature | NOTIFY | Body temperature (°C × 100) |
| ...def4 | Battery | NOTIFY | Voltage, Current, SoC, Charger |
| ...def5 | Profile | READ/WRITE | User profile (age, height, weight) |
| ...def6 | Config | READ/WRITE | Device config (mode, interval, LED) |
| ...def7 | Info | READ | Device info (firmware version, etc.) |

### Data Structures

#### Vitals Notification (14 bytes)
```c
typedef struct __attribute__((packed)) {
    uint32_t ts_ms;         // Timestamp (k_uptime_get_32)
    uint16_t hr;            // Heart rate (BPM)
    uint16_t spo2;          // SpO2 × 10 (985 = 98.5%)
    uint8_t  hr_conf;       // HR confidence (0-100)
    uint8_t  spo2_conf;     // SpO2 confidence (0-100)
    uint8_t  scd;           // Skin contact (3 = on skin)
    uint8_t  algo_mode;     // Algorithm mode
} ble_vitals_t;
```

#### IMU Notification (16 bytes)
```c
typedef struct __attribute__((packed)) {
    uint32_t ts_ms;
    uint16_t kicks;         // Kick count
    uint8_t  orient;        // Orientation code
    uint8_t  activity;      // Activity level (0-3)
    uint8_t  sleep;         // Sleep-like state (0/1)
    uint32_t events;        // Event bitmask
} ble_imu_t;
```

#### Temperature Notification (6 bytes)
```c
typedef struct __attribute__((packed)) {
    uint32_t ts_ms;
    int16_t  temp_c100;     // Temperature (°C × 100)
} ble_temp_t;
```

#### Battery Notification (10 bytes)
```c
typedef struct __attribute__((packed)) {
    uint32_t ts_ms;
    uint16_t mv;            // Voltage (mV)
    int16_t  ma;            // Current (mA × 10, signed)
    uint8_t  soc;           // State of Charge (0-100%)
    uint8_t  chg;           // Charger present (0/1)
} ble_batt_t;
```

### Connection Parameters
- **Interval**: 50-75ms (40-60 units of 1.25ms)
- **Latency**: 0 (no slave latency)
- **Timeout**: 4s (400 units of 10ms)
- **TX Power**: +8 dBm (max power)

### Transmission Strategy

#### 1. **Live Transmission** (Priority)
- When BLE connected AND notifications enabled
- Data flows directly from `data_manager` → BLE TX
- Zero storage overhead when link is active

#### 2. **Store-and-Forward** (Fallback)
- When disconnected OR notifications disabled
- Data saved to NVS (100-batch circular buffer)
- Periodic flush every 10 seconds when reconnected

#### 3. **Burst Transmission**
- Sends up to 10 batches per cycle (max 100 samples)
- 10ms pacing between vitals samples
- 5ms pacing between slow channels
- 30ms pause between batches

#### 4. **Retry Logic**
- Retries up to 25 times on `-ENOMEM`
- 10ms delay between retries
- Fails gracefully on `-ENOTCONN`

---

## Operational Policies

### Power-On Sequence

The system follows a strict initialization order to ensure safe and reliable startup:

```
1. MCU Boot (Hardware Reset)
   └─> Clock initialization (RC oscillator)
   └─> Memory initialization (heap, stacks)
   └─> Kernel start

2. I2C Bus Initialization
   └─> TWIM21 configured (100kHz, P1.11/P1.12)
   └─> I2C mutex created

3. PMIC Initialization (CRITICAL - BLOCKING)
   └─> CID poll (10s timeout, 50 attempts × 200ms)
   └─> ⚠️ CHGIN validation (STAT_CHG_B.CHGIN_DTLS)
        ├─> If CHGIN_DTLS != 3: Enter ship mode → HALT
        └─> If CHGIN_DTLS == 3: Continue
   └─> GPIO configuration (nEN Hi-Z, nIRQ pull-up)
   └─> Rail configuration (SBB0=1.8V, SBB1=3.3V, SBB2=5.0V)
   └─> Fuel gauge initialization
   └─> Charger configuration
   Result: System powered, battery monitored

4. Sensor Initialization (Non-blocking)
   └─> MAX30208 temperature sensor
        └─> If failed: Log warning, continue
   └─> MAX32664C biosensor hub
        └─> If failed: Log warning, continue
   └─> LSM6DSV32X IMU
        └─> If failed: Log warning, continue

5. Storage Initialization
   └─> NVS partition mount
   └─> Circular buffer setup (100 batches)
   └─> Recovery of pending data (if any)

6. Data Manager Initialization
   └─> Queue creation
   └─> Batch counters reset
   └─> Semaphore initialization

7. BLE Stack Initialization
   └─> bt_enable() blocking call
   └─> Connection callbacks registered
   └─> Advertising start (non-connectable initially)
   └─> GATT service ready

8. Thread Launch (Priority Order)
   └─> Priority 5: max32664c_sensor (vitals sampling)
   └─> Priority 6: lsm6dsv32x_imu (motion processing)
   └─> Priority 6: ble_ingest (data ingestion)
   └─> Priority 7: max77658_pmic (battery monitoring)
   └─> Priority 7: ble_thread (connection manager)
   └─> Priority 7: main (I2C scanner, temp polling)

9. Steady State Operation
   └─> All threads running
   └─> Data flowing sensor → storage → BLE
   └─> PMIC monitoring battery health
```

**Critical Rules:**
- ❌ **No BLE transmission before storage init** (data loss risk)
- ❌ **No sensor polling before PMIC ready** (power instability)
- ✅ **PMIC must succeed or system halts** (safety requirement)
- ✅ **Sensors can fail individually** (graceful degradation)

---

### Shutdown Policy

The system implements coordinated shutdown across all subsystems:

#### Trigger Conditions
1. **Battery Critical** (<5% SoC)
2. **Charger Removed** (optional, configurable)
3. **User Request** (button press, BLE command)
4. **Thermal Shutdown** (>60°C PMIC temperature)
5. **Fault Condition** (I2C failure, watchdog, etc.)

#### Shutdown Sequence
```
1. Shutdown Request
   └─> Any thread calls: max77658_request_software_off("reason")
   └─> Atomic flag set (g_shutdown_requested)

2. Thread Notification (Broadcast)
   └─> All threads check: max77658_shutdown_requested()
   └─> Threads enter cleanup mode

3. Sensor Thread Cleanup (5s grace period)
   └─> max32664c_sensor: Stop sampling, flush last batch
   └─> lsm6dsv32x_imu: Stop motion detection
   └─> max30208: Stop temperature polling

4. Data Manager Flush
   └─> Finish current batch assembly
   └─> Push final batch to storage
   └─> No new data accepted

5. BLE Shutdown Coordination
   └─> ble_prepare_for_shutdown() called
   └─> Stop advertising
   └─> Disable all notifications (g_notify_mask = 0)
   └─> Flush up to 10 pending batches (2s timeout)
   └─> Disconnect active connection gracefully
   └─> Wait for ACK (500ms max)

6. Storage Finalization
   └─> Commit pending NVS writes
   └─> Verify data integrity
   └─> Close NVS partition

7. PMIC Shutdown Execution (Main Thread)
   └─> Disable PMIC IRQ
   └─> Optionally disable SBB rails (ship mode only)
   └─> Release nEN to Hi-Z
   └─> Write SFT_CTRL register:
        ├─> 0x02 = Software Off (CHGIN + nEN wake)
        └─> 0x03 = Ship Mode (CHGIN only wake)
   └─> Infinite loop (device powered off)
```

**Shutdown Types:**

| Type | Function | Wake Sources | Use Case |
|------|----------|--------------|----------|
| **Software Off** | `max77658_enter_software_off()` | CHGIN + nEN | Normal idle, user shutdown |
| **Ship Mode** | `max77658_enter_ship_mode()` | CHGIN only | Factory, RMA, long-term storage |

**Timing Guarantees:**
- Grace period: 5 seconds (threads finish work)
- BLE flush timeout: 2 seconds (best effort)
- Total shutdown time: <10 seconds

---

### IMU Policy

The LSM6DSV32X accelerometer/gyroscope implements intelligent motion detection:

#### Sampling Configuration
- **Accelerometer**: 26 Hz (low power mode)
- **Gyroscope**: On-demand (disabled by default)
- **FIFO**: Disabled (streaming mode)
- **Range**: ±4g (accelerometer), ±500 dps (gyro)

#### Motion Detection States
```
┌─────────────┐
│   IDLE      │ No significant motion detected
│ (Baseline)  │ LED current: Min, Sampling: 1Hz
└──────┬──────┘
       │ Acceleration > 0.3g sustained >100ms
       ▼
┌─────────────┐
│  ACTIVE     │ Normal movement detected
│  (Default)  │ LED current: Normal, Sampling: 25Hz
└──────┬──────┘
       │ Acceleration > 2g spike
       ▼
┌─────────────┐
│  EXERCISE   │ Vigorous activity
│ (High Rate) │ LED current: High, Sampling: 50Hz
└──────┬──────┘
       │ Freefall detected (all axes <0.1g, >300ms)
       ▼
┌─────────────┐
│   ALERT     │ Potential fall event
│  (Emergency)│ Immediate notification, log event
└─────────────┘
```

#### Kick Detection Algorithm
1. **Window**: 1-minute sliding window
2. **Threshold**: Acceleration spike >1.5g, <200ms duration
3. **Direction**: Primarily Z-axis (wrist rotation)
4. **Debounce**: 500ms minimum between kicks
5. **Counter**: Cumulative count, reset every hour

#### Orientation Detection
```c
typedef enum {
    ORIENT_UNKNOWN      = 0,
    ORIENT_UPRIGHT      = 1,  // Standing/sitting
    ORIENT_SUPINE       = 2,  // Lying on back
    ORIENT_PRONE        = 3,  // Lying face down
    ORIENT_LEFT_SIDE    = 4,  // Lying on left
    ORIENT_RIGHT_SIDE   = 5,  // Lying on right
    ORIENT_INVERTED     = 6   // Upside down
} imu_orientation_t;
```
- **Hysteresis**: 2-second stable period required
- **Confidence**: Based on gravity vector magnitude (9.8 ± 0.5 m/s²)

#### Activity Level Classification
```c
typedef enum {
    ACTIVITY_SEDENTARY  = 0,  // <0.1g RMS
    ACTIVITY_LIGHT      = 1,  // 0.1-0.5g RMS
    ACTIVITY_MODERATE   = 2,  // 0.5-1.5g RMS
    ACTIVITY_VIGOROUS   = 3   // >1.5g RMS
} activity_level_t;
```
- **Window**: 10-second RMS calculation
- **Update Rate**: Every 2 seconds

#### Sleep Detection
- **Criteria**: Activity level = SEDENTARY + Orientation stable >10 min
- **Exit**: Any motion >0.3g or orientation change
- **Action**: Reduce MAX32664C LED current, extend sampling interval

#### Event Flags (Bitmask)
```c
#define IMU_EVT_FREEFALL      (1U << 0)  // Freefall detected
#define IMU_EVT_IMPACT        (1U << 1)  // High-g impact (>8g)
#define IMU_EVT_ROLLOVER      (1U << 2)  // Orientation flip
#define IMU_EVT_PROLONGED_SIT (1U << 3)  // >2 hours sedentary
#define IMU_EVT_STEP_DETECTED (1U << 4)  // Walking/running
#define IMU_EVT_GESTURE       (1U << 5)  // Recognized gesture
```

---

### Data Retention Policy

#### NVS Storage Strategy
- **Partition**: 16KB flash region (dedicated)
- **Capacity**: 100 batches × ~100 bytes = ~10KB usable
- **Structure**: Circular buffer with wear leveling
- **Write Endurance**: >100,000 cycles (NVS managed)

#### Storage Triggers
1. **BLE Not Connected** → Store all batches
2. **BLE Connected, No Subscription** → Store all batches
3. **BLE TX Failure** (-ENOMEM, -ENOTCONN) → Store failed batch
4. **Shutdown Requested** → Flush final batch

#### Retrieval Strategy
- **FIFO Order**: Oldest data transmitted first
- **Burst Limit**: 10 batches per flush cycle (prevent radio hogging)
- **Flush Interval**: Every 10 seconds when connected
- **Delete Policy**: Only after successful `bt_gatt_notify()` (ACK received)

#### Data Loss Prevention
```
Priority 1: Live TX (zero latency)
  ↓ (if fails)
Priority 2: NVS Storage (persistent)
  ↓ (if full)
Priority 3: Overwrite oldest (circular buffer)
  ↓ (never)
Priority 4: Drop data ❌ (NEVER HAPPENS)
```

#### Flash Wear Management
- NVS rotates writes across sectors automatically
- Expected lifetime: >1 year at 1 write/second
- No manual wear leveling required

---

### Battery Management Policy

#### Charging Behavior
1. **Fast Charge**: 0.5C (175mA) until 4.0V
2. **Trickle Charge**: 0.1C (35mA) from 4.0V to 4.2V
3. **Termination**: Current <10mA or timeout (3 hours)
4. **Recharge Threshold**: 3.9V (auto-restart)

#### Battery Protection
- **Over-Voltage**: 4.3V cutoff (hardware)
- **Under-Voltage**: 3.0V cutoff (software shutdown)
- **Over-Current**: 500mA limit (hardware)
- **Thermal**: >45°C suspend charging, >60°C emergency shutdown

#### SoC Reporting
- **Method**: Coulomb counting + voltage correlation
- **Accuracy**: ±5% (typical), ±10% (worst case)
- **Calibration**: Every full charge cycle (4.2V → 3.0V → 4.2V)
- **Update Rate**: Every 2 seconds (PMIC thread)

#### Low Battery Actions
| SoC Level | Action | Warning |
|-----------|--------|------|
| <20% | Reduce LED current by 50% | BLE notification |
| <10% | Reduce sampling to 12.5Hz | BLE notification + LED blink |
| <5% | Enter software off mode | Final BLE notification, log flush |

---

### Sensor Fallback Policy

Graceful degradation when sensors fail:

#### MAX32664C Failure (Vitals)
- **Detection**: I2C NACK, invalid data (HR=0, SpO2=0)
- **Action**: 
  - Continue operation with other sensors
  - Push empty vitals (valid_flags &= ~VF_VITALS)
  - Log warning every 60 seconds
  - Retry init every 5 minutes
- **Recovery**: Auto-restart if device responds

#### MAX30208 Failure (Temperature)
- **Detection**: I2C timeout, out-of-range values
- **Action**:
  - Use PMIC die temperature as fallback
  - Mark as invalid (valid_flags &= ~VF_TEMP)
  - Continue normal operation
- **Impact**: Minimal (non-critical sensor)

#### LSM6DSV32X Failure (IMU)
- **Detection**: Zephyr sensor API error
- **Action**:
  - Disable motion-based power optimization
  - Use fixed LED current (no AGC)
  - Report static orientation (UNKNOWN)
  - valid_flags &= ~VF_IMU
- **Impact**: Reduced battery life, no fall detection

#### PMIC Failure (Critical)
- **Detection**: I2C failure during init
- **Action**:
  - **HALT SYSTEM** (infinite loop)
  - Cannot proceed without power management
  - No safe operating mode available
- **Recovery**: Hardware reset required

---

### BLE Connection Policy

#### Advertising Strategy
- **Interval**: 100ms (fast advertising)
- **Timeout**: None (continuous until connected)
- **Connectable**: Yes
- **Scannable**: Yes
- **Directed**: No (undirected advertising)

#### Connection Parameters
- **Initial Request**: 50-75ms interval (power optimized)
- **Negotiation**: Accept central's proposal if reasonable
- **Minimum Acceptable**: 20ms (iOS requirement)
- **Maximum Acceptable**: 200ms (latency tolerance)
- **Latency**: 0 (no slave latency, reliable data delivery)
- **Timeout**: 4 seconds (prevent ghost connections)

#### Disconnection Handling
1. **Immediate Actions**:
   - Set g_notify_mask = 0 (stop TX attempts)
   - Store current batch to NVS
   - Restart advertising

2. **Data Preservation**:
   - All in-flight batches saved to NVS
   - No data loss on unexpected disconnect

3. **Reconnection**:
   - Client must re-subscribe (CCC reset)
   - Automatic catchup transmission starts
   - Flush up to 10 batches immediately

#### Multi-Connection Policy
- **Maximum Connections**: 1 (CONFIG_BT_MAX_CONN=1)
- **Reject Additional**: Automatically reject 2nd connection
- **Reason**: Prevent data duplication, reduce complexity

---

## Power Management

### Boot Policy

The firmware enforces **charger-only boot** to prevent battery drain and boot loops:

```
1. MCU boots (from USB/debugger/residual power)
2. I2C initialized
3. PMIC CID poll (10s timeout, 50 attempts)
4. ⚠️ EARLY CHGIN CHECK - Read STAT_CHG_B.CHGIN_DTLS
5. If CHGIN_DTLS != 3 (charger not valid):
   → Enter ship mode immediately
   → Infinite loop (does not proceed)
6. If CHGIN_DTLS == 3 (charger connected):
   → Continue full initialization
   → Start application threads
```

### Power States

| State | SFT_CTRL | Wake Sources | Current | Function |
|-------|----------|--------------|---------|----------|
| **Active** | 0x00 | N/A | 50-200mA | Normal operation |
| **Software Off** | 0x02 | CHGIN + nEN | 10-50µA | `max77658_enter_software_off()` |
| **Ship Mode** | 0x03 | CHGIN only | <5µA | `max77658_enter_ship_mode()` |

### GPIO Requirements

#### nEN (P1.07) - Power Enable
- **CRITICAL**: Must be `GPIO_INPUT` (Hi-Z, no pull-up)
- Internal pull-up can prevent proper shutdown
- Actively driven LOW only during wake sequence
- Released to Hi-Z after boot

#### nIRQ (P1.06) - Interrupt
- `GPIO_INPUT | GPIO_PULL_UP`
- Falling edge interrupt
- Disabled before ship mode entry

#### nRST (P1.05) - Reset Monitor
- `GPIO_INPUT` (monitor only)
- NOT used for readiness gating
- Debug signal only

### Shutdown Coordination

All threads periodically check:
```c
if (max77658_shutdown_requested()) {
    // Cleanup, flush buffers, etc.
    k_sleep(K_SECONDS(1));
    continue; // Don't proceed with work
}
```

Any thread can request shutdown:
```c
max77658_request_software_off("Battery Critical");
```

Main thread executes shutdown after grace period:
```c
max77658_enter_ship_mode(); // or enter_software_off()
```

---

## Build & Flash Instructions

### Build Command (ALWAYS USE)
```powershell
cd C:\ncs\v3.1.0
& "C:\ncs\toolchains\c1a76fddb2\opt\bin\python.exe" -m west build `
  -b nrf52840dk/nrf52840 `
  "C:\nrf_workspace\pmic_2512_nrf54" `
  --build-dir "C:\nrf_workspace\pmic_2512_nrf54\build_1" `
  --no-sysbuild
```

### Flash Command (West)
```powershell
cd C:\ncs\v3.1.0
& "C:\ncs\toolchains\c1a76fddb2\opt\bin\python.exe" -m west flash `
  --build-dir "C:\nrf_workspace\pmic_2512_nrf54\build_1"
```

### Flash Command (nrfjprog)
```powershell
cd C:\nrf_workspace\pmic_2512_nrf54
nrfjprog --program build_1\zephyr\zephyr.hex --chiperase --verify --reset
```

### Clean Build
```powershell
cd C:\ncs\v3.1.0
& "C:\ncs\toolchains\c1a76fddb2\opt\bin\python.exe" -m west build `
  -b nrf52840dk/nrf52840 `
  "C:\nrf_workspace\pmic_2512_nrf54" `
  --build-dir "C:\nrf_workspace\pmic_2512_nrf54\build_1" `
  --pristine --no-sysbuild
```

### Critical Build Rules
1. ✅ **MUST** run west from `C:\ncs\v3.1.0`
2. ✅ **MUST** include `--no-sysbuild` flag
3. ❌ **DO NOT** modify PMIC register sequences without explicit request
4. ❌ **DO NOT** change I2C addresses (PM: 0x48, FG: 0x36)

---

## Testing & Validation

### I2C Bus Scan
Expected devices:
```
Device found at 0x36  (MAX77658 Fuel Gauge)
Device found at 0x48  (MAX77658 Power Management)
Device found at 0x50  (MAX30208 Temperature)
Device found at 0x55  (MAX32664C Sensor Hub)
Device found at 0x6B  (LSM6DSV32X IMU)
Found 5 device(s) on I2C bus
```

### Vitals Data Validation
Expected output (every 40ms):
```
[00:00:05.040,000] <inf> max32664c_app: HR=75bpm (95%) SpO2=98.5% (90%) 
                                        RR=16.0rpm SCD=3 Mode=3
```

### Battery Monitoring
Expected output (every 2s):
```
[00:00:10.000,000] <inf> max77658_app: Battery: 85% (3950mV, -45.0mA) 
                                        CHG: Yes, Temp: 28.5C
```

### BLE Connection Test
1. Connect using nRF Connect app
2. Enable notifications on Vitals characteristic
3. Verify data rate: ~2.5 notifications/second (10 samples @ 25Hz)
4. Disconnect and reconnect
5. Verify catchup: Should receive buffered data

### Power State Test

#### Ship Mode Entry
```
1. Disconnect charger
2. Verify log: "MAX77658: Entering SHIP MODE (SFT_CTRL=0x03)"
3. Verify current: <10µA (use ammeter)
```

#### Wake from Ship Mode
```
1. Connect charger
2. Verify device boots immediately
3. Check log: "CHGIN valid: proceeding with init"
```

#### nEN Button Test (Should NOT Wake)
```
1. Device in ship mode
2. Press nEN button
3. Verify device stays off
```

---

## Troubleshooting

### Build Errors

#### Error: "No rule to make target 'zephyr.hex'"
**Cause**: Build directory mismatch  
**Solution**: Verify `--build-dir` matches actual build output

#### Error: "west: command not found"
**Cause**: West not in PATH or wrong directory  
**Solution**: Run from `C:\ncs\v3.1.0` with full Python path

#### Error: "CMake Error: Could not find board"
**Cause**: Invalid board name  
**Solution**: Use exact board string: `nrf52840dk/nrf52840`

### Runtime Errors

#### "I2C device not ready"
**Cause**: I2C pins not configured or hardware issue  
**Solution**: 
1. Check Device Tree overlay
2. Verify wiring (SDA=P1.11, SCL=P1.12)
3. Check pull-up resistors (4.7kΩ)

#### "MAX32664C Init Failed"
**Cause**: Device not responding or firmware issue  
**Solution**:
1. Check I2C bus scan (should see 0x55)
2. Verify MFIO/RST GPIO connections
3. Check OE signal (level shifter enable)

#### "BLE TX failed (-ENOMEM)"
**Cause**: Controller buffers full  
**Solution**:
1. Increase `CONFIG_BT_BUF_ACL_TX_COUNT`
2. Add pacing between notifications
3. Check connection interval (should be <100ms)

#### "Device boots then immediately shuts down"
**Cause**: Charger not detected  
**Solution**:
1. Connect USB charger
2. Check CHGIN_DTLS register (should be 0x03)
3. Verify charger detection threshold (4.0V min)

#### "Device won't enter ship mode"
**Cause**: nEN pin misconfigured  
**Solution**:
1. Verify nEN is `GPIO_INPUT` (no pull-up)
2. Check for external pull-up resistor
3. Measure nEN voltage (should be floating)

### BLE Connection Issues

#### "Device not advertising"
**Cause**: Bluetooth stack init failure  
**Solution**:
1. Check `CONFIG_BT=y` in prj.conf
2. Verify SoftDevice controller enabled
3. Check log for `bt_enable` errors

#### "Connection drops frequently"
**Cause**: Poor RF environment or low TX power  
**Solution**:
1. Verify TX power set to +8 dBm
2. Check connection parameters (timeout should be >4s)
3. Reduce distance to central device

#### "Notifications not working"
**Cause**: CCC not enabled  
**Solution**:
1. Enable notifications in nRF Connect app
2. Check `g_notify_mask` value in logs
3. Verify `bt_gatt_notify()` return value

---

## Appendix

### File Structure Reference
```
src/
├── main.c               # Application entry, I2C scanner
├── ble_app.c            # BLE stack, GATT service, store-and-forward
├── ble_app.h
├── data_manager.c       # Multi-source data aggregation
├── data_manager.h
├── storage_manager.c    # NVS persistence layer
├── storage_manager.h
├── pmic_gpio.c          # PMIC GPIO control (nEN, nRST, nIRQ)
└── pmic_gpio.h

lib/
├── max77658_main.c      # PMIC application layer
├── max77658_main.h
├── max32664c_main.c     # Biosensor application layer
├── max32664c_main.h
├── max30208_main.c      # Temperature sensor layer
├── max30208_main.h
├── lsm6dsv32x_main.c    # IMU application layer
├── lsm6dsv32x_main.h
└── comman/
    └── app_i2c_lock.c   # I2C mutex for multi-thread access

driver/
├── pmic/
│   ├── max77658.c       # PMIC driver core
│   ├── max77658_pm.c    # Power management (4700+ lines, PROTECTED)
│   ├── max77658_fg.c    # Fuel gauge (1000+ lines, PROTECTED)
│   ├── max77658_defines.h
│   └── bsp.c            # Board support
├── max32664c/
│   ├── max32664c.c      # Main driver
│   ├── max32664c_init.c
│   ├── max32664c_acc.c
│   ├── max32664c_bl.c
│   ├── max32664c_worker.c
│   └── max32664c_interrupt.c
└── max30208/
    └── max30208.c       # Temperature sensor driver
```

### Key Constants
```c
/* Data Manager */
#define BATCH_SIZE 10              // Samples per batch
#define PAYLOAD_VER 1              // Protocol version

/* BLE */
#define BURST_INTERVAL_MS 10000    // Flush every 10s
#define SAMPLE_PACING_MS 10        // Between vitals samples
#define MAX_BATCHES_PER_FLUSH 10   // Catchup limit

/* Storage */
#define STORAGE_MAX_BATCHES 100    // ~10KB total
#define NVS_PARTITION_SIZE 0x4000  // 16KB flash region

/* PMIC */
#define PMIC_PM_ADDR 0x48          // Power management
#define PMIC_FG_ADDR 0x36          // Fuel gauge
#define DESIGN_CAPACITY_MAH 350    // Battery capacity
#define RSENSE_MOHM 10             // Current sense resistor
```

### Regulatory & Safety
⚠️ **Medical Device Disclaimer**: This device is for research and development purposes only. Not approved for clinical use or diagnosis. SpO2 accuracy is not validated for medical decisions.

⚠️ **Battery Safety**: Use only specified 350mAh Li-Ion battery. Do not exceed 4.2V charging voltage. Implement thermal protection.

⚠️ **RF Compliance**: Ensure BLE TX power complies with local regulations. Default +8 dBm may require licensing in some regions.

---

## Related Documentation

- [README_POWER_MANAGEMENT.md](README_POWER_MANAGEMENT.md) - Power state quick reference
- [SHIP_MODE_GUIDE.md](SHIP_MODE_GUIDE.md) - Detailed ship mode implementation
- [BLE_DATA_PIPELINE.md](docs/BLE_DATA_PIPELINE.md) - BLE data flow architecture
- [.github/copilot-instructions.md](.github/copilot-instructions.md) - AI-assisted build instructions

---

**Document Version**: 1.0  
**Last Updated**: January 22, 2026  
**Firmware Version**: v3.1.0-based  
**Author**: Neso Monitor Development Team
