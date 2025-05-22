// SilverLink ESP32 Full Integration
// WebSocket + Offline Mode + DHT11 + Soil Moisture + Command+Data Exchange + Dynamic Config

#include <WiFi.h>
#include <ESPAsyncWebServer.h>
#include <LittleFS.h>
#include <ArduinoJson.h>
#include <WebSocketsClient.h>
#include <DHT.h>
#include <Wire.h>
#include <LiquidCrystal_I2C.h>

#define DHTTYPE DHT11
#define RESET_PIN 4


WebSocketsClient webSocket;
AsyncWebServer server(80);
std::vector<String> ssidList;
String ssid, password, token;
LiquidCrystal_I2C lcd(0x27, 16, 2);

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

void checkResetButton() {
  pinMode(RESET_PIN, INPUT_PULLUP);
  delay(300);
  if (digitalRead(RESET_PIN) == LOW) {
    Serial.println("🧨 RESET tombol ditekan, hapus config.json...");
    LittleFS.begin();
    LittleFS.remove("/config.json");
    LittleFS.remove("/config_runtime.json");
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
  serializeJson(doc, configFile);
  configFile.close();
}

bool loadConfig() {
  if (!LittleFS.begin()) return false;
  File configFile = LittleFS.open("/config.json", "r");
  if (!configFile) return false;
  DynamicJsonDocument doc(512);
  deserializeJson(doc, configFile);
  ssid = doc["ssid"].as<String>();
  password = doc["password"].as<String>();
  token = doc["token"].as<String>();
  return true;
}

bool loadRuntimeConfig() {
  if (!LittleFS.exists("/config_runtime.json")) return false;
  File file = LittleFS.open("/config_runtime.json", "r");
  if (!file || file.size() == 0) {
    Serial.println("❌ config_runtime.json kosong atau tidak valid");
    return false;
  }

  DynamicJsonDocument doc(2048);
  deserializeJson(doc, file);
  sensors.clear(); aktuators.clear();
  for (JsonObject s : doc["sensors"].as<JsonArray>()) {
  String type = s["type"].as<String>();
  int pin = s["pin"];
  int threshold = s["threshold"] | -1;

  if (type == "dht11") {
    Sensor tempSensor;
    tempSensor.type = "dht_temperature";
    tempSensor.pin = pin;
    sensors.push_back(tempSensor);

    Sensor humSensor;
    humSensor.type = "dht_humidity";
    humSensor.pin = pin;
    sensors.push_back(humSensor);
  } else {
    Sensor sensor;
    sensor.type = type;
    sensor.pin = pin;
    sensor.threshold = threshold;
    sensors.push_back(sensor);
  }

  pinMode(pin, INPUT);
}

  for (JsonObject a : doc["aktuators"].as<JsonArray>()) {
    Aktuator akt;
    akt.function = a["function"].as<String>();
    akt.pin = a["pin"];
    aktuators.push_back(akt);
    pinMode(akt.pin, OUTPUT); digitalWrite(akt.pin, LOW);
  }
  runtimeLoaded = true;
  return true;
}

void saveRuntimeConfig(const String& jsonString) {
  File f = LittleFS.open("/config_runtime.json", "w");
  f.print(jsonString);
  f.close();
  loadRuntimeConfig();
}

void scanNetworks() {
  WiFi.mode(WIFI_STA);
  WiFi.disconnect();
  delay(100);
  int n = WiFi.scanNetworks();
  for (int i = 0; i < n; i++) {
    String s = WiFi.SSID(i);
    if (s.length() > 0) ssidList.push_back(s);
  }
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
      font-family: 'Arial', sans-serif;
    }
    
    body {
      background-color: #f5f5f5;
      display: flex;
      justify-content: center;
      align-items: center;
      min-height: 100vh;
      padding: 20px;
    }
    
    .container {
      background-color: white;
      border-radius: 10px;
      box-shadow: 0 4px 15px rgba(0, 0, 0, 0.1);
      width: 100%;
      max-width: 450px;
      padding: 30px;
    }
    
    .logo-container {
      text-align: center;
      margin-bottom: 25px;
    }
    
    .logo {
      max-width: 180px;
      height: auto;
    }
    
    h1 {
      color: #333;
      font-size: 24px;
      text-align: center;
      margin-bottom: 20px;
    }
    
    .form-group {
      margin-bottom: 20px;
    }
    
    label {
      display: block;
      font-weight: bold;
      margin-bottom: 8px;
      color: #555;
    }
    
    select, input[type="text"], input[type="password"] {
      width: 100%;
      padding: 12px;
      border: 1px solid #ddd;
      border-radius: 6px;
      font-size: 16px;
      transition: border-color 0.3s;
    }
    
    select:focus, input:focus {
      outline: none;
      border-color: #0066cc;
      box-shadow: 0 0 0 2px rgba(0, 102, 204, 0.2);
    }
    
    button {
      background-color: #0066cc;
      color: white;
      border: none;
      border-radius: 6px;
      padding: 14px;
      font-size: 16px;
      font-weight: bold;
      width: 100%;
      cursor: pointer;
      transition: background-color 0.3s;
      margin-top: 10px;
    }
    
    button:hover {
      background-color: #0052a3;
    }
    
    .footer {
      text-align: center;
      margin-top: 25px;
      font-size: 14px;
      color: #777;
    }
  </style>
</head>
<body>
  <div class="container">
    <div class="logo-container">
      <img class="logo" src="data:image/png;base64,iVBORw0KGgoAAAANSUhEUgAAAPAAAABGCAQAAABYz9FxAAAAIGNIUk0AAHomAACAhAAA+gAAAIDoAAB1MAAA6mAAADqYAAAXcJy6UTwAAAACYktHRAD/h4/MvwAAAAlwSFlzAAAOwwAADsMBx2+oZAAAAAd0SU1FB+kFExAEMsdOoB8AABEFSURBVHja7dx5fJXFuQfw78kelgTCEmUpICCbIgLugBZFr2C5KlCXolaRulTs7fWj3l63aq211mpr1SoW97XWrVapCihYxQuCIChLWQVZZTEEgUDy3j/ycu45yUlyEhKCt/nNP+fMzPvMM/O8M/PM8zzz0oAGNOBfEREp9c1CA6pGao2fzDLUNtvquwMNqBw1n4U77XazDvXdgQZUjn1ZZt9WYIJ+9d2FBtQd2plhsVPqm40G1B3OVmC5ofXNRgPqCul+L7DS8PpmpAF1hc7mCKwysr4ZaUBd4TzbBNYZsw+HrgYcwMj2uEBgi5/IqG9mGlAX6GOpQKDQ9Q0iPrBQO4vqOiVOlibDcQIz7KnvbjWgtpHjFYFAYKdbZNU3Ow2ofRzri1DEO/xCdn2z04BS1J7eu1qKk6UgzXHSTbe7vjvXgNpFSxPDORwocpcm9c1QA2obg6yJini338mpb4Zi0FSj+mahjtBMWsWFtWua+EITg0RAiqM0M93O/drZTPl2KY7LS3GI842z3Or9yksySJWPoho+na2fqwzzgR37i+GDTI3O4UCxCVrWQSvp8hykVbk52dwDPvdbTaM5eU73kCUCTxyAil+6a8zzoo7VfjJVBxd73UbfOLeyimnJUkwS69zu2ahQU1ws27W+rMUWvmOYAbpq5RvLzTPVe9FV4jhjpWvnr6aCHm7T1yFg/v57y7V3jrToOhKRao2/2F6uXgc/0d5hprq/mi2McKXD5WGzJfutXyDVr2PmcCDwsu/UEu1055vsUVe50njrBAIb9I6WH22twFKHhf/zHau/NwQC1+3HMWjnZu8oCvu/3lPGJNQADvKRQKGzq91CL/2N9KXAJkfvx56B9j4sI+KJOtcC3XQ/s8IPpYMUJ/lUoEDfaI00w91lSJk4lSv3u4Ahz6vhNvWTUCtJhKP9ygU1NAyle7N+BMy/21JGxG/rus9UL7LDk3Fq4TBbbXNkFc9dWi8C5j8EAhvrLKgpzZ+rFnDdhL6+6akyOUOM132faLZxtSyz4zTkSd5OogeRKmvUDQrAzjrc+5PoWd0IeLd7zS6Td5KH9dwHmv31pIxOvstkJfUmwKpwQPBV21r0Xiz3G3/SOC5vkEdcbl4NKbaThVM9aE1M7hzvKIz5n66nlj6o8PSdIis68IGdSkBmmXEoijGzttZVa0WWWV6OaqaesqPes2aO0MxCi6rRq4hDdPWxr+LyOupspq/D/xkO1ck35thUCaU0mWVyiuyuOwHzqsHGlsk73niXm1sjetsUS9XPdW6IOXB87ILoEtjSCUY6xVpDrauASku/0EUxIta6wSrwAyNkhsKOKPF7b4IWRhugsZY6KzLZb2J4b2ugMw02w/ftwYlucIxsbxiVpLu0iX7ONEyOs6ICztHPcENl+l44FQ5zjaFyFJtmnKUVUjvWtdGTfgoK3WdKLcmyAnQzr4yqFQjM1L9G1HpZKRDY5SH55UpTXOAtmwQC8x0cVzY2RsnK8F2vhZzcHX3nOxgZ6ryBta7VBnTxpDv0kudgoy0WmBtq7Fl+aprCUH1sjO9ZFT4/KdTyGSMQWFXBxnSSF0P/21bHh3lDvRTSWR8e/oaaZZHZtgsEfhVHIc2LMUpWC6d6OuRhuXsMrxMTUxmMCRmLT584tga0UtxoT0jhzXKaabphTnaZTQLzKhEw5JssELgprlZXywQ2GBH+b+evbohZ4U6zXuB1OWjkbIP9XJHA27L0M90fXeJFgXeSFPAJznCmz+MEfJIhRlkjsE5vDPOeK7SR51q7BN6Lc+DEC7j0+UKFHthHdbYaaOypBAIOfObEGlBr6m7fhBSWuCiB6bGluUkImOEKBd7XLCYv1yyB28IdOtOD/i43pjzFwwI7nBXN6WGtwN918LzLpaCdz0xJUsClNJ+NE3Bpy+8IrNPLUd50WpjbzlKB+ZrH1Cwr4AwPWOrC+KCpur0huN3dCQ1pPY33b9Wmts2Nrgl3zc4edJ9OZWpEwn20Kkw1E731icnLk2ehxwTgVOd5KarmQIlJimU5NUZJK0aKS2zyqBKsMbta7puUhPzuQokubvWSt8K8HVU6JBq5VncXeTK+Zt0pWaWY6x73ltPvONTDrvZaNant9Edz3ORUqRq5VB8/M6kGXH1topPkON170bwuWvm15SDbJXKdqGPMUScIL9p10yS8U1la1lOeH4WDWuJ9nZN8ySpHI/9theei/6s6cjV3kzauLK/B74uAU0Q0lydDB1km+iZhrWecktDW+h0PyPJCtVud7gJXuEo++nvSTZ6oQZDfO67R2hD3RvXtQbZ4JfzdwwAbfGZr3MCu9r6IDWUiVdp6x6fRf49LKeOsrBlydXdjBSNaFsU6uNVmV9tQvrB6As6WKltHTWXrr50M3RzkC+97xa4Knilwp6O0T1DS1n3SPRMuisljk1/6yG2Ow8Husd3z1R7ABT4yXA/HehW0NtRbFoSlR2ppgQmJBqwcikyMecFq6tktj498mGTNAUY53OjE3FYu4IhmmkjVwSHStddbtqYOkSVVYwTmedTLFlT61s70e3cmbKm1e6R6stoiDkyy1F1GiMgxzjuVmgASYac3nCHLMK8rxgBtXR/tRSekJxkBss6caradHCYlcC8mQorTHS/iFosTmVjihz1Vmgz5cjR2mI6y9ZYvVUs55T7ZUGS2p7weKj2V4zEnOz1hSSu/leaxKvetDB1siFN6lrtSqrPQU3cfVHsA37VSJydqb4UM55hjerQsF7laWJEEnZVJzfPqYls5U29FKDZexKn6ucOPyr/oaVLky9dOtrYOk6OZQ+RI07SS2b3Nh57zVoX2orLY7Nf6lDm87EULd4mEWmjFaOMxd4fL6V5sdJcBWsmoUezXcu/rpJOBVjjMcW6OmTE70EJvs5KgszZ0KiSPTOf5zMxK6xQkNXEgYqXrfEd3Z1rklrKxrCnSjPCClzzjHmOMMkRnrTSvULxb/MX5zvFE0uKFqR6sUIR57nZZFceLNO3DyIxYfGoudlR7gYY93lAkzRnSDbfJ2zFlywVSDE+4SGeWeVE3Vltr7uzGKj99sdXWpOmlmOsGm6S4ygXlC4s85ucWSE/C+7HGBCNd6G9xi2Vy+JNpFZbl+pVxUQNBIgQ4qlyN3TZgYSUW2srwoSU43kmGeaWMC2MrCbeVND+OnuBLNYfqq1UDZVfpkPimGieDCF5xlz2aus2Q+MIUbPecEcZXsamvdKdhrjClhv7Nde6I85nEI9ftVdxNLDG4nGs/S1vFJkRncERE8m66L72DfDdr6uW4kk9NQ1O3xJlCyHWjI6PGh9KWqhvKd7CLrakySi2tAhNUor4FCDzoSaUnkzgr4V4yS11tbKX64JeWKNynQ/wUf6pEX27sVtdXErxSorXr5MXlHeNwE/w5hkZT5JYJuW8FCRwUgYkKpRtgavSAVIrtfmcdDveoEaHBMtcQT+nv5uhcb6YJulaobeclyMt1o2PMD1fAdC2QrUVMjUwt0SrOjLp3tWgcZzzN0AppoVO20C2moruHogbOcuji4dBHkijtscITRmpbY1d2J/MrpF56ce0XZXzI/8fZCoE9Ho05UR/p7x6IDk5EY9cpEtjtJk1CHlP18JFAYJ6+5fb5FqYLbDW4XHsRVygIr8S+7xETfGiTF6PhgxHN3Rfeih4jR2qZMUnRwbuhLbqX0sjKHAO9YLfANYjIcKaNAoFn5UsJ80YrCPvQNGYWt7FYoMj1GoWrVIazQ+/ZI1qFNY+yWCCwxnXaJ9aiMp1rThVCmOc+Q8N5UR0c5qFysVpl0y73aZ3g2TYmhq/eXLc6z2i3e9YPowtkmitNti2kUmiKq6XqZrzPo7SXeLzMksutAm8mvGST5tLw1nNpWuG/oqb+xv7b+3aGJdtM87jLYkTcxUM+URyWPuMP7jfBVJuVXs47FWd4LYwLDew2053yjPSar8K87d5zSzTC+1a7Qmpv+zHO9Ir10adn+GU440eGQt/jc/drk3hN7+J651Zxs2i7+d7ytnlJHhIOMtoVCfTg8pjrkgRnwIiWeuunl/YybbLMx96N2clSnaC9PeFSFpFqralaGyQ1ar5IwQdljh/He9ot5SLI9qKXkY7R1Fc+9LqF0fxM35WnONpaihSrTYtuQK0NFgnbjUgJRzlQolhXZzvfPx2ud5Rf0hSarLvucX3YaoqdiBjkYCWISPOl9/TRM9o+aQpM8Q3SDNRagFS7TKlIF890rrlVzLVAYJNJrtO3CptPtlH+Eb7PladirziiUloR2XJr8f5xum5VcJ8pp1L9vvot9jkw7k9383TU+1pZKvGll4zVLeEwRPT3RHTprDxt94c4daMBdYxGLrEgKdEEdlvqSefrEKfIdHRr9Fp4VWmNMQfGm/2vhd6esSNJEQV2mut3hspHjtFmKEnyyZkV2KobUOdo4hILkxZxIFDgI/d6NakFvnTnfUmX+u7mvzaO8Gw15nH10jZ37I/4vwZUjibGWlQH4l3p0lrVURuwDzjCc9HDfe2kGQbWd6caEIvGLg2NYfue9niqFm4cNqDWcYTna2Eeb/XLOMN5Aw4gNDbWP/dJvMudU+cBu6TLr8B1sRdZUS7S9ImLCzk4LsQ8Fk3kaR4XEBEp4+Zs4hiHljECp8mXl9AwnF5BqEOu45My7VaBmnxlZ7fZpmmuSw2FNNU4E2slergyZLrauQb5PC44IX6Iz5VqLch2kWW2REt+IMuyBFQjznalUbpbqCCk1sYF5tsdpZ5ntBO8GxOImOunznKif9ocw0Hpr+/LTugdbm2s7jH27Rqipp9RWm+i1Xok9HlWhj2edpX5+8Z0Uujje653iM6aOMUIGxQY7RxBTCjd6TZZjhw/0MhHdhlliMEW6mOLrwy1tFy86GJbtXS7LU7xI/lWutRoLRQrcLVB1vvCGoebGhOTMdyhbjbDRke7QmcLneAUI23RwbV6yLPOuQ4zTFtrjDXc1xbapOu+C7jmV1e2e8Qoz1cYD50Im91o3H76WlW+FTaaK9/pcq0z3HGO97ELY9yRe1eRXRbpr6U051mpuQGKNTHckQkCZ4oU2GGb1i4y1RBHWmC9WVbb4X80cTFKyoillxm2WC3dGLP08V0DtbHcSF/6wkJzfS3NCHN1cJZO5rlco3JUaoR9u5s0x1hXJx0RtdjlflPtGMSaYpdGyFGkyKdmCORr61Bfxy3TxWHdD6xXLLDDdPNlynChsd4NXXTxKP3fwk4TrdXMHGtMssIJThPRTKmrL3YLKghNOU2lesvnWin2mekCqyw3x8e+scgSb/itXJ+YLFMWMa7OGmNfv3RXZJb35epa5X48xWUm18Y7mSR2Gq6NwSbqYJUinXzgeMvMMj86bCfpp6POCn3fKbKsNdAkXeyRaaZF+pmmuVGWxYXWtdXNu4qdpose3rDJcC0V66+RtXr7RIERchXYHOVltFY62+hIPfQ2WUebbNbLFL0MkKrQKP0stkqu4ToKTJTtHBm+rkGAYwxq41OG67xtre6V7MdFxvtPi+tUoGVRaImD/cN71ltmo1U+s0xLmyyLzq7Ndthhq3Wa+tg6K3xmpa+stNwiHyv0pY7O8be4pXqnlTbaYZFW3jTbdsscZIsPNbbKLBus9pU8K6MCXmehfLvNN1tL0/zDBstssMoGS6XJtsIu86220Re2K/GsLQps0czS/bbmVYG+XqjgfLzetVUcVw5k9DWuvlk4UNDUj+OimErTAmccGN+bqSEyDsBvXNYj+nohDA8LBEr8rcrPlDXgW4amxlkhENjh/oQRkg341qOvly1zWUMIzv9f5On1rd55G9CAbwf+FzUcVoZTUqK4AAAAJXRFWHRkYXRlOmNyZWF0ZQAyMDI1LTA1LTE5VDE2OjA0OjMzKzAwOjAwqCRaugAAACV0RVh0ZGF0ZTptb2RpZnkAMjAyNS0wNS0xOVQxNjowNDozMyswMDowMNl54gYAAAAodEVYdGRhdGU6dGltZXN0YW1wADIwMjUtMDUtMTlUMTY6MDQ6NTArMDA6MDB569DDAAAAAElFTkSuQmCC" alt="SilverLink Logo">
    </div>
    
    <h1>SilverLink IoT Setup</h1>
    
    <form action="/save" method="POST">
      <div class="form-group">
        <label for="ssid">Pilih Jaringan WiFi:</label>
        <select id="ssid" name="ssid">
)rawliteral";

  // Add SSIDs dynamically
  for (auto s : ssidList) {
    html += "<option>" + s + "</option>";
  }
  
  html += R"rawliteral(
        </select>
      </div>
      
      <div class="form-group">
        <label for="password">Password WiFi:</label>
        <input type="password" id="password" name="password" placeholder="Masukkan password WiFi">
      </div>
      
      <div class="form-group">
        <label for="token">Token SilverLink:</label>
        <input type="text" id="token" name="token" placeholder="Masukkan token perangkat">
      </div>
      
      <button type="submit">Hubungkan</button>
    </form>
    
    <div class="footer">
      &copy; 2025 SilverLink IoT.
      <p>Created With ❤️ for Silver Wolf</p>
    </div>
  </div>
