  // SilverLink ESP32 Full Integration - ENHANCED VERSION dengan State Sync
// WebSocket + Offline Mode + DHT11 + Soil Moisture + State Persistence + Dynamic Config
// OPTIMIZED: No LCD dependency, enhanced serial logging, improved stability

#include <WiFi.h>
#include <ESPAsyncWebServer.h>
#include <LittleFS.h>
#include <ArduinoJson.h>
#include <WebSocketsClient.h>
#include <DHT.h>

#define DHTTYPE DHT11
#define RESET_PIN 4

// Soil Moisture Calibration (from your calibration code)
#define SENSOR_PIN 34
int minValue = 2300;  // Dry soil (highest ADC value)
int maxValue = 2100;  // Wet soil (lowest ADC value)

// Basic monitoring
#define VOLTAGE_PIN 35  // Optional voltage monitoring
bool powerMonitoringEnabled = false;

// Global variables
WebSocketsClient webSocket;
AsyncWebServer server(80);
std::vector<String> ssidList;
String ssid, password, token;

// Runtime config
struct Sensor {
  String type;
  int pin;
  int threshold = -1;
  float value = 0;
};

struct Aktuator {
  String function;
  int pin;
  bool state = false;
  bool pendingStateSync = false;  // NEW: Track pending sync
};

std::vector<Sensor> sensors;
std::vector<Aktuator> aktuators;

// MODE STATUS
enum StatusMode { ONLINE, OFFLINE_WIFI, OFFLINE_SERVER };
StatusMode mode = ONLINE;
unsigned long offlineUntil = 0;
unsigned long lastMoistureCheck = 0;
const unsigned long moistureInterval = 5 * 60 * 1000;

bool runtimeLoaded = false;
int serverFailCount = 0;
int wifiFailCount = 0;
unsigned long lastHeartbeat = 0;
const unsigned long heartbeatInterval = 30000; // 30 seconds

// System monitoring
unsigned long systemStartTime = 0;
unsigned long lastMemoryCheck = 0;
unsigned long lastStatusReport = 0;
int consecutiveErrors = 0;
const int maxConsecutiveErrors = 5;

// NEW: State management
unsigned long lastStateSyncRequest = 0;
const unsigned long stateSyncRequestInterval = 60000; // Request sync every minute if needed
bool pendingStateSync = false;
unsigned long lastStateSync = 0;

// Function declarations
void checkResetButton();
void saveConfig(String ssid, String password, String token);
bool loadConfig();
bool loadRuntimeConfig();
void saveRuntimeConfig(const String& jsonString);
void scanNetworks();
String htmlForm();
void webSocketEvent(WStype_t type, uint8_t * payload, size_t length);
void sendSensorData();
void handleServerCommand(const char* message);
void checkOfflineMode();
void monitorConnection();
void basicSystemMonitoring();
float readSoilMoisture(int pin);
void printSystemStatus();
void printConnectionInfo();

// NEW: State sync functions
void requestStateSync();
void applyActuatorStateSync(const JsonArray& actuators);
void confirmActuatorState(int pin, bool state, bool success);
void checkPendingStateSync();
void saveActuatorState(int pin, bool state);
bool loadActuatorStates();
void syncActuatorWithDatabase(int pin, bool expectedState);

void checkResetButton() {
  pinMode(RESET_PIN, INPUT_PULLUP);
  delay(300);
  if (digitalRead(RESET_PIN) == LOW) {
    Serial.println("🧨 FACTORY RESET - Button pressed, clearing configuration...");
    Serial.println("   - Removing config.json");
    Serial.println("   - Removing config_runtime.json");
    Serial.println("   - Removing actuator_states.json");
    Serial.println("   - Restarting in 3 seconds...");
    
    LittleFS.begin();
    LittleFS.remove("/config.json");
    LittleFS.remove("/config_runtime.json");
    LittleFS.remove("/actuator_states.json");  // NEW: Remove state file
    delay(3000);
    ESP.restart();
  }
}

void saveConfig(String ssid, String password, String token) {
  DynamicJsonDocument doc(512);
  doc["ssid"] = ssid;
  doc["password"] = password;
  doc["token"] = token;
  
  File configFile = LittleFS.open("/config.json", "w");
  if (configFile) {
    serializeJson(doc, configFile);
    configFile.close();
    Serial.println("✅ Configuration saved successfully");
    Serial.printf("   - SSID: %s\n", ssid.c_str());
    Serial.printf("   - Token: %s...\n", token.substring(0, 8).c_str());
  } else {
    Serial.println("❌ Failed to save configuration file");
  }
}

bool loadConfig() {
  if (!LittleFS.begin()) {
    Serial.println("❌ LittleFS mount failed");
    return false;
  }
  
  File configFile = LittleFS.open("/config.json", "r");
  if (!configFile) {
    Serial.println("📋 No configuration file found - first time setup required");
    return false;
  }
  
  if (configFile.size() > 1024) {
    Serial.println("⚠️ Configuration file too large, possibly corrupted");
    configFile.close();
    return false;
  }
  
  DynamicJsonDocument doc(512);
  DeserializationError error = deserializeJson(doc, configFile);
  configFile.close();
  
  if (error) {
    Serial.printf("❌ Configuration JSON parse error: %s\n", error.c_str());
    return false;
  }
  
  ssid = doc["ssid"].as<String>();
  password = doc["password"].as<String>();
  token = doc["token"].as<String>();
  
  if (ssid.isEmpty() || token.isEmpty()) {
    Serial.println("⚠️ Configuration incomplete - missing SSID or token");
    return false;
  }
  
  Serial.println("✅ Configuration loaded successfully");
  Serial.printf("   - SSID: %s\n", ssid.c_str());
  Serial.printf("   - Token: %s...\n", token.substring(0, 8).c_str());
  
  return true;
}

