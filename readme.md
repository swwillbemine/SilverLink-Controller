# 🌱 SilverLink Controller
### Kode Mikrokontroller untuk Sistem IoT SilverLink

<div align="center">

![SilverLink](https://img.shields.io/badge/SilverLink-IoT%20System-blue?style=for-the-badge)
![ESP32](https://img.shields.io/badge/ESP32-Compatible-green?style=for-the-badge)
![Lisensi: MIT](https://img.shields.io/badge/Lisensi-MIT-kuning.svg?style=for-the-badge)


</div>

---

## 📋 Deskripsi

**SilverLink Controller** adalah firmware mikrokontroller untuk sistem IoT SilverLink yang mengintegrasikan sensor-sensor pertanian cerdas dengan backend **SilverCore** (Node.js) dan aplikasi mobile **SilverLink: Connect**. Controller ini mendukung mode online/offline dengan kemampuan konfigurasi dinamis melalui WebSocket.

### 🏗️ Arsitektur Sistem SilverLink

```
┌─────────────────┐    WebSocket    ┌─────────────────┐    REST API     ┌─────────────────┐
│  SilverLink     │◄───────────────►│   SilverCore    │◄───────────────►│  SilverLink:    │
│  Controller     │                 │   (Backend)     │                 │   Connect       │
│  (ESP32)        │                 │   (Node.js)     │                 │  (Mobile App)   │
└─────────────────┘                 └─────────────────┘                 └─────────────────┘
```

---

## ✨ Fitur Utama

### 🌐 **Konektivitas**
- **Mode Online**: Terhubung dengan backend SilverCore melalui WebSocket
- **Mode Offline**: Operasi mandiri dengan timer otomatis
- **Auto-reconnect**: Reconnection otomatis saat koneksi terputus
- **WiFi Setup**: Portal konfigurasi WiFi dengan captive portal

### 📊 **Sensor Support**
- **DHT11**: Sensor suhu dan kelembaban udara
- **Soil Moisture**: Sensor kelembaban tanah dengan threshold
- **Extensible**: Mudah menambah sensor baru melalui konfigurasi JSON

### ⚡ **Aktuator Control**
- **Relay Control**: Kontrol pompa air dan perangkat lain
- **Remote Control**: Kontrol jarak jauh melalui WebSocket
- **Offline Automation**: Otomasi berdasarkan threshold sensor

### 🖥️ **User Interface**
- **LCD Display**: Informasi status real-time (20x4 I2C)
- **Web Interface**: Setup portal dengan desain responsif
- **Status Monitoring**: Monitoring koneksi dan mode operasi

---

## 🛠️ Hardware Requirements

### **Platform Utama**
- **ESP32 DevKit V1/V2** ✅
- **ESP32-WROOM-32** ✅

### **Komponen Pendukung**
- LCD I2C 20x4 (Address: 0x27)
- DHT11 Temperature & Humidity Sensor
- Soil Moisture Sensor (Analog)
- Relay Module untuk aktuator
- Push Button untuk reset (Pin 4)

### **Koneksi Hardware**
```
ESP32 Pin    │ Komponen
─────────────┼──────────────────
GPIO 21      │ LCD SDA
GPIO 22      │ LCD SCL
GPIO 4       │ Reset Button
GPIO xx      │ DHT11 Data (Configurable)
GPIO xx      │ Soil Moisture (Configurable)
GPIO xx      │ Relay Control (Configurable)
```

---

## 📚 Library Dependencies

### **Library Standar (Arduino Library Manager)**
```cpp
#include <WiFi.h>                    // ESP32 WiFi
#include <ESPAsyncWebServer.h>       // Async Web Server
#include <LittleFS.h>               // File System
#include <ArduinoJson.h>            // JSON Parser
#include <WebSocketsClient.h>       // WebSocket Client
#include <DHT.h>                    // DHT Sensor
#include <Wire.h>                   // I2C Communication
#include <LiquidCrystal_I2C.h>      // LCD I2C
```

### **Library Eksternal (Manual Install)**

| Library | Sumber | Keterangan |
|---------|---------|------------|
| **ESPAsyncWebServer** | [🔗 me-no-dev/ESPAsyncWebServer](https://github.com/me-no-dev/ESPAsyncWebServer) | Async web server untuk ESP32 |
| **AsyncTCP** | [🔗 me-no-dev/AsyncTCP](https://github.com/me-no-dev/AsyncTCP) | Dependency untuk ESPAsyncWebServer |
| **WebSocketsClient** | [🔗 Links2004/arduinoWebSockets](https://github.com/Links2004/arduinoWebSockets) | WebSocket client library |

### **Instalasi Library Manual**
1. Download library dari GitHub (ZIP)
2. Arduino IDE → Sketch → Include Library → Add .ZIP Library
3. Pilih file ZIP yang sudah didownload
4. Restart Arduino IDE

---

## 🚀 Setup & Instalasi

### **1. Persiapan Environment**
```bash
# Install Arduino IDE atau PlatformIO
# Tambahkan ESP32 Board Manager:
# https://dl.espressif.com/dl/package_esp32_index.json
```

### **2. Upload Firmware**
1. Buka kode di Arduino IDE
2. Pilih board: **ESP32 Dev Module**
3. Pilih port COM yang sesuai
4. Upload firmware ke ESP32

### **3. Konfigurasi Awal**
1. **Pertama kali**: ESP32 akan membuat hotspot `SilverLink-Setup`
2. Hubungkan ke hotspot tersebut
3. Buka browser → http://192.168.4.1
4. Masukkan:
   - **SSID WiFi**: Nama jaringan WiFi
   - **Password WiFi**: Password jaringan
   - **Token SilverLink**: Token device dari Aplikasi SilverLink: Connect

### **4. Reset Konfigurasi**
- Tekan dan tahan tombol reset (Pin 4) saat boot
- Konfigurasi akan terhapus dan kembali ke setup mode

---

## 🔧 Konfigurasi Sensor & Aktuator

### **Format Konfigurasi JSON**
Controller menerima konfigurasi dinamis dari backend dalam format:

```json
{
  "event": "runtime_config",
  "sensors": [
    {
      "type": "dht11",
      "pin": 2
    },
    {
      "type": "soil_moisture",
      "pin": 34,
      "threshold": 30
    }
  ],
  "aktuators": [
    {
      "function": "pump",
      "pin": 5
    }
  ]
}
```

### **Tipe Sensor Supported**
- `dht11`: Temperature & Humidity sensor
- `soil_moisture`: Analog soil moisture sensor
- `dht_temperature`: Temperature only (dari DHT11)
- `dht_humidity`: Humidity only (dari DHT11)

### **Tipe Aktuator Supported**
- `pump`: Water pump control
- `fan`: Cooling fan control
- `light`: LED/Light control

---

## 📡 Komunikasi WebSocket

### **Event dari Controller ke Backend**
```json
{
  "event": "auth",
  "token": "device_token"
}

{
  "event": "sensor_data",
  "token": "device_token",
  "data": [
    {
      "type": "soil_moisture",
      "pin": 34,
      "value": 45.2
    }
  ]
}
```

### **Event dari Backend ke Controller**
```json
{
  "event": "set_aktuator",
  "pin": 5,
  "state": true
}

{
  "event": "runtime_config",
  "sensors": [...],
  "aktuators": [...]
}

{
  "event": "refresh_config"
}
```

---

## 🔄 Mode Operasi

### **🟢 Mode Online**
- Terhubung ke WiFi dan backend SilverCore
- Mengirim data sensor setiap 5 detik
- Menerima perintah kontrol real-time
- Status: `ONLINE`

### **🟡 Mode Offline WiFi**
- WiFi terputus, namun device tetap beroperasi
- Durasi: 30 menit, kemudian restart
- Automasi berdasarkan threshold sensor
- Status: `OFFLINE_WIFI`

### **🔴 Mode Offline Server**
- WiFi terhubung, namun server tidak dapat diakses
- Durasi: 6 jam, kemudian restart
- Automasi lokal tetap berjalan
- Status: `OFFLINE_SERVER`

---

## 📱 Monitoring & Debugging

### **LCD Display Info**
```
===SilverLink-IoT===
WiFi: MyNetwork    
IP: 192.168.1.100
Server Terhubung   
```

### **Serial Monitor**
```
=================================
  SilverLink-IoT System Start!
Created With ❤️ for Silver Wolf
=================================
📡 Terhubung ke SSID: MyNetwork - IP Address: 192.168.1.100
🔗 Terhubung ke server
📥 Pesan: {"event":"runtime_config",...}
⚡ Aktuator pin 5 => ON
🚿 Pompa aktif (offline)
```

---

## ⚙️ Kustomisasi

### **Menambah Sensor Baru**
1. Tambahkan case baru di function `sendSensorData()`
2. Implementasikan pembacaan sensor
3. Update konfigurasi JSON di backend

### **Menambah Aktuator Baru**
1. Tambahkan case baru di function `handleServerCommand()`
2. Implementasikan kontrol aktuator
3. Update konfigurasi JSON di backend

### **Mengubah Server WebSocket**
```cpp
// Ganti di function setup()
webSocket.begin("your-server.com", "your-websocket-port", "/ws");
```

---

## 🐛 Troubleshooting

| Problem | Solution |
|---------|----------|
| **WiFi tidak connect** | Periksa SSID/password, atau reset konfigurasi |
| **Server tidak connect** | Periksa koneksi internet dan server backend |
| **Sensor tidak terbaca** | Periksa wiring dan konfigurasi pin |
| **LCD tidak menyala** | Periksa alamat I2C (biasanya 0x27 atau 0x3F) |
| **Reset tidak berfungsi** | Pastikan tombol terhubung ke Pin 4 dan GND |

---

## 📄 License

MIT License - Feel free to modify and distribute

---

## 🤝 Contributing

Contributions are welcome! Please feel free to submit a Pull Request. Silver Wolf is waiting for you.

---

<div align="center">

**SilverLink**  
*One link, Total Control*

Created With ❤️ for Silver Wolf

</div>