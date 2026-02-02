# Neso Monitor - App Developer Integration Guide

## 📱 Quick Start

This guide provides everything needed to build iOS/Android apps that communicate with the Neso Monitor BLE device.

---

## 🔌 BLE Connection Basics

### Device Name
```
"Neso Monitor"
```

### Advertising Data
- **Flags**: `BT_LE_AD_GENERAL | BT_LE_AD_NO_BREDR`
- **Service UUID**: `12345678-1234-5678-1234-56789abcdef0`

### Connection Parameters (Expected)
- **Interval**: 50-75 ms
- **Latency**: 0 (instant response)
- **Timeout**: 4 seconds
- **TX Power**: +8 dBm (50m range)

---

## 🆔 UUID Reference

All UUIDs use the base: `12345678-1234-5678-1234-xxxxxxxxxxxx`

### Service UUID
```
12345678-1234-5678-1234-56789abcdef0    // Primary GATT Service
```

### Characteristic UUIDs

| Characteristic | UUID | Properties | Description |
|---------------|------|------------|-------------|
| **Vitals** | `12345678-1234-5678-1234-56789abcdef1` | `NOTIFY` | HR, SpO2, breathing (1 Hz) |
| **IMU** | `12345678-1234-5678-1234-56789abcdef2` | `NOTIFY` | Motion, orientation, kicks |
| **Temperature** | `12345678-1234-5678-1234-56789abcdef3` | `NOTIFY` | Body temperature |
| **Battery** | `12345678-1234-5678-1234-56789abcdef4` | `NOTIFY` | Battery status, charging |
| **Profile** | `12345678-1234-5678-1234-56789abcdef5` | `READ/WRITE` | User profile (age, weight, etc) |
| **Config** | `12345678-1234-5678-1234-56789abcdef6` | `READ/WRITE` | Device configuration |
| **Info** | `12345678-1234-5678-1234-56789abcdef7` | `READ` | Device info (FW version, etc) |

---

## 📊 Data Structures

All multi-byte fields are **little-endian**. All structures are **packed** (no padding).

### 1. Vitals Notification (15 bytes)

**Characteristic**: `12345678-1234-5678-1234-56789abcdef1`

```c
typedef struct __attribute__((packed)) {
    uint32_t ts_ms;        // Timestamp (milliseconds since device boot)
    uint16_t hr;           // Heart rate (bpm)
    uint16_t spo2;         // SpO2 × 10 (985 = 98.5%)
    uint8_t  hr_conf;      // HR confidence (0-100)
    uint8_t  spo2_conf;    // SpO2 confidence (0-100)
    uint8_t  scd;          // Skin contact detector (3 = on skin, 0 = off skin)
    uint8_t  algo_mode;    // Algorithm mode (1=continuous, 2=sampled)
} ble_vitals_t;
```

**Example parsing (Python):**
```python
import struct

def parse_vitals(data: bytes):
    ts_ms, hr, spo2, hr_conf, spo2_conf, scd, algo_mode = struct.unpack('<IHHBBBB', data)
    return {
        'timestamp_ms': ts_ms,
        'heart_rate_bpm': hr,
        'spo2_percent': spo2 / 10.0,  # Convert to 98.5%
        'hr_confidence': hr_conf,
        'spo2_confidence': spo2_conf,
        'on_skin': scd == 3,
        'algorithm_mode': algo_mode
    }
```

**Example parsing (Swift/iOS):**
```swift
struct VitalsData {
    let timestamp: UInt32
    let heartRate: UInt16
    let spo2: Double
    let hrConfidence: UInt8
    let spo2Confidence: UInt8
    let onSkin: Bool
    let algoMode: UInt8
    
    init?(data: Data) {
        guard data.count == 15 else { return nil }
        
        timestamp = data.withUnsafeBytes { $0.load(fromByteOffset: 0, as: UInt32.self) }
        heartRate = data.withUnsafeBytes { $0.load(fromByteOffset: 4, as: UInt16.self) }
        let spo2Raw = data.withUnsafeBytes { $0.load(fromByteOffset: 6, as: UInt16.self) }
        spo2 = Double(spo2Raw) / 10.0
        hrConfidence = data[8]
        spo2Confidence = data[9]
        onSkin = data[10] == 3
        algoMode = data[11]
    }
}
```