</body>
</html>
)rawliteral";

  return html;
}

void updateLCD4(const String& line2 = "", const String& line3 = "", const String& line4 = "") {
  lcd.clear();
  lcd.setCursor(0, 0);
  lcd.print("===SilverLink-IoT===");

  lcd.setCursor(0, 1);
  lcd.print(line2.length() > 20 ? line2.substring(0, 20) : line2);

  lcd.setCursor(0, 2);
  lcd.print(line3.length() > 20 ? line3.substring(0, 20) : line3);

  lcd.setCursor(0, 3);
  lcd.print(line4.length() > 20 ? line4.substring(0, 20) : line4);
}



void setup() {
  lcd.init();
  lcd.backlight();
  lcd.setCursor(0, 0);


  WiFi.setHostname("SilverLink-IoT");
  Serial.begin(115200);
  Serial.println("=================================");
  Serial.println("  SilverLink-IoT System Start!");
  Serial.println("Created With ❤️ for Silver Wolf");
  Serial.println("=================================");
  updateLCD4("Memulai Sistem...","Firmware V1.2");

  checkResetButton();
  if (!LittleFS.begin()) Serial.println("❌ Gagal mount LittleFS");
  if (loadConfig()) {
    WiFi.begin(ssid.c_str(), password.c_str());
    unsigned long startAttemptTime = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - startAttemptTime < 15000) delay(500);
    if (WiFi.status() == WL_CONNECTED) {
      String ip = WiFi.localIP().toString();
      String ssid = WiFi.SSID();
      updateLCD4("WiFi: " + ssid, "IP: " + ip);
      Serial.printf("📡 Terhubung ke SSID: %s", WiFi.SSID().c_str());
      Serial.printf(" - IP Address: %s", WiFi.localIP().toString().c_str());
      Serial.println("");
      updateLCD4("Terhubung ke WiFi","SSID: " + WiFi.SSID(), "IP: " + WiFi.localIP().toString());
      webSocket.begin("silverlink.eula.my.id", 5050, "/ws");
      webSocket.onEvent(webSocketEvent);
      webSocket.setReconnectInterval(5000);
      return;
    } else {
      Serial.println("❌ WiFi gagal, masuk offline 30 menit");
      updateLCD4("WiFi gagal", "Masuk mode offline", "Tunggu 30 menit");

      mode = OFFLINE_WIFI;
      offlineUntil = millis() + 30L * 60 * 1000;
    }
  } else {
access_point_mode:
    scanNetworks();
    WiFi.mode(WIFI_AP);
    WiFi.softAP("SilverLink-Setup");
    updateLCD4("Setup Mode - SSID : ","SilverLink-Setup");
    server.on("/", HTTP_GET, [](AsyncWebServerRequest *req) {
      req->send(200, "text/html", htmlForm());
    });
    server.on("/save", HTTP_POST, [](AsyncWebServerRequest *req) {
      saveConfig(req->getParam("ssid", true)->value(),
                 req->getParam("password", true)->value(),
                 req->getParam("token", true)->value());
      req->send(200, "text/html", "Disimpan. Restart...");
      delay(2000); ESP.restart();
    });
    server.begin();
  }
}