bool loadRuntimeConfig() {
  if (!LittleFS.exists("/config_runtime.json")) {
    Serial.println("📋 No runtime configuration file found");
    return false;
  }
  
  File file = LittleFS.open("/config_runtime.json", "r");
  if (!file || file.size() == 0) {
    Serial.println("❌ Runtime configuration file empty or invalid");
    if (file) file.close();
    return false;
  }

  if (file.size() > 2048) {
    Serial.println("⚠️ Runtime configuration file too large");
    file.close();
    return false;
  }

  DynamicJsonDocument doc(2048);
  DeserializationError error = deserializeJson(doc, file);
  file.close();
  
  if (error) {
    Serial.printf("❌ Runtime configuration JSON parse error: %s\n", error.c_str());
    return false;
  }
  
  // Clear existing configuration
  sensors.clear(); 
  aktuators.clear();
  
  // Load sensors with validation
  JsonArray sensorArray = doc["sensors"];
  if (sensorArray) {
    Serial.println("📡 Loading sensor configuration:");
    for (JsonObject s : sensorArray) {
      String type = s["type"].as<String>();
      int pin = s["pin"];
      int threshold = s["threshold"] | -1;

      // Pin validation
      if (pin < 0 || pin > 39) {
        Serial.printf("   ⚠️ Invalid sensor pin: %d - skipping\n", pin);
        continue;
      }

      if (type == "dht11") {
        Sensor tempSensor;
        tempSensor.type = "dht_temperature";
        tempSensor.pin = pin;
        sensors.push_back(tempSensor);

        Sensor humSensor;
        humSensor.type = "dht_humidity";
        humSensor.pin = pin;
        sensors.push_back(humSensor);
        
        Serial.printf("   ✅ DHT11 sensor on pin %d (temperature + humidity)\n", pin);
      } else {
        Sensor sensor;
        sensor.type = type;
        sensor.pin = pin;
        sensor.threshold = threshold;
        sensors.push_back(sensor);
        
        Serial.printf("   ✅ %s sensor on pin %d", type.c_str(), pin);
        if (threshold > 0) {
          Serial.printf(" (threshold: %d)", threshold);
        }
        Serial.println();
      }

      pinMode(pin, INPUT);
    }
  }

  // Load actuators with validation - ENHANCED dengan state management
  JsonArray actuatorArray = doc["aktuators"];
  if (actuatorArray) {
    Serial.println("⚡ Loading actuator configuration:");
    for (JsonObject a : actuatorArray) {
      String function = a["function"].as<String>();
      int pin = a["pin"];
      bool currentState = a["current_state"] | false;  // NEW: Load state from server
      
      // Pin validation
      if (pin < 0 || pin > 39) {
        Serial.printf("   ⚠️ Invalid actuator pin: %d - skipping\n", pin);
        continue;
      }
      
      Aktuator akt;
      akt.function = function;
      akt.pin = pin;
      akt.state = currentState;  // NEW: Set state from server config
      akt.pendingStateSync = true;  // NEW: Mark for sync verification
      aktuators.push_back(akt);
      
      pinMode(akt.pin, OUTPUT); 
      digitalWrite(akt.pin, currentState ? HIGH : LOW);  // NEW: Set pin to server state
      
      Serial.printf("   ✅ %s actuator on pin %d (initialized %s)\n", 
                    function.c_str(), pin, currentState ? "ON" : "OFF");
    }
  }
  
  // Load saved actuator states from filesystem
  loadActuatorStates();
  
  runtimeLoaded = true;
  Serial.printf("✅ Runtime configuration loaded successfully: %d sensors, %d actuators\n", 
                sensors.size(), aktuators.size());
  
  // Request state sync after loading config
  pendingStateSync = true;
  
  return true;
}

void saveRuntimeConfig(const String& jsonString) {
  if (jsonString.length() > 2048) {
    Serial.println("⚠️ Runtime configuration too large, skipping save");
    return;
  }
  
  File f = LittleFS.open("/config_runtime.json", "w");
  if (f) {
    f.print(jsonString);
    f.close();
    Serial.println("💾 Runtime configuration saved to filesystem");
    loadRuntimeConfig();
  } else {
    Serial.println("❌ Failed to save runtime configuration");
  }
}

// NEW: Save actuator states to filesystem
void saveActuatorState(int pin, bool state) {
  DynamicJsonDocument doc(1024);
  
  // Load existing states
  File file = LittleFS.open("/actuator_states.json", "r");
  if (file) {
    deserializeJson(doc, file);
    file.close();
  }
  
  // Update state for this pin
  doc[String(pin)] = state;
  
  // Save back to file
  file = LittleFS.open("/actuator_states.json", "w");
  if (file) {
    serializeJson(doc, file);
    file.close();
    Serial.printf("💾 Actuator state saved: Pin %d = %s\n", pin, state ? "ON" : "OFF");
  }
}

// NEW: Load actuator states from filesystem
bool loadActuatorStates() {
  if (!LittleFS.exists("/actuator_states.json")) {
    Serial.println("📋 No saved actuator states found");
    return false;
  }
  
  File file = LittleFS.open("/actuator_states.json", "r");
  if (!file) {
    Serial.println("❌ Failed to open actuator states file");
    return false;
  }
  
  DynamicJsonDocument doc(1024);
  DeserializationError error = deserializeJson(doc, file);
  file.close();
  
  if (error) {
    Serial.printf("❌ Actuator states JSON parse error: %s\n", error.c_str());
    return false;
  }
  
  Serial.println("📋 Loading saved actuator states:");
  
  // Apply saved states to actuators
  for (Aktuator &akt : aktuators) {
    String pinKey = String(akt.pin);
    if (doc.containsKey(pinKey)) {
      bool savedState = doc[pinKey];
      akt.state = savedState;
      digitalWrite(akt.pin, savedState ? HIGH : LOW);
      Serial.printf("   ✅ Pin %d (%s): %s (from saved state)\n", 
                    akt.pin, akt.function.c_str(), savedState ? "ON" : "OFF");
    }
  }
  
  return true;
}

void scanNetworks() {
  WiFi.mode(WIFI_STA);
  WiFi.disconnect();
  delay(100);
  
  Serial.println("🔍 Scanning available WiFi networks...");
  int n = WiFi.scanNetworks();
  ssidList.clear();
  
  if (n > 0) {
    Serial.printf("   Found %d networks:\n", n);
    for (int i = 0; i < n && i < 20; i++) {
      String s = WiFi.SSID(i);
      if (s.length() > 0 && s.length() < 32) {
        ssidList.push_back(s);
        Serial.printf("   %2d. %s (RSSI: %d dBm)\n", i+1, s.c_str(), WiFi.RSSI(i));
      }
    }
  } else {
    Serial.println("   No networks found");
  }
}

// Improved soil moisture reading with proper calibration
float readSoilMoisture(int pin) {
  // Take multiple readings for stability
  long sum = 0;
  const int samples = 5;
  
  for (int i = 0; i < samples; i++) {
    sum += analogRead(pin);
    delay(10);
  }
  
  int rawValue = sum / samples;
  
  // Apply calibrated mapping
  float moisture = map(rawValue, minValue, maxValue, 61, 86);
  moisture = constrain(moisture, 0, 100);
  
  return moisture;
}