---

### 2. IMU Notification (14 bytes)

**Characteristic**: `12345678-1234-5678-1234-56789abcdef2`

```c
typedef struct __attribute__((packed)) {
    uint32_t ts_ms;        // Timestamp
    uint16_t kicks;        // Cumulative kick count
    uint8_t  orient;       // Orientation (see enum below)
    uint8_t  activity;     // Activity level (0-3)
    uint8_t  sleep;        // Sleep detected (0=awake, 1=asleep)
    uint32_t events;       // Event bitmask (see below)
} ble_imu_t;
```

**Orientation Values:**
```c
0 = UNKNOWN
1 = LEFT_SIDE
2 = RIGHT_SIDE
3 = PRONE (face down)
4 = SUPINE (face up)
```

**Event Bitmask:**
```c
#define IMU_EVT_ROLLOVER   (1 << 0)  // Baby rolled over
#define IMU_EVT_FALL_LIKE  (1 << 1)  // Fall detected
#define IMU_EVT_FREEFALL   (1 << 2)  // Freefall detected
#define IMU_EVT_IMPACT     (1 << 3)  // Impact detected
#define IMU_EVT_WAKEUP     (1 << 4)  // Motion after sleep
```

**Example parsing (Python):**
```python
def parse_imu(data: bytes):
    ts_ms, kicks, orient, activity, sleep, events = struct.unpack('<IHBBBBI', data)
    
    orientation_map = {
        0: 'unknown', 1: 'left_side', 2: 'right_side',
        3: 'prone', 4: 'supine'
    }
    
    return {
        'timestamp_ms': ts_ms,
        'kick_count': kicks,
        'orientation': orientation_map.get(orient, 'unknown'),
        'activity_level': activity,  # 0-3
        'asleep': sleep == 1,
        'rollover': bool(events & 0x01),
        'fall_detected': bool(events & 0x02),
        'freefall': bool(events & 0x04),
        'impact': bool(events & 0x08),
        'wakeup': bool(events & 0x10)
    }
```

---

### 3. Temperature Notification (6 bytes)

**Characteristic**: `12345678-1234-5678-1234-56789abcdef3`

```c
typedef struct __attribute__((packed)) {
    uint32_t ts_ms;        // Timestamp
    int16_t  temp_c100;    // Temperature × 100 (2514 = 25.14°C)
} ble_temp_t;
```

**Example parsing:**
```python
def parse_temperature(data: bytes):
    ts_ms, temp_c100 = struct.unpack('<Ih', data)  # 'h' = signed int16
    return {
        'timestamp_ms': ts_ms,
        'temperature_celsius': temp_c100 / 100.0,
        'temperature_fahrenheit': (temp_c100 / 100.0) * 9/5 + 32
    }
```

**Invalid value:** `temp_c100 = -32768` (0x8000) means no reading available.

---

### 4. Battery Notification (10 bytes)

**Characteristic**: `12345678-1234-5678-1234-56789abcdef4`

```c
typedef struct __attribute__((packed)) {
    uint32_t ts_ms;        // Timestamp
    uint16_t mv;           // Battery voltage (mV)
    int16_t  ma;           // Battery current × 10 (signed, -1250 = -125.0 mA)
    uint8_t  soc;          // State of charge (0-100%)
    uint8_t  chg;          // Charger present (0=no, 1=yes)
} ble_batt_t;
```

**Example parsing:**
```python
def parse_battery(data: bytes):
    ts_ms, mv, ma, soc, chg = struct.unpack('<IHhBB', data)
    return {
        'timestamp_ms': ts_ms,
        'voltage_mv': mv,
        'current_ma': ma / 10.0,  # Convert to mA (negative = charging)
        'battery_percent': soc,
        'charging': chg == 1
    }
```

---

### 5. User Profile (READ/WRITE, 5 bytes)

**Characteristic**: `12345678-1234-5678-1234-56789abcdef5`

```c
typedef struct __attribute__((packed)) {
    uint8_t  age;          // Age in years (0 = not set)
    uint8_t  height_cm;    // Height in cm
    uint16_t weight_dg;    // Weight × 10 in kg (185 = 18.5 kg)
    uint8_t  gender;       // 0=unknown, 1=male, 2=female
} user_profile_t;
```