void loop() {
  webSocket.loop();
  checkOfflineMode();
  if (runtimeLoaded) sendSensorData();
}

int serverFailCount = 0;
void webSocketEvent(WStype_t type, uint8_t * payload, size_t length) {
  switch(type) {
    case WStype_CONNECTED:
      serverFailCount = 0;
      Serial.println("🔗 Terhubung ke server");
      updateLCD4("Server Terhubung","Menerima data...");
      {
        DynamicJsonDocument doc(256);
        doc["token"] = token;
        doc["event"] = "auth";
        String out; serializeJson(doc, out);
        webSocket.sendTXT(out);
      }
      break;
    case WStype_DISCONNECTED:
      Serial.println("⚠️ Terputus dari server");
      updateLCD4("Server Terputus", "Menghubungkan...", "Percobaan ke-"+String(serverFailCount));
      serverFailCount++;
      if (serverFailCount >= 5) {
        mode = OFFLINE_SERVER;
        offlineUntil = millis() + 6L * 60 * 60 * 1000;
        updateLCD4("[OFFLINE MODE]","Activated...");
      }
      break;
    case WStype_TEXT:
      Serial.printf("📥 Pesan: %s\n", (char*)payload);
      handleServerCommand((char*)payload);
      break;
  }
}

void sendSensorData() {
  static unsigned long lastSend = 0;
  if (mode != ONLINE || millis() - lastSend < 5000) return;
  lastSend = millis();

  DynamicJsonDocument doc(1024);
  doc["event"] = "sensor_data";
  doc["token"] = token;
  JsonArray data = doc.createNestedArray("data");

  float lastDHTTemp = NAN;
  float lastDHTHum = NAN;

  for (Sensor &s : sensors) {
    // Soil moisture
    if (s.type == "soil_moisture") {
      int raw = analogRead(s.pin);
      s.value = map(raw, 4095, 0, 0, 100);
    }

    // DHT temperature/humidity
    else if (s.type == "dht_temperature" || s.type == "dht_humidity") {
      if (isnan(lastDHTTemp) || isnan(lastDHTHum)) {
        static DHT dht(s.pin, DHT11);  // asumsikan pin sama untuk kedua tipe
        dht.begin();
        delay(300);
        lastDHTTemp = dht.readTemperature();
        lastDHTHum = dht.readHumidity();
        if (isnan(lastDHTTemp) || isnan(lastDHTHum)) {
          Serial.println("⚠️ Gagal baca DHT");
          continue;
        }
      }
      s.value = (s.type == "dht_temperature") ? lastDHTTemp : lastDHTHum;
    }

    JsonObject entry = data.createNestedObject();
    entry["type"] = s.type;
    entry["pin"] = s.pin;
    entry["value"] = s.value;
  }

  String output;
  serializeJson(doc, output);
  webSocket.sendTXT(output);
}