String htmlForm() {
  String html = R"rawliteral(
<!DOCTYPE html>
<html lang="id">
<head>
  <meta charset="UTF-8">
  <meta name="viewport" content="width=device-width, initial-scale=1.0">
  <title>SilverLink IoT Setup</title>
  <style>
    * {
      box-sizing: border-box;
      margin: 0;
      padding: 0;
      font-family: 'Segoe UI', Tahoma, Geneva, Verdana, sans-serif;
    }
    
    body {
      background: linear-gradient(135deg, #667eea 0%, #764ba2 100%);
      display: flex;
      justify-content: center;
      align-items: center;
      min-height: 100vh;
      padding: 20px;
    }
    
    .container {
      background-color: white;
      border-radius: 15px;
      box-shadow: 0 8px 32px rgba(0, 0, 0, 0.1);
      backdrop-filter: blur(10px);
      width: 100%;
      max-width: 450px;
      padding: 40px;
    }
    
    h1 {
      color: #333;
      font-size: 28px;
      text-align: center;
      margin-bottom: 30px;
      font-weight: 300;
    }
    
    .version {
      text-align: center;
      color: #666;
      font-size: 14px;
      margin-bottom: 25px;
    }
    
    .form-group {
      margin-bottom: 25px;
    }
    
    label {
      display: block;
      font-weight: 600;
      margin-bottom: 8px;
      color: #555;
      font-size: 14px;
    }
    
    select, input[type="text"], input[type="password"] {
      width: 100%;
      padding: 15px;
      border: 2px solid #e1e5e9;
      border-radius: 8px;
      font-size: 16px;
      transition: all 0.3s ease;
      background-color: #f8f9fa;
    }
    
    select:focus, input:focus {
      outline: none;
      border-color: #667eea;
      background-color: white;
      box-shadow: 0 0 0 3px rgba(102, 126, 234, 0.1);
    }
    
    button {
      background: linear-gradient(135deg, #667eea 0%, #764ba2 100%);
      color: white;
      border: none;
      border-radius: 8px;
      padding: 15px;
      font-size: 16px;
      font-weight: 600;
      width: 100%;
      cursor: pointer;
      transition: all 0.3s ease;
      margin-top: 10px;
    }
    
    button:hover {
      transform: translateY(-2px);
      box-shadow: 0 4px 15px rgba(102, 126, 234, 0.4);
    }
    
    .footer {
      text-align: center;
      margin-top: 30px;
      font-size: 14px;
      color: #777;
    }
    
    .info-box {
      background-color: #e3f2fd;
      border-left: 4px solid #2196f3;
      padding: 15px;
      margin-bottom: 20px;
      border-radius: 4px;
    }
    
    .info-box p {
      margin: 0;
      font-size: 14px;
      color: #1565c0;
    }
  </style>
</head>
<body>
  <div class="container">
    <h1>SilverLink IoT</h1>
    <div class="version">Setup Configuration v1.4</div>
    
    <div class="info-box">
      <p>🌐 Connected to setup network. Configure your device to connect to your WiFi network and SilverLink server.</p>
    </div>
    
    <form action="/save" method="POST">
      <div class="form-group">
        <label for="ssid">Select WiFi Network:</label>
        <select id="ssid" name="ssid" required>
)rawliteral";

  // Add SSIDs dynamically
  for (size_t i = 0; i < ssidList.size(); i++) {
    html += "<option>" + ssidList[i] + "</option>";
  }
  
  html += R"rawliteral(
        </select>
      </div>
      
      <div class="form-group">
        <label for="password">WiFi Password:</label>
        <input type="password" id="password" name="password" placeholder="Enter WiFi password" required>
      </div>
      
      <div class="form-group">
        <label for="token">SilverLink Device Token:</label>
        <input type="text" id="token" name="token" placeholder="Enter device token from dashboard" required>
      </div>
      
      <button type="submit">Connect Device</button>
    </form>
    
    <div class="footer">
      &copy; 2025 SilverLink IoT<br>
      <small>Created with ❤️ for Silver Wolf</small>
    </div>
  </div>
</body>
</html>
)rawliteral";

  return html;
}

void printSystemStatus() {
  unsigned long uptime = (millis() - systemStartTime) / 1000;
  
  Serial.println("📊 ========== SYSTEM STATUS ==========");
  Serial.printf("   🕒 Uptime: %lu seconds (%lu minutes)\n", uptime, uptime / 60);
  Serial.printf("   🧠 Free Memory: %d bytes\n", ESP.getFreeHeap());
  Serial.printf("   📡 WiFi Status: %s\n", WiFi.status() == WL_CONNECTED ? "Connected" : "Disconnected");
  
  if (WiFi.status() == WL_CONNECTED) {
    Serial.printf("   📶 Signal Strength: %d dBm\n", WiFi.RSSI());
    Serial.printf("   🌐 IP Address: %s\n", WiFi.localIP().toString().c_str());
  }
  
  Serial.printf("   🔗 WebSocket: %s\n", webSocket.isConnected() ? "Connected" : "Disconnected");
  Serial.printf("   🎯 Mode: %s\n", 
                mode == ONLINE ? "ONLINE" : 
                mode == OFFLINE_WIFI ? "OFFLINE_WIFI" : "OFFLINE_SERVER");
  Serial.printf("   📈 Runtime Config: %s\n", runtimeLoaded ? "Loaded" : "Not Loaded");
  Serial.printf("   🔢 Sensors: %d | Actuators: %d\n", sensors.size(), aktuators.size());
  Serial.printf("   ⚠️ Consecutive Errors: %d/%d\n", consecutiveErrors, maxConsecutiveErrors);
  Serial.printf("   🔄 Pending State Sync: %s\n", pendingStateSync ? "YES" : "NO");
  
  // Show actuator states
  if (aktuators.size() > 0) {
    Serial.println("   ⚡ Actuator States:");
    for (const Aktuator &akt : aktuators) {
      Serial.printf("      Pin %d (%s): %s%s\n", 
                    akt.pin, akt.function.c_str(), 
                    akt.state ? "ON" : "OFF",
                    akt.pendingStateSync ? " [PENDING SYNC]" : "");
    }
  }
  
  Serial.println("=====================================");
}

void printConnectionInfo() {
  Serial.println("🔗 ===== CONNECTION INFORMATION =====");
  Serial.printf("   Server: silverlink.eula.my.id:5050\n");
  Serial.printf("   Protocol: WebSocket (/ws)\n");
  Serial.printf("   Heartbeat Interval: %lu seconds\n", heartbeatInterval / 1000);
  Serial.printf("   Reconnect Interval: 5 seconds\n");
  Serial.printf("   Server Failures: %d\n", serverFailCount);
  Serial.printf("   WiFi Failures: %d\n", wifiFailCount);
  Serial.printf("   State Sync Enabled: YES\n");
  Serial.println("=====================================");
}

// NEW: Request state sync from server
void requestStateSync() {
  if (mode != ONLINE || !webSocket.isConnected()) {
    return;
  }
  
  DynamicJsonDocument doc(256);
  doc["event"] = "request_state_sync";
  doc["token"] = token;
  doc["timestamp"] = millis();
  
  String output;
  serializeJson(doc, output);
  
  if (webSocket.sendTXT(output)) {
    Serial.println("📡 State sync requested from server");
    lastStateSyncRequest = millis();
  } else {
    Serial.println("❌ Failed to request state sync");
  }
}