**Write example (Python):**
```python
def write_profile(age: int, height_cm: int, weight_kg: float, gender: int):
    weight_dg = int(weight_kg * 10)
    data = struct.pack('<BBHB', age, height_cm, weight_dg, gender)
    # Write to characteristic UUID ...def5
    return data
```

---

### 6. Device Config (READ/WRITE, 4 bytes)

**Characteristic**: `12345678-1234-5678-1234-56789abcdef6`

```c
typedef struct __attribute__((packed)) {
    uint8_t  op_mode;      // Operating mode (0=off, 1=continuous, 2=periodic)
    uint16_t interval_s;   // Sample interval in seconds (periodic mode only)
    uint8_t  led_current;  // LED brightness (0-255, default 50)
} device_config_t;
```

**Write example:**
```python
def write_config(op_mode: int, interval_s: int, led_current: int):
    data = struct.pack('<BHB', op_mode, interval_s, led_current)
    # Write to characteristic UUID ...def6
    return data
```

---

## 🔔 Notification Behavior

### Vitals (High-Rate)
- **Frequency**: 1 Hz (every second)
- **Batch behavior**: All samples in a batch sent individually
- **Example**: If device batches 10 samples, you'll receive 10 notifications back-to-back

### IMU / Temperature / Battery (Low-Rate)
- **Frequency**: Once per batch (every 10 seconds in continuous mode)
- **Value**: Latest snapshot at batch time

### Catch-Up Sync
When reconnecting after disconnection:
1. Device automatically sends stored data (up to 50 batches = ~20 seconds)
2. Notifications arrive in bursts (10 batches every 500ms)
3. Timestamps allow ordering/deduplication
4. **Important**: Use `ts_ms` to detect old vs. live data

---

## 📱 Connection Flow

### Step 1: Scan & Connect
```python
# Pseudo-code
devices = scan_for_ble_devices(timeout=5)
neso = find_device_by_name("Neso Monitor")
connect(neso)
```

### Step 2: Discover Services
```python
services = discover_services(neso)
hope_service = find_service_by_uuid("12345678-1234-5678-1234-56789abcdef0")
```

### Step 3: Enable Notifications
```python
vitals_char = find_char(hope_service, "...def1")
imu_char = find_char(hope_service, "...def2")
temp_char = find_char(hope_service, "...def3")
batt_char = find_char(hope_service, "...def4")

# Subscribe to all
enable_notifications(vitals_char, on_vitals_received)
enable_notifications(imu_char, on_imu_received)
enable_notifications(temp_char, on_temp_received)
enable_notifications(batt_char, on_batt_received)
```

### Step 4: Set Callbacks
```python
def on_vitals_received(data: bytes):
    vitals = parse_vitals(data)
    print(f"HR: {vitals['heart_rate_bpm']} bpm, SpO2: {vitals['spo2_percent']}%")

def on_imu_received(data: bytes):
    imu = parse_imu(data)
    if imu['rollover']:
        alert_user("Baby rolled over!")
```

---

## ⚠️ Important Considerations

### 1. Timestamp Management
- `ts_ms` is **device uptime** (not wall clock time)
- Resets to 0 on device reboot
- **Solution**: Calculate offset on first notification:
  ```python
  device_boot_time = current_time_ms() - first_ts_ms
  real_time = device_boot_time + ts_ms
  ```

### 2. Data Validity Flags
Not all fields are always valid. Check:
- `scd == 3` → HR/SpO2 valid
- `temp_c100 != -32768` → Temperature valid
- `batt_mv != 0` → Battery data valid

### 3. Confidence Scores
- Only trust HR/SpO2 when confidence > 80%
- Lower confidence during motion or poor contact

### 4. Duplicate Detection
During catch-up sync, use `(ts_ms, batch_seq)` to deduplicate:
```python
seen_samples = set()

def on_vitals_received(data):
    vitals = parse_vitals(data)
    key = (vitals['timestamp_ms'], vitals['heart_rate_bpm'])
    
    if key in seen_samples:
        return  # Skip duplicate
    
    seen_samples.add(key)
    process_vitals(vitals)
```