void handleServerCommand(const char* message) {
  DynamicJsonDocument doc(1024);
  DeserializationError err = deserializeJson(doc, message);
  if (err) return;

  if (doc["event"] == "set_aktuator") {
    int pin = doc["pin"];
    bool state = doc["state"];
    for (Aktuator &a : aktuators) {
      if (a.pin == pin) {
        digitalWrite(pin, state ? HIGH : LOW);
        a.state = state;
        Serial.printf("⚡ Aktuator pin %d => %s\n", pin, state ? "ON" : "OFF");
      }
    }
  }
  else if (doc["event"] == "runtime_config") {
    saveRuntimeConfig(message);
    Serial.println("✅ Config runtime diterima dan disimpan");
    updateLCD4("Receive Config Data", "Saving data..");
  }
  else if (doc["event"] == "refresh_config") {
    bool result = loadRuntimeConfig();
    if (result) {
      Serial.println("🔄 Runtime config dimuat ulang");
      updateLCD4("Config diperbarui");
    } else {
      Serial.println("⚠️ Gagal memuat config_runtime.json");
      updateLCD4("❌ Gagal update", "Config runtime error..");
    }
  }

}

void checkOfflineMode() {
  if (mode == ONLINE) return;

  unsigned long now = millis();
  if (now > offlineUntil) {
    Serial.println("🔁 Waktu offline habis, restart koneksi...");
    ESP.restart();
  }

  if (now - lastMoistureCheck >= moistureInterval) {
    for (Sensor s : sensors) {
      if (s.type == "soil_moisture" && s.threshold > 0) {
        int raw = analogRead(s.pin);
        float value = map(raw, 4095, 0, 0, 100);
        if (value < s.threshold) {
          for (Aktuator a : aktuators) {
            if (a.function == "pump") {
              digitalWrite(a.pin, HIGH);
              delay(10000);
              digitalWrite(a.pin, LOW);
              Serial.println("🚿 Pompa aktif (offline)");
            }
          }
        }
      } else if (s.type == "dht_temperature" || s.type == "dht_humidity") {
        DHT dht(s.pin, DHT11);
        dht.begin();
        float temp = dht.readTemperature();
        float hum = dht.readHumidity();
        Serial.printf("🌡️ DHT Temp: %.1f°C | Hum: %.1f%%\n", temp, hum);
      }
    }
    lastMoistureCheck = now;
  }
}