// NEW: Apply actuator state sync from server
void applyActuatorStateSync(const JsonArray& actuators) {
  Serial.println("🔄 ===== APPLYING STATE SYNC =====");
  Serial.printf("   Received state data for %d actuators\n", actuators.size());
  
  int syncedCount = 0;
  
  for (JsonObject actuatorData : actuators) {
    int pin = actuatorData["pin"];
    String function = actuatorData["function"];
    bool expectedState = actuatorData["state"];
    
    // Find matching local actuator
    for (Aktuator &akt : aktuators) {
      if (akt.pin == pin) {
        bool currentState = digitalRead(pin) == HIGH;
        
        Serial.printf("   🔧 Pin %d (%s): DB=%s, Current=%s", 
                      pin, function.c_str(),
                      expectedState ? "ON" : "OFF",
                      currentState ? "ON" : "OFF");
        
        if (currentState != expectedState) {
          // Sync with database state
          digitalWrite(pin, expectedState ? HIGH : LOW);
          akt.state = expectedState;
          saveActuatorState(pin, expectedState);
          
          Serial.printf(" → SYNCED to %s\n", expectedState ? "ON" : "OFF");
          
          // Confirm state change
          confirmActuatorState(pin, expectedState, true);
          syncedCount++;
        } else {
          Serial.printf(" → ALREADY SYNCED\n");
          // Still confirm to update database
          confirmActuatorState(pin, expectedState, true);
        }
        
        akt.pendingStateSync = false;
        break;
      }
    }
  }
  
  pendingStateSync = false;
  lastStateSync = millis();
  
  Serial.printf("✅ State sync completed: %d actuators synced\n", syncedCount);
  Serial.println("==================================");
}

// NEW: Confirm actuator state to server
void confirmActuatorState(int pin, bool state, bool success) {
  if (mode != ONLINE || !webSocket.isConnected()) {
    return;
  }
  
  DynamicJsonDocument doc(256);
  doc["event"] = "actuator_state_confirm";
  doc["token"] = token;
  doc["pin"] = pin;
  doc["state"] = state;
  doc["success"] = success;
  doc["timestamp"] = millis();
  
  String output;
  serializeJson(doc, output);
  
  if (webSocket.sendTXT(output)) {
    Serial.printf("✅ State confirmation sent: Pin %d = %s (%s)\n", 
                  pin, state ? "ON" : "OFF", success ? "SUCCESS" : "FAILED");
  } else {
    Serial.printf("❌ Failed to send state confirmation for pin %d\n", pin);
  }
}

// NEW: Check if state sync is needed
void checkPendingStateSync() {
  if (mode != ONLINE || !runtimeLoaded) {
    return;
  }
  
  unsigned long now = millis();
  
  // Request state sync if pending and enough time has passed
  if (pendingStateSync && (now - lastStateSyncRequest >= stateSyncRequestInterval)) {
    Serial.println("⏰ Requesting periodic state sync...");
    requestStateSync();
  }
  
  // Check for actuators with pending sync
  bool hasUnsynced = false;
  for (const Aktuator &akt : aktuators) {
    if (akt.pendingStateSync) {
      hasUnsynced = true;
      break;
    }
  }
  
  if (hasUnsynced && (now - lastStateSync >= stateSyncRequestInterval)) {
    Serial.println("⚠️ Some actuators still pending sync, requesting again...");
    requestStateSync();
  }
}

// NEW: Sync specific actuator with database
void syncActuatorWithDatabase(int pin, bool expectedState) {
  for (Aktuator &akt : aktuators) {
    if (akt.pin == pin) {
      bool currentState = digitalRead(pin) == HIGH;
      
      if (currentState != expectedState) {
        Serial.printf("🔄 Syncing actuator pin %d: %s → %s\n", 
                      pin, currentState ? "ON" : "OFF", expectedState ? "ON" : "OFF");
        
        digitalWrite(pin, expectedState ? HIGH : LOW);
        akt.state = expectedState;
        saveActuatorState(pin, expectedState);
        
        confirmActuatorState(pin, expectedState, true);
      }
      
      akt.pendingStateSync = false;
      break;
    }
  }
}

void webSocketEvent(WStype_t type, uint8_t * payload, size_t length) {
  // Payload size validation
  if (length > 1024) {
    Serial.printf("⚠️ Received oversized payload: %zu bytes (max 1024)\n", length);
    return;
  }
  
  switch(type) {
    case WStype_CONNECTED:
      serverFailCount = 0;
      consecutiveErrors = 0;
      mode = ONLINE;
      offlineUntil = 0;
      
      Serial.println("🔗 ===== WEBSOCKET CONNECTED =====");
      Serial.printf("   🌐 Server URL: %s\n", (char*)payload);
      Serial.printf("   📡 Connection established at: %lu ms\n", millis());
      printConnectionInfo();
      
      // Send authentication
      {
        DynamicJsonDocument doc(256);
        doc["token"] = token;
        doc["event"] = "auth";
        String out; 
        serializeJson(doc, out);
        webSocket.sendTXT(out);
        Serial.printf("🔐 Authentication request sent: %s\n", out.c_str());
      }
      break;
      
    case WStype_DISCONNECTED:
      Serial.println("⚠️ ===== WEBSOCKET DISCONNECTED =====");
      Serial.printf("   📊 Connection was active for: %lu seconds\n", 
                    (millis() - systemStartTime) / 1000);
      Serial.printf("   🔢 Server failure count: %d\n", ++serverFailCount);
      Serial.printf("   ⚠️ Consecutive errors: %d\n", ++consecutiveErrors);
      
      // Mark all actuators for re-sync on reconnect
      for (Aktuator &akt : aktuators) {
        akt.pendingStateSync = true;
      }
      pendingStateSync = true;
      
      if (serverFailCount >= 5) {
        mode = OFFLINE_SERVER;
        offlineUntil = millis() + 6L * 60 * 60 * 1000;
        Serial.println("🔴 ENTERING OFFLINE MODE (6 hours)");
        Serial.println("   Reason: Too many server connection failures");
        Serial.println("   System will restart automatically after offline period");
      } else {
        Serial.println("🔄 Will attempt reconnection in 5 seconds...");
        Serial.println("   State sync will be requested on reconnect");
      }
      break;
      
    case WStype_TEXT:
      {
        // Safe string handling
        char message[length + 1];
        memcpy(message, payload, length);
        message[length] = '\0';
        
        Serial.printf("📥 Received message: %s\n", message);
        consecutiveErrors = 0;  // Reset on successful message
        handleServerCommand(message);
      }
      break;
      
    case WStype_ERROR:
      Serial.printf("❌ WebSocket error occurred\n");
      consecutiveErrors++;
      break;
      
    case WStype_PONG:
      Serial.println("🏓 Pong received - connection healthy");
      break;
      
    default:
      Serial.printf("🔍 Unknown WebSocket event type: %d\n", type);
      break;
  }
}