### 5. Connection Stability
- Device uses 4-second timeout → expect disconnects on interference
- Auto-reconnect logic recommended
- Stored data preserved across disconnects

---

## 🧪 Testing Scenarios

### Scenario 1: Live Streaming
1. Connect to device
2. Subscribe to vitals
3. Observe 1 Hz notifications
4. **Expected**: Latency < 200ms

### Scenario 2: Disconnection Recovery
1. Connect and stream for 10 seconds
2. Disconnect (airplane mode)
3. Wait 15 seconds (device stores data)
4. Reconnect and subscribe
5. **Expected**: Burst of ~15 vitals notifications (catch-up)

### Scenario 3: Low Battery
1. Monitor battery notifications
2. When `soc < 20%`, show warning
3. When `chg == 1`, show charging icon

### Scenario 4: Alert Detection
1. Subscribe to IMU
2. Wait for `events & IMU_EVT_ROLLOVER`
3. **Expected**: Alert within 1 second of roll

---

## 📚 Code Examples

### Full iOS/Swift Example
```swift
import CoreBluetooth

class NesoMonitor: NSObject, CBCentralManagerDelegate, CBPeripheralDelegate {
    var centralManager: CBCentralManager!
    var nesoDevice: CBPeripheral?
    
    let SERVICE_UUID = CBUUID(string: "12345678-1234-5678-1234-56789ABCDEF0")
    let VITALS_UUID = CBUUID(string: "12345678-1234-5678-1234-56789ABCDEF1")
    
    override init() {
        super.init()
        centralManager = CBCentralManager(delegate: self, queue: nil)
    }
    
    func centralManagerDidUpdateState(_ central: CBCentralManager) {
        if central.state == .poweredOn {
            centralManager.scanForPeripherals(
                withServices: [SERVICE_UUID],
                options: nil
            )
        }
    }
    
    func centralManager(_ central: CBCentralManager,
                       didDiscover peripheral: CBPeripheral,
                       advertisementData: [String: Any],
                       rssi RSSI: NSNumber) {
        
        if peripheral.name == "Neso Monitor" {
            nesoDevice = peripheral
            centralManager.stopScan()
            centralManager.connect(peripheral, options: nil)
        }
    }
    
    func centralManager(_ central: CBCentralManager,
                       didConnect peripheral: CBPeripheral) {
        peripheral.delegate = self
        peripheral.discoverServices([SERVICE_UUID])
    }
    
    func peripheral(_ peripheral: CBPeripheral,
                   didDiscoverServices error: Error?) {
        guard let services = peripheral.services else { return }
        
        for service in services {
            if service.uuid == SERVICE_UUID {
                peripheral.discoverCharacteristics([VITALS_UUID], for: service)
            }
        }
    }
    
    func peripheral(_ peripheral: CBPeripheral,
                   didDiscoverCharacteristicsFor service: CBService,
                   error: Error?) {
        guard let characteristics = service.characteristics else { return }
        
        for char in characteristics {
            if char.uuid == VITALS_UUID {
                peripheral.setNotifyValue(true, for: char)
            }
        }
    }
    
    func peripheral(_ peripheral: CBPeripheral,
                   didUpdateValueFor characteristic: CBCharacteristic,
                   error: Error?) {
        guard let data = characteristic.value else { return }
        
        if characteristic.uuid == VITALS_UUID {
            if let vitals = VitalsData(data: data) {
                print("HR: \(vitals.heartRate) bpm, SpO2: \(vitals.spo2)%")
            }
        }
    }
}
```