void sendSensorData() {
  static unsigned long lastSend = 0;
  
  if (mode != ONLINE) {
    return;
  }
  
  if (!runtimeLoaded) {
    return;
  }
  
  if (millis() - lastSend < 5000) return;  // Send every 5 seconds
  
  lastSend = millis();
  Serial.println("📤 ===== SENDING SENSOR DATA =====");

  DynamicJsonDocument doc(1024);
  doc["event"] = "sensor_data";
  doc["token"] = token;
  JsonArray data = doc.createNestedArray("data");

  // DHT management
  DHT* dht = nullptr;
  int dhtPin = -1;
  float dhtTemp = NAN;
  float dhtHum = NAN;
  
  int validSensors = 0;

  for (Sensor &s : sensors) {
    bool validReading = false;
    
    // Soil moisture with proper calibration
    if (s.type == "soil_moisture") {
      s.value = readSoilMoisture(s.pin);
      validReading = true;
      validSensors++;
      Serial.printf("   🌱 Soil moisture pin %d: %.1f%% (calibrated)\n", s.pin, s.value);
    }
    // DHT sensors
    else if (s.type == "dht_temperature" || s.type == "dht_humidity") {
      if (dhtPin != s.pin) {
        // Cleanup previous DHT
        if (dht) {
          delete dht;
          dht = nullptr;
        }
        
        Serial.printf("   🌡️ Initializing DHT11 on pin %d...\n", s.pin);
        dht = new DHT(s.pin, DHT11);
        dht->begin();
        dhtPin = s.pin;
        delay(500); // Stabilization time
        
        dhtTemp = dht->readTemperature();
        dhtHum = dht->readHumidity();
        
        if (isnan(dhtTemp) || isnan(dhtHum)) {
          Serial.printf("   ❌ Failed to read from DHT sensor on pin %d\n", s.pin);
          delete dht;
          dht = nullptr;
          continue;
        }
        Serial.printf("   🌡️ DHT11 pin %d - Temperature: %.1f°C, Humidity: %.1f%%\n", 
                      s.pin, dhtTemp, dhtHum);
      }
      
      s.value = (s.type == "dht_temperature") ? dhtTemp : dhtHum;
      
      // Value validation for DHT
      if (s.type == "dht_temperature" && (s.value < -40 || s.value > 80)) {
        Serial.printf("   ⚠️ Invalid temperature reading: %.1f°C (out of range)\n", s.value);
        continue;
      }
      if (s.type == "dht_humidity" && (s.value < 0 || s.value > 100)) {
        Serial.printf("   ⚠️ Invalid humidity reading: %.1f%% (out of range)\n", s.value);
        continue;
      }
      
      validReading = true;
      validSensors++;
    }

    if (validReading) {
      JsonObject entry = data.createNestedObject();
      entry["type"] = s.type;
      entry["pin"] = s.pin;
      entry["value"] = s.value;
    }
  }

  // Cleanup DHT object
  if (dht) {
    delete dht;
    dht = nullptr;
  }

  if (validSensors == 0) {
    Serial.println("   ⚠️ No valid sensor readings available");
    return;
  }

  String output;
  serializeJson(doc, output);
  
  // Size validation
  if (output.length() > 800) {
    Serial.printf("   ⚠️ Message too large: %d bytes (max 800)\n", output.length());
    return;
  }
  
  if (webSocket.isConnected()) {
    bool sent = webSocket.sendTXT(output);
    if (sent) {
      Serial.printf("   ✅ Successfully sent data for %d sensors (%d bytes)\n", 
                    validSensors, output.length());
      consecutiveErrors = 0;
    } else {
      Serial.println("   ❌ Failed to send sensor data");
      consecutiveErrors++;
    }
  } else {
    Serial.println("   ❌ WebSocket not connected - cannot send data");
  }
  
  Serial.println("==================================");
}

void handleServerCommand(const char* message) {
  // Message length validation
  if (strlen(message) > 1024) {
    Serial.println("⚠️ Received command message too long, ignoring");
    return;
  }
  
  DynamicJsonDocument doc(1024);
  DeserializationError err = deserializeJson(doc, message);
  if (err) {
    Serial.printf("❌ Failed to parse command JSON: %s\n", err.c_str());
    return;
  }

  String event = doc["event"].as<String>();
  Serial.printf("🎯 Processing command: %s\n", event.c_str());
  
  if (event == "set_aktuator") {
    int pin = doc["pin"];
    bool state = doc["state"];
    bool requireConfirmation = doc["require_confirmation"] | false;
    
    // Pin validation
    if (pin < 0 || pin > 39) {
      Serial.printf("   ❌ Invalid actuator pin: %d\n", pin);
      if (requireConfirmation) {
        confirmActuatorState(pin, state, false);
      }
      return;
    }
    
    bool actuatorFound = false;
    for (Aktuator &a : aktuators) {
      if (a.pin == pin) {
        digitalWrite(pin, state ? HIGH : LOW);
        a.state = state;
        a.pendingStateSync = false;  // Clear pending sync since we got explicit command
        
        // Save state to filesystem
        saveActuatorState(pin, state);
        
        Serial.printf("   ⚡ Actuator pin %d set to %s\n", pin, state ? "ON" : "OFF");
        Serial.printf("   🔧 Function: %s\n", a.function.c_str());
        
        // Send confirmation if requested
        if (requireConfirmation) {
          confirmActuatorState(pin, state, true);
        }
        
        actuatorFound = true;
        delay(50);  // Small delay for stability
        break;
      }
    }
    
    if (!actuatorFound) {
      Serial.printf("   ⚠️ Actuator on pin %d not found in configuration\n", pin);
      if (requireConfirmation) {
        confirmActuatorState(pin, state, false);
      }
    }
  }
  else if (event == "runtime_config") {
    Serial.println("   📋 Received new runtime configuration from server");
    saveRuntimeConfig(message);
    // Request state sync after new config
    pendingStateSync = true;
  }
  else if (event == "refresh_config") {
    Serial.println("   🔄 Server requested configuration refresh");
    bool result = loadRuntimeConfig();
    Serial.printf("   %s Configuration refresh %s\n", 
                  result ? "✅" : "❌", 
                  result ? "successful" : "failed");
    if (result) {
      pendingStateSync = true;  // Request state sync after refresh
    }
  }
  else if (event == "auth_success") {
    Serial.println("   🔐 Authentication successful - device authorized");
    Serial.printf("   🎫 Connection ID: %s\n", doc["connection_id"].as<String>().c_str());
    
    // Request state sync after successful auth
    pendingStateSync = true;
    lastStateSyncRequest = millis() - stateSyncRequestInterval + 2000;  // Request sync in 2 seconds
    Serial.println("   🔄 State sync will be requested in 2 seconds");
  }
  else if (event == "actuator_state_sync") {
    Serial.println("   🔄 Received actuator state sync from server");
    JsonArray actuators = doc["actuators"];
    if (actuators) {
      applyActuatorStateSync(actuators);
    } else {
      Serial.println("   ⚠️ No actuator data in state sync message");
    }
  }
  else if (event == "data_ack") {
    int processed = doc["processed"] | 0;
    Serial.printf("   ✅ Server acknowledged: %d sensor readings processed\n", processed);
  }
  else if (event == "heartbeat_ack") {
    Serial.println("   💓 Heartbeat acknowledged by server");
  }
  else if (event == "error") {
    String errorMsg = doc["message"].as<String>();
    Serial.printf("   ❌ Server error: %s\n", errorMsg.c_str());
  }
  else {
    Serial.printf("   ❓ Unknown command event: %s\n", event.c_str());
  }
}

void checkOfflineMode() {
  if (mode == ONLINE) return;

  unsigned long now = millis();
  
  // Display countdown
  if (offlineUntil > now) {
    static unsigned long lastCountdown = 0;
    if (now - lastCountdown >= 60000) {  // Log every minute
      unsigned long remaining = (offlineUntil - now) / 1000;
      Serial.printf("⏰ OFFLINE MODE - Restart in: %lu hours %lu minutes\n", 
                    remaining / 3600, (remaining % 3600) / 60);
      lastCountdown = now;
    }
  }
  
  if (now >= offlineUntil && offlineUntil > 0) {
    Serial.println("🔁 ===== OFFLINE PERIOD ENDED =====");
    Serial.println("   Restarting system to restore connection...");
    Serial.println("   Device will attempt to reconnect to WiFi and server");
    delay(2000);
    ESP.restart();
  }

  // Enhanced offline sensor logic with state preservation
  if (now - lastMoistureCheck >= moistureInterval) {
    Serial.println("🔄 ===== OFFLINE SENSOR CHECK =====");
    Serial.printf("   Mode: %s\n", mode == OFFLINE_WIFI ? "OFFLINE_WIFI" : "OFFLINE_SERVER");
    
    bool emergencyAction = false;
    
    for (Sensor &s : sensors) {
      if (s.type == "soil_moisture" && s.threshold > 0) {
        float value = readSoilMoisture(s.pin);
        s.value = value;
        
        Serial.printf("   🌱 Soil moisture check - Pin %d: %.1f%% (threshold: %d%%)\n", 
                      s.pin, value, s.threshold);
        
        if (value < s.threshold) {
          Serial.printf("   🚨 EMERGENCY: Soil moisture below threshold!\n");
          Serial.printf("      Current: %.1f%% | Required: %d%%\n", value, s.threshold);
          
          for (Aktuator &a : aktuators) {
            if (a.function == "pump") {
              Serial.println("   🚿 Activating emergency watering system...");
              Serial.printf("      Pump pin %d: ON for 10 seconds\n", a.pin);
              
              digitalWrite(a.pin, HIGH);
              a.state = true;
              saveActuatorState(a.pin, true);  // Save emergency state
              
              delay(10000);
              
              digitalWrite(a.pin, LOW);
              a.state = false;
              saveActuatorState(a.pin, false);  // Save off state
              
              emergencyAction = true;
              
              Serial.println("   🚿 Emergency watering completed");
              Serial.printf("      Next check in %lu minutes\n", moistureInterval / 60000);
              break;
            }
          }
        } else {
          Serial.printf("   ✅ Soil moisture OK - no action needed\n");
        }
      } 
      else if (s.type == "dht_temperature" || s.type == "dht_humidity") {
        DHT dht(s.pin, DHT11);
        dht.begin();
        delay(500);
        
        float temp = dht.readTemperature();
        float hum = dht.readHumidity();
        
        if (!isnan(temp) && !isnan(hum)) {
          Serial.printf("   🌡️ Environmental monitoring - Pin %d: %.1f°C, %.1f%%RH\n", 
                        s.pin, temp, hum);
        } else {
          Serial.printf("   ❌ Failed to read DHT sensor on pin %d\n", s.pin);
        }
      }
    }
    
    if (!emergencyAction) {
      Serial.println("   ✅ All sensors within normal parameters");
    }
    
    Serial.println("==================================");
    lastMoistureCheck = now;
  }
}

// Enhanced system monitoring
void basicSystemMonitoring() {
  unsigned long now = millis();
  
  // Memory monitoring
  if (now - lastMemoryCheck >= 60000) {  // Every minute
    int freeHeap = ESP.getFreeHeap();
    
    if (freeHeap < 20000) {  // Less than 20KB
      Serial.printf("⚠️ LOW MEMORY WARNING: %d bytes free\n", freeHeap);
      
      if (freeHeap < 10000) {  // Critical: Less than 10KB
        Serial.println("🔴 CRITICAL MEMORY SHORTAGE");
        Serial.printf("   Free heap: %d bytes\n", freeHeap);
        Serial.println("   System will restart to prevent crash");
        delay(2000);
        ESP.restart();
      }
    }
    
    lastMemoryCheck = now;
  }
  
  // Periodic status reporting
  if (now - lastStatusReport >= 120000) {  // Every 2 minutes
    printSystemStatus();
    
    // Check for too many consecutive errors
    if (consecutiveErrors >= maxConsecutiveErrors) {
      Serial.printf("🔴 CRITICAL: Too many consecutive errors (%d/%d)\n", 
                    consecutiveErrors, maxConsecutiveErrors);
      Serial.println("   System stability compromised - restarting for recovery");
      delay(2000);
      ESP.restart();
    }
    
    // Optional: Power monitoring
    if (powerMonitoringEnabled) {
      int raw = analogRead(VOLTAGE_PIN);
      float voltage = (raw * 3.3 * 2.0) / 4095.0;  // Assuming voltage divider
      
      Serial.printf("🔋 Power monitoring: %.2fV (ADC: %d)\n", voltage, raw);
      
      if (voltage < 4.5) {
        Serial.printf("⚠️ LOW VOLTAGE WARNING: %.2fV\n", voltage);
        Serial.println("   Check power supply stability");
      }
    }
    
    lastStatusReport = now;
  }
  
  // Check for pending state sync
  checkPendingStateSync();
}