### Full Android/Kotlin Example
```kotlin
import android.bluetooth.*
import android.content.Context
import java.nio.ByteBuffer
import java.nio.ByteOrder
import java.util.UUID

class NesoMonitor(context: Context) {
    private val bluetoothManager = context.getSystemService(Context.BLUETOOTH_SERVICE) as BluetoothManager
    private val bluetoothAdapter = bluetoothManager.adapter
    private var gatt: BluetoothGatt? = null
    
    companion object {
        val SERVICE_UUID = UUID.fromString("12345678-1234-5678-1234-56789ABCDEF0")
        val VITALS_UUID = UUID.fromString("12345678-1234-5678-1234-56789ABCDEF1")
        val CCC_UUID = UUID.fromString("00002902-0000-1000-8000-00805f9b34fb")
    }
    
    fun connect(deviceAddress: String) {
        val device = bluetoothAdapter.getRemoteDevice(deviceAddress)
        gatt = device.connectGatt(context, false, gattCallback)
    }
    
    private val gattCallback = object : BluetoothGattCallback() {
        override fun onConnectionStateChange(gatt: BluetoothGatt, status: Int, newState: Int) {
            if (newState == BluetoothProfile.STATE_CONNECTED) {
                gatt.discoverServices()
            }
        }
        
        override fun onServicesDiscovered(gatt: BluetoothGatt, status: Int) {
            val service = gatt.getService(SERVICE_UUID)
            val vitalsChar = service?.getCharacteristic(VITALS_UUID)
            
            vitalsChar?.let {
                gatt.setCharacteristicNotification(it, true)
                
                // Enable GATT notifications
                val descriptor = it.getDescriptor(CCC_UUID)
                descriptor.value = BluetoothGattDescriptor.ENABLE_NOTIFICATION_VALUE
                gatt.writeDescriptor(descriptor)
            }
        }
        
        override fun onCharacteristicChanged(gatt: BluetoothGatt,
                                             characteristic: BluetoothGattCharacteristic) {
            if (characteristic.uuid == VITALS_UUID) {
                val vitals = parseVitals(characteristic.value)
                println("HR: ${vitals.hr} bpm, SpO2: ${vitals.spo2}%")
            }
        }
    }
    
    data class VitalsData(
        val timestamp: Long,
        val hr: Int,
        val spo2: Double,
        val hrConf: Int,
        val spo2Conf: Int,
        val onSkin: Boolean
    )
    
    private fun parseVitals(data: ByteArray): VitalsData {
        val buffer = ByteBuffer.wrap(data).order(ByteOrder.LITTLE_ENDIAN)
        
        return VitalsData(
            timestamp = buffer.int.toLong() and 0xFFFFFFFFL,
            hr = buffer.short.toInt() and 0xFFFF,
            spo2 = (buffer.short.toInt() and 0xFFFF) / 10.0,
            hrConf = buffer.get().toInt() and 0xFF,
            spo2Conf = buffer.get().toInt() and 0xFF,
            onSkin = (buffer.get().toInt() and 0xFF) == 3
        )
    }
}
```

---

## 🐛 Troubleshooting

### No Notifications Received
1. **Check**: Did you enable CCC (Client Characteristic Configuration)?
2. **Check**: Is device showing "connected" in logs?
3. **Try**: Write to Config characteristic to trigger activity

### Incorrect Data Parsing
1. **Verify**: Little-endian byte order
2. **Verify**: Structure packing (no padding)
3. **Test**: Parse known values (e.g., temp_c100 = 2500 → 25.0°C)

### Connection Drops
1. **Increase**: Connection timeout (4s minimum)
2. **Reduce**: Radio interference (WiFi, microwave)
3. **Check**: Battery level (low battery → unstable radio)

### Old Data After Reconnect
1. **Expected**: Device sends buffered data first
2. **Solution**: Check `ts_ms` against last received timestamp
3. **Filter**: Discard samples with `ts_ms < last_seen_ts`

---

## 📖 Additional Resources

- **BLE Pipeline Architecture**: [`docs/BLE_DATA_PIPELINE.md`](docs/BLE_DATA_PIPELINE.md)
- **Storage Manager Details**: [`src/storage_manager.c`](src/storage_manager.c)
- **Data Manager Implementation**: [`src/data_manager.c`](src/data_manager.c)
- **BLE App Implementation**: [`src/ble_app.c`](src/ble_app.c)

---

## 🆘 Support

For questions or issues:
1. Check device logs (requires debug build)
2. Verify UUIDs match exactly (case-insensitive)
3. Test with nRF Connect app (iOS/Android) to validate device behavior
4. Ensure device firmware version matches this documentation

---

**Last Updated**: January 2026  
**Firmware Version**: BM_ESO_V1_0  
**Compatible Platforms**: iOS 13+, Android 8+, Web Bluetooth (Chrome 89+)