void monitorConnection() {
  unsigned long now = millis();
  
  // Enhanced WiFi monitoring
  if (WiFi.status() != WL_CONNECTED) {
    if (mode == ONLINE) {
      Serial.println("⚠️ ===== WIFI CONNECTION LOST =====");
      Serial.printf("   Previous SSID: %s\n", WiFi.SSID().c_str());
      Serial.printf("   WiFi failure count: %d\n", ++wifiFailCount);
      
      mode = OFFLINE_WIFI;
      offlineUntil = millis() + 30L * 60 * 1000;  // 30 minutes offline
      
      // Mark all actuators for re-sync when reconnected
      for (Aktuator &akt : aktuators) {
        akt.pendingStateSync = true;
      }
      pendingStateSync = true;
      
      Serial.println("   🔄 Entering OFFLINE_WIFI mode for 30 minutes");
      Serial.println("   System will attempt automatic recovery");
      Serial.println("   State sync will be requested on reconnect");
    }
    return;
  }
  
  // Reset WiFi fail count on successful connection
  if (wifiFailCount > 0 && WiFi.status() == WL_CONNECTED) {
    Serial.println("✅ WiFi connection recovered");
    Serial.printf("   Connected to: %s\n", WiFi.SSID().c_str());
    Serial.printf("   Signal strength: %d dBm\n", WiFi.RSSI());
    wifiFailCount = 0;
  }
  
  // Send heartbeat every 30 seconds if online
  if (mode == ONLINE && now - lastHeartbeat >= heartbeatInterval) {
    if (webSocket.isConnected()) {
      DynamicJsonDocument doc(512);
      doc["event"] = "heartbeat";
      doc["token"] = token;
      doc["uptime"] = (now - systemStartTime) / 1000;
      doc["free_memory"] = ESP.getFreeHeap();
      doc["wifi_rssi"] = WiFi.RSSI();
      doc["sensor_count"] = sensors.size();
      doc["actuator_count"] = aktuators.size();
      doc["pending_state_sync"] = pendingStateSync;
      
      // Add actuator states to heartbeat
      JsonArray actuatorStates = doc.createNestedArray("actuator_states");
      for (const Aktuator &akt : aktuators) {
        JsonObject state = actuatorStates.createNestedObject();
        state["pin"] = akt.pin;
        state["function"] = akt.function;
        state["state"] = akt.state;
        state["pending_sync"] = akt.pendingStateSync;
      }
      
      String output;
      serializeJson(doc, output);
      
      bool sent = webSocket.sendTXT(output);
      if (sent) {
        Serial.printf("💓 Heartbeat sent - Uptime: %lu seconds\n", (now - systemStartTime) / 1000);
        consecutiveErrors = 0;
      } else {
        Serial.println("❌ Heartbeat transmission failed");
        consecutiveErrors++;
      }
    } else {
      Serial.println("⚠️ WebSocket disconnected during ONLINE mode");
      mode = OFFLINE_SERVER;
      serverFailCount++;
      
      // Mark for state sync on reconnect
      for (Aktuator &akt : aktuators) {
        akt.pendingStateSync = true;
      }
      pendingStateSync = true;
    }
    lastHeartbeat = now;
  }
}

void setup() {
  // Initialize system timing
  systemStartTime = millis();
  
  // Configure hostname and serial
  WiFi.setHostname("SilverLink-IoT");
  Serial.begin(115200);
  
  // Wait for serial to be ready
  delay(1000);
  
  // Print startup banner
  Serial.println("\n==================================================");
  Serial.println("     🌱 SilverLink IoT System Starting 🌱");
  Serial.println("        ENHANCED VERSION v1.4 (State Sync)");
  Serial.println("     Created with ❤️ for Silver Wolf");
  Serial.println("==================================================");
  
  Serial.printf("🚀 System initialization started at: %lu ms\n", millis());
  Serial.printf("🧠 Initial free memory: %d bytes\n", ESP.getFreeHeap());
  Serial.printf("📶 WiFi hostname: %s\n", WiFi.getHostname());
  
  // Hardware information
  Serial.println("\n📋 Hardware Configuration:");
  Serial.printf("   🔌 Reset button: Pin %d\n", RESET_PIN);
  Serial.printf("   🌱 Soil sensor: Pin %d\n", SENSOR_PIN);
  Serial.printf("   📊 Voltage monitor: Pin %d\n", VOLTAGE_PIN);
  
  // Soil moisture calibration info
  Serial.println("\n🌱 Soil Moisture Calibration:");
  Serial.printf("   🏜️ Dry soil (max ADC): %d\n", minValue);
  Serial.printf("   💧 Wet soil (min ADC): %d\n", maxValue);
  Serial.printf("   📏 Measurement range: 0-100%%\n");
  
  // State sync information
  Serial.println("\n🔄 State Sync Features:");
  Serial.println("   ✅ Actuator state persistence");
  Serial.println("   ✅ Database state synchronization");
  Serial.println("   ✅ Reconnect state recovery");
  Serial.println("   ✅ Offline state preservation");
  
  // Check if voltage monitoring is possible
  pinMode(VOLTAGE_PIN, INPUT);
  int testRead = analogRead(VOLTAGE_PIN);
  if (testRead > 0 && testRead < 4095) {
    powerMonitoringEnabled = true;
    Serial.println("📊 Power monitoring: ENABLED");
  } else {
    Serial.println("📊 Power monitoring: DISABLED (no voltage divider detected)");
  }

  // Check reset button
  checkResetButton();
  
  // Initialize filesystem
  if (!LittleFS.begin()) {
    Serial.println("❌ CRITICAL: LittleFS mount failed");
    Serial.println("   File system required for configuration storage");
    Serial.println("   System will restart in 5 seconds...");
    delay(5000);
    ESP.restart();
  }
  Serial.println("✅ LittleFS mounted successfully");
  
  // Load configuration and start normal operation
  if (loadConfig()) {
    Serial.println("\n🔗 ===== CONNECTING TO WIFI =====");
    Serial.printf("   Target SSID: %s\n", ssid.c_str());
    Serial.printf("   Connection timeout: 20 seconds\n");
    
    WiFi.begin(ssid.c_str(), password.c_str());
    unsigned long startAttemptTime = millis();
    
    // Connection attempt with progress indication
    int dots = 0;
    while (WiFi.status() != WL_CONNECTED && millis() - startAttemptTime < 20000) {
      delay(500);
      Serial.print(".");
      if (++dots % 10 == 0) {
        Serial.printf(" %lu ms\n   ", millis() - startAttemptTime);
      }
    }
    Serial.println();
    
    if (WiFi.status() == WL_CONNECTED) {
      Serial.println("✅ ===== WIFI CONNECTED SUCCESSFULLY =====");
      Serial.printf("   📡 SSID: %s\n", WiFi.SSID().c_str());
      Serial.printf("   🌐 IP Address: %s\n", WiFi.localIP().toString().c_str());
      Serial.printf("   📶 Signal Strength: %d dBm\n", WiFi.RSSI());
      Serial.printf("   🔗 Gateway: %s\n", WiFi.gatewayIP().toString().c_str());
      Serial.printf("   🏷️ MAC Address: %s\n", WiFi.macAddress().c_str());
      
      // Load runtime configuration
      bool runtimeSuccess = loadRuntimeConfig();
      if (!runtimeSuccess) {
        Serial.println("⚠️ No runtime configuration found");
        Serial.println("   Device will wait for configuration from server");
      } else {
        Serial.println("✅ Runtime configuration loaded with state sync enabled");
      }
      
      // Start WebSocket connection
      Serial.println("\n🌐 ===== STARTING WEBSOCKET =====");
      Serial.printf("   🎯 Server: silverlink.eula.my.id:5050\n");
      Serial.printf("   📡 Protocol: WebSocket (/ws)\n");
      Serial.printf("   🔄 Reconnect interval: 5 seconds\n");
      Serial.printf("   💓 Heartbeat interval: %lu seconds\n", heartbeatInterval / 1000);
      Serial.printf("   🔄 State sync: ENABLED\n");
      
      webSocket.begin("silverlink.eula.my.id", 5050, "/ws");
      webSocket.onEvent(webSocketEvent);
      webSocket.setReconnectInterval(5000);
      webSocket.enableHeartbeat(15000, 3000, 2);  // Conservative heartbeat
      
      Serial.println("✅ WebSocket client initialized");
      Serial.println("   Waiting for server connection...");
      Serial.println("   State sync will be requested after authentication");
      
      return;
    } else {
      Serial.println("❌ ===== WIFI CONNECTION FAILED =====");
      Serial.printf("   Attempted SSID: %s\n", ssid.c_str());
      Serial.printf("   Connection timeout after 20 seconds\n");
      Serial.printf("   WiFi status code: %d\n", WiFi.status());
      
      mode = OFFLINE_WIFI;
      offlineUntil = millis() + 30L * 60 * 1000;
      wifiFailCount++;
      
      Serial.println("🔴 Entering OFFLINE_WIFI mode for 30 minutes");
      Serial.println("   System will restart automatically for recovery");
    }
  } else {
    // Access Point Mode for initial setup
    Serial.println("\n🔧 ===== ENTERING SETUP MODE =====");
    Serial.println("   Reason: No valid configuration found");
    Serial.println("   Starting Access Point for device setup");
    
    scanNetworks();
    
    WiFi.mode(WIFI_AP);
    bool apStarted = WiFi.softAP("SilverLink-Setup");
    
    if (apStarted) {
      Serial.println("✅ ===== ACCESS POINT STARTED =====");
      Serial.printf("   📡 SSID: SilverLink-Setup\n");
      Serial.printf("   🔐 Password: silverlink123\n");
      Serial.printf("   🌐 IP Address: %s\n", WiFi.softAPIP().toString().c_str());
      Serial.printf("   👥 Max clients: 4\n");
      
      Serial.println("\n📱 Setup Instructions:");
      Serial.println("   1. Connect to 'SilverLink-Setup' WiFi network");
      Serial.println("   2. Enter password: silverlink123");
      Serial.println("   3. Open browser and go to: 192.168.4.1");
      Serial.println("   4. Configure your WiFi and device token");
    } else {
      Serial.println("❌ Failed to start Access Point");
      Serial.println("   System will restart in 5 seconds...");
      delay(5000);
      ESP.restart();
    }
    
    // Web server setup for configuration
    server.on("/", HTTP_GET, [](AsyncWebServerRequest *req) {
      Serial.printf("📱 Setup page requested from: %s\n", req->client()->remoteIP().toString().c_str());
      req->send(200, "text/html", htmlForm());
    });
    
    server.on("/save", HTTP_POST, [](AsyncWebServerRequest *req) {
      Serial.println("💾 Configuration submission received");
      
      if (req->hasParam("ssid", true) && req->hasParam("password", true) && req->hasParam("token", true)) {
        String newSSID = req->getParam("ssid", true)->value();
        String newPassword = req->getParam("password", true)->value();
        String newToken = req->getParam("token", true)->value();
        
        Serial.printf("   📝 New SSID: %s\n", newSSID.c_str());
        Serial.printf("   🎫 New Token: %s...\n", newToken.substring(0, 8).c_str());
        
        saveConfig(newSSID, newPassword, newToken);
        
        req->send(200, "text/html", 
          "<!DOCTYPE html><html><head><meta charset='UTF-8'></head>"
          "<body style='font-family:Arial;text-align:center;padding:50px;background:#f0f8ff;'>"
          "<div style='background:white;padding:40px;border-radius:10px;box-shadow:0 4px 15px rgba(0,0,0,0.1);max-width:400px;margin:auto;'>"
          "<h2 style='color:#4CAF50;margin-bottom:20px;'>✅ Configuration Saved!</h2>"
          "<p style='margin-bottom:15px;'>Device will restart and connect to your WiFi network.</p>"
          "<p style='color:#666;'>State sync will be enabled automatically.</p>"
          "<div style='margin-top:30px;padding:15px;background:#e8f5e8;border-radius:5px;'>"
          "<small>If connection fails, the setup network will automatically restart.</small>"
          "</div></div></body></html>");
        
        Serial.println("✅ Configuration saved successfully");
        Serial.println("🔄 Restarting system in 3 seconds...");
        
        delay(3000);
        ESP.restart();
      } else {
        Serial.println("❌ Incomplete configuration data received");
        req->send(400, "text/html", 
          "<!DOCTYPE html><html><head><meta charset='UTF-8'></head>"
          "<body style='font-family:Arial;text-align:center;padding:50px;'>"
          "<div style='background:white;padding:40px;border-radius:10px;max-width:400px;margin:auto;'>"
          "<h2 style='color:#f44336;'>❌ Configuration Error</h2>"
          "<p>Missing required information. Please fill all fields.</p>"
          "<a href='/' style='display:inline-block;margin-top:20px;padding:10px 20px;background:#2196f3;color:white;text-decoration:none;border-radius:5px;'>Try Again</a>"
          "</div></body></html>");
      }
    });
    
    server.begin();
    Serial.println("🌐 Web server started successfully");
    Serial.println("   Ready for configuration at: http://192.168.4.1");
  }
  
  Serial.println("\n🎯 System initialization completed");
  Serial.printf("⏱️ Total startup time: %lu ms\n", millis() - systemStartTime);
  Serial.println("==================================================");
}

void loop() {
  // Main system monitoring
  basicSystemMonitoring();
  
  // WebSocket handling
  webSocket.loop();
  
  // Connection monitoring
  monitorConnection();
  
  // Offline mode checking
  checkOfflineMode();
  
  // Sensor data transmission
  if (runtimeLoaded && mode == ONLINE) {
    sendSensorData();
  }
  
  // System stability
  yield();
  delay(50);
}
