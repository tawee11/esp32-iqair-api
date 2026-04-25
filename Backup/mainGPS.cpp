/*
  ESP32-S3 YD-ESP32-23 V1.3
  IQAir AQI by lat/lon -> RGB LED

  Features
  - Boot: try saved Wi-Fi first
  - If failed after 3 attempts -> AP config mode
  - Config page allows:
      * WPA/WPA2 Personal
      * WPA2 Enterprise (PEAP)
      * IQAir API Key
      * Latitude / Longitude
      * Use phone location (browser geolocation)
      * Fallback manual lat/lon
      * Scan SSID
      * LED brightness slider 10-100%
  - Poll IQAir every 10 minutes
  - Use AQI from: data.current.pollution.aqius
  - After Wi-Fi connected, fetch API immediately

  Required libraries:
    - ArduinoJson
    - Adafruit NeoPixel
*/

#include <WiFi.h>
#include <WebServer.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <Preferences.h>
#include <Adafruit_NeoPixel.h>

static const int LED_PIN = 48;

// ---------------------------
// User settings / defaults
// ---------------------------
static const char* AP_SSID = "ESP32-IQAir-Config";
static const char* AP_PASS = "12345678";
static const char* IQAIR_BASE_URL = "http://api.airvisual.com/v2/nearest_city";

static const uint32_t WIFI_CONNECT_TIMEOUT_MS = 15000;
static const uint8_t  WIFI_MAX_ATTEMPTS = 3;
static const uint32_t POLL_INTERVAL_MS = 10UL * 60UL * 1000UL;

// ---------------------------
// Global objects
// ---------------------------
Preferences prefs;
WebServer server(80);
Adafruit_NeoPixel pixel(1, LED_PIN, NEO_GRB + NEO_KHZ800);

// ---------------------------
// Config structure
// ---------------------------
struct AppConfig {
  String wifiMode;     // "personal" or "enterprise"
  String ssid;
  String password;

  // Enterprise
  String identity;
  String username;
  String eapPassword;

  // IQAir by lat/lon
  String apiKey;
  String latitude;
  String longitude;

  uint8_t brightnessPercent; // 10..100
};

AppConfig cfg;

bool wifiConnected = false;
bool apMode = false;
unsigned long lastPollMs = 0;
unsigned long lastRainbowStepMs = 0;
uint16_t rainbowIndex = 0;

// preview control in AP mode
bool previewMode = false;
uint8_t previewR = 0, previewG = 0, previewB = 180;

// ---------------------------
// Helpers
// ---------------------------
String htmlEscape(const String& s) {
  String out = s;
  out.replace("&", "&amp;");
  out.replace("<", "&lt;");
  out.replace(">", "&gt;");
  out.replace("\"", "&quot;");
  return out;
}

String urlEncodeSimple(const String& s) {
  String out;
  char hex[4];
  for (size_t i = 0; i < s.length(); i++) {
    char c = s[i];
    if (isalnum((unsigned char)c) || c == '-' || c == '_' || c == '.' || c == '~') {
      out += c;
    } else {
      snprintf(hex, sizeof(hex), "%%%02X", (unsigned char)c);
      out += hex;
    }
  }
  return out;
}

bool isValidLatLon(const String& latStr, const String& lonStr, float &lat, float &lon) {
  if (latStr.isEmpty() || lonStr.isEmpty()) return false;

  lat = latStr.toFloat();
  lon = lonStr.toFloat();

  bool latLooksZero = (lat == 0.0f && latStr != "0" && latStr != "0.0" && latStr != "0.00");
  bool lonLooksZero = (lon == 0.0f && lonStr != "0" && lonStr != "0.0" && lonStr != "0.00");
  if (latLooksZero || lonLooksZero) return false;

  if (lat < -90.0f || lat > 90.0f) return false;
  if (lon < -180.0f || lon > 180.0f) return false;

  return true;
}

String buildIQAirUrl() {
  return String(IQAIR_BASE_URL)
       + "?lat=" + urlEncodeSimple(cfg.latitude)
       + "&lon=" + urlEncodeSimple(cfg.longitude)
       + "&key=" + urlEncodeSimple(cfg.apiKey);
}

uint8_t percentToNeoBrightness(uint8_t percent) {
  if (percent < 10) percent = 10;
  if (percent > 100) percent = 100;
  return map(percent, 10, 100, 26, 255);
}

void applyBrightness() {
  pixel.setBrightness(percentToNeoBrightness(cfg.brightnessPercent));
}

void setLed(uint8_t r, uint8_t g, uint8_t b) {
  applyBrightness();
  pixel.setPixelColor(0, pixel.Color(r, g, b));
  pixel.show();
}

void ledOff() {
  applyBrightness();
  pixel.setPixelColor(0, pixel.Color(0, 0, 0));
  pixel.show();
}

uint32_t wheel(byte pos) {
  pos = 255 - pos;
  if (pos < 85) {
    return pixel.Color(255 - pos * 3, 0, pos * 3);
  }
  if (pos < 170) {
    pos -= 85;
    return pixel.Color(0, pos * 3, 255 - pos * 3);
  }
  pos -= 170;
  return pixel.Color(pos * 3, 255 - pos * 3, 0);
}

void rainbowStep() {
  if (millis() - lastRainbowStepMs < 40) return;
  lastRainbowStepMs = millis();

  applyBrightness();
  pixel.setPixelColor(0, wheel(rainbowIndex & 255));
  pixel.show();
  rainbowIndex++;
}

void blinkColor(uint8_t r, uint8_t g, uint8_t b, int times = 2, int onMs = 180, int offMs = 120) {
  for (int i = 0; i < times; i++) {
    setLed(r, g, b);
    delay(onMs);
    ledOff();
    delay(offMs);
  }
}

void showAQIColor(int aqi) {
  previewMode = false;

  if (aqi <= 50) {
    setLed(0, 180, 0);
  } else if (aqi <= 100) {
    setLed(255, 180, 0);
  } else if (aqi <= 150) {
    setLed(255, 100, 0);
  } else if (aqi <= 200) {
    setLed(255, 0, 0);
  } else if (aqi <= 300) {
    setLed(140, 0, 255);
  } else {
    setLed(128, 0, 0);
  }
}

void showApModeLed() {
  if (previewMode) {
    setLed(previewR, previewG, previewB);
  } else {
    setLed(0, 0, 180);
  }
}

// ---------------------------
// HTML page
// ---------------------------
String makePage(const String& msg = "") {
  String checkedPersonal   = (cfg.wifiMode != "enterprise") ? "checked" : "";
  String checkedEnterprise = (cfg.wifiMode == "enterprise") ? "checked" : "";

  String page = R"HTML(
<!DOCTYPE html>
<html>
<head>
  <meta charset="utf-8">
  <meta name="viewport" content="width=device-width,initial-scale=1">
  <title>ESP32 IQAir Config</title>
  <style>
    body { font-family: Arial, sans-serif; max-width: 760px; margin: 20px auto; padding: 0 16px; background:#f5f7fb; }
    .card { background:#fff; border-radius:14px; padding:20px; box-shadow:0 2px 10px rgba(0,0,0,.08); }
    h1 { margin-top:0; font-size:24px; }
    label { display:block; margin-top:12px; font-weight:600; }
    input, select, button { font-size:16px; }
    input[type="text"], input[type="password"], input[type="number"] {
      width:100%; padding:10px; margin-top:6px; border:1px solid #ccc; border-radius:10px; box-sizing:border-box;
    }
    button {
      margin-top:12px; padding:12px 18px; border:none; border-radius:12px;
      background:#2563eb; color:#fff; cursor:pointer;
    }
    .btn2 { background:#0f766e; }
    .btn3 { background:#7c3aed; }
    .msg { margin:12px 0; padding:10px 12px; border-radius:10px; background:#eef6ff; color:#184b8a; }
    .warn { background:#fff4e5; color:#8a5a00; }
    .ok { background:#ecfdf5; color:#166534; }
    .section { margin-top:20px; padding-top:12px; border-top:1px solid #eee; }
    .radio-group { display:flex; gap:20px; margin-top:10px; }
    .radio-group label { font-weight:normal; }
    .small { font-size:13px; color:#666; }
    .ssid-item {
      padding:10px; border:1px solid #ddd; border-radius:10px; margin-top:8px; cursor:pointer;
      background:#fafafa;
    }
    .ssid-item:hover { background:#eef6ff; }
    .preview-box {
      margin-top:10px; width:100%; height:42px; border-radius:12px; border:1px solid #ddd;
      background: linear-gradient(90deg, #0000ff, #00ffff, #00ff00, #ffff00, #ff7f00, #ff0000);
      opacity: 0.9;
    }
    .inline { display:flex; gap:10px; align-items:center; flex-wrap:wrap; }
    .value-badge {
      padding:6px 10px; border-radius:999px; background:#eef2ff; display:inline-block; font-weight:700;
    }
    .grid2 {
      display:grid;
      grid-template-columns:1fr 1fr;
      gap:12px;
    }
    @media (max-width:600px) {
      .grid2 { grid-template-columns:1fr; }
    }
  </style>
  <script>
    function updateMode() {
      const modeEl = document.querySelector('input[name="wifiMode"]:checked');
      const mode = modeEl ? modeEl.value : 'personal';
      const ent = document.getElementById('enterpriseFields');
      ent.style.display = (mode === 'enterprise') ? 'block' : 'none';
    }

    async function scanSSID() {
      const box = document.getElementById('scanResults');
      box.innerHTML = '<div class="msg">กำลังสแกน Wi-Fi...</div>';
      try {
        const res = await fetch('/scan');
        const data = await res.json();
        if (!data.networks || data.networks.length === 0) {
          box.innerHTML = '<div class="msg warn">ไม่พบ SSID</div>';
          return;
        }

        let html = '';
        data.networks.forEach(n => {
          html += `<div class="ssid-item" onclick="pickSSID('${String(n.ssid).replace(/'/g, "\\'")}')">
                    <b>${n.ssid}</b><br>
                    RSSI: ${n.rssi} dBm | ${n.secure ? 'Secure' : 'Open'}
                  </div>`;
        });
        box.innerHTML = html;
      } catch (e) {
        box.innerHTML = '<div class="msg warn">สแกนไม่สำเร็จ</div>';
      }
    }

    function pickSSID(ssid) {
      document.querySelector('input[name="ssid"]').value = ssid;
      window.scrollTo({ top: 0, behavior: 'smooth' });
    }

    async function previewBrightness() {
      const v = document.getElementById('brightness').value;
      document.getElementById('brightnessValue').innerText = v + '%';
      try {
        await fetch('/previewBrightness?value=' + encodeURIComponent(v));
      } catch(e) {}
    }

    function setLocationMessage(text, cls) {
      const box = document.getElementById('locationMsg');
      box.className = 'msg ' + (cls || '');
      box.innerText = text;
      box.style.display = 'block';
    }

    function usePhoneLocation() {
      if (!navigator.geolocation) {
        setLocationMessage('Browser ไม่รองรับ Geolocation กรุณากรอก Latitude/Longitude เอง', 'warn');
        return;
      }

      setLocationMessage('กำลังอ่านตำแหน่งจากโทรศัพท์...', '');

      navigator.geolocation.getCurrentPosition(
        function(pos) {
          const lat = pos.coords.latitude.toFixed(6);
          const lon = pos.coords.longitude.toFixed(6);

          document.getElementById('latitude').value = lat;
          document.getElementById('longitude').value = lon;

          setLocationMessage('ดึงตำแหน่งสำเร็จ: lat=' + lat + ', lon=' + lon, 'ok');
        },
        function(err) {
          let msg = 'อ่านตำแหน่งไม่ได้';
          if (err && err.message) msg += ': ' + err.message;
          msg += ' กรุณากรอก Latitude/Longitude เอง';
          setLocationMessage(msg, 'warn');
        },
        {
          enableHighAccuracy: true,
          timeout: 10000,
          maximumAge: 60000
        }
      );
    }

    window.onload = function() {
      updateMode();
      previewBrightness();
    };
  </script>
</head>
<body>
  <div class="card">
    <h1>ESP32 IQAir Config</h1>
)HTML";

  if (msg.length()) {
    page += "<div class='msg'>" + msg + "</div>";
  }

  page += "<div class='msg warn'>หลังบันทึกค่า อุปกรณ์จะลองเชื่อมต่อ Wi-Fi ใหม่ทันที และจะดึง AQI ทันทีเมื่อเชื่อมต่อสำเร็จ</div>";

  page += R"HTML(
    <form method="POST" action="/save">
      <label>Wi-Fi Type</label>
      <div class="radio-group">
        <label><input type="radio" name="wifiMode" value="personal" )HTML";
  page += checkedPersonal;
  page += R"HTML( onclick="updateMode()"> WPA/WPA2 Personal</label>
        <label><input type="radio" name="wifiMode" value="enterprise" )HTML";
  page += checkedEnterprise;
  page += R"HTML( onclick="updateMode()"> WPA2 Enterprise</label>
      </div>

      <div class="section">
        <div class="inline">
          <label style="margin:0;">SSID</label>
          <button type="button" class="btn2" onclick="scanSSID()">Scan SSID</button>
        </div>
        <input name="ssid" value=")HTML" + htmlEscape(cfg.ssid) + R"HTML(">
        <div id="scanResults"></div>

        <label>Password</label>
        <input name="password" type="password" value=")HTML" + htmlEscape(cfg.password) + R"HTML(">
        <div class="small">สำหรับ Personal ใช้ช่องนี้เป็นรหัสผ่าน Wi-Fi ปกติ</div>
      </div>

      <div class="section" id="enterpriseFields">
        <label>Identity</label>
        <input name="identity" value=")HTML" + htmlEscape(cfg.identity) + R"HTML(">

        <label>Username</label>
        <input name="username" value=")HTML" + htmlEscape(cfg.username) + R"HTML(">

        <label>Enterprise Password</label>
        <input name="eapPassword" type="password" value=")HTML" + htmlEscape(cfg.eapPassword) + R"HTML(">
        <div class="small">สำหรับ WPA2 Enterprise (PEAP)</div>
      </div>

      <div class="section">
        <label>IQAir API Key</label>
        <input name="apiKey" value=")HTML" + htmlEscape(cfg.apiKey) + R"HTML(">

        <div class="inline">
          <label style="margin:0;">Location</label>
          <button type="button" class="btn3" onclick="usePhoneLocation()">Use phone location</button>
        </div>

        <div id="locationMsg" class="msg" style="display:none;"></div>

        <div class="grid2">
          <div>
            <label>Latitude</label>
            <input id="latitude" name="latitude" value=")HTML" + htmlEscape(cfg.latitude) + R"HTML(" placeholder="เช่น 13.7563">
          </div>
          <div>
            <label>Longitude</label>
            <input id="longitude" name="longitude" value=")HTML" + htmlEscape(cfg.longitude) + R"HTML(" placeholder="เช่น 100.5018">
          </div>
        </div>
        <div class="small">กด Use phone location เพื่อดึงพิกัดอัตโนมัติ หรือกรอกเองได้หาก browser ไม่อนุญาต</div>
      </div>

      <div class="section">
        <label>LED Brightness</label>
        <div class="inline">
          <input
            id="brightness"
            name="brightness"
            type="range"
            min="10"
            max="100"
            step="1"
            value=")HTML" + String(cfg.brightnessPercent) + R"HTML("
            oninput="previewBrightness()"
            style="width:320px;"
          >
          <span id="brightnessValue" class="value-badge">)HTML" + String(cfg.brightnessPercent) + R"HTML(%
          </span>
        </div>
        <div class="preview-box"></div>
        <div class="small">ลากเพื่อปรับความสว่าง 10% ถึง 100% และ LED บนบอร์ดจะแสดงให้ดูทันที</div>
      </div>

      <button type="submit">Save & Connect</button>
    </form>

    <div class="section">
      <div><b>AP SSID:</b> )HTML";
  page += AP_SSID;
  page += R"HTML(</div>
      <div><b>AP IP:</b> 192.168.4.1</div>
    </div>
  </div>
</body>
</html>
)HTML";

  return page;
}

// ---------------------------
// Preferences
// ---------------------------
void loadConfig() {
  cfg.wifiMode = "personal";
  cfg.apiKey = "";
  cfg.latitude = "";
  cfg.longitude = "";
  cfg.brightnessPercent = 30;

  if (!prefs.begin("iqaircfg", true)) {
    Serial.println("NVS namespace 'iqaircfg' not found yet. Using defaults.");
    return;
  }

  cfg.wifiMode          = prefs.getString("wifiMode", "personal");
  cfg.ssid              = prefs.getString("ssid", "");
  cfg.password          = prefs.getString("password", "");
  cfg.identity          = prefs.getString("identity", "");
  cfg.username          = prefs.getString("username", "");
  cfg.eapPassword       = prefs.getString("eapPass", "");
  cfg.apiKey            = prefs.getString("apiKey", "");
  cfg.latitude          = prefs.getString("lat", "");
  cfg.longitude         = prefs.getString("lon", "");
  cfg.brightnessPercent = (uint8_t)prefs.getUChar("brightPct", 30);

  if (cfg.brightnessPercent < 10 || cfg.brightnessPercent > 100) {
    cfg.brightnessPercent = 30;
  }

  prefs.end();
}

void saveConfig() {
  if (!prefs.begin("iqaircfg", false)) {
    Serial.println("NVS open failed in saveConfig().");
    return;
  }

  prefs.putString("wifiMode", cfg.wifiMode);
  prefs.putString("ssid", cfg.ssid);
  prefs.putString("password", cfg.password);
  prefs.putString("identity", cfg.identity);
  prefs.putString("username", cfg.username);
  prefs.putString("eapPass", cfg.eapPassword);
  prefs.putString("apiKey", cfg.apiKey);
  prefs.putString("lat", cfg.latitude);
  prefs.putString("lon", cfg.longitude);
  prefs.putUChar("brightPct", cfg.brightnessPercent);

  prefs.end();
}

// ---------------------------
// Wi-Fi connect
// ---------------------------
bool tryConnectWiFiOnce() {
  if (cfg.ssid.isEmpty()) {
    Serial.println("No saved SSID.");
    return false;
  }

  WiFi.disconnect(true, true);
  delay(300);
  WiFi.mode(WIFI_STA);
  delay(200);

  Serial.printf("Connecting to SSID: %s\n", cfg.ssid.c_str());

  if (cfg.wifiMode == "enterprise") {
    Serial.println("WiFi mode: WPA2 Enterprise");
    String identity = cfg.identity.length() ? cfg.identity : cfg.username;

    WiFi.begin(
      cfg.ssid.c_str(),
      WPA2_AUTH_PEAP,
      identity.c_str(),
      cfg.username.c_str(),
      cfg.eapPassword.c_str()
    );
  } else {
    Serial.println("WiFi mode: WPA/WPA2 Personal");
    WiFi.begin(cfg.ssid.c_str(), cfg.password.c_str());
  }

  unsigned long startMs = millis();
  while (millis() - startMs < WIFI_CONNECT_TIMEOUT_MS) {
    rainbowStep();
    if (WiFi.status() == WL_CONNECTED) {
      Serial.println("\nWiFi connected.");
      Serial.print("IP: ");
      Serial.println(WiFi.localIP());
      return true;
    }
    delay(20);
  }

  Serial.println("\nWiFi connect timeout.");
  return false;
}

bool fetchIQAirAndUpdateLed();

bool connectWiFiWithRetries() {
  for (uint8_t i = 0; i < WIFI_MAX_ATTEMPTS; i++) {
    Serial.printf("WiFi attempt %u/%u\n", i + 1, WIFI_MAX_ATTEMPTS);
    if (tryConnectWiFiOnce()) {
      wifiConnected = true;
      apMode = false;
      previewMode = false;
      blinkColor(0, 180, 0, 2);

      fetchIQAirAndUpdateLed();
      lastPollMs = millis();

      return true;
    }
    blinkColor(255, 0, 0, 1);
    delay(500);
  }

  wifiConnected = false;
  return false;
}

// ---------------------------
// AP + web config
// ---------------------------
void startAPMode() {
  WiFi.disconnect(true, true);
  delay(200);
  WiFi.mode(WIFI_AP);
  delay(200);

  bool ok = WiFi.softAP(AP_SSID, AP_PASS);
  apMode = true;
  wifiConnected = false;

  Serial.println(ok ? "AP started." : "AP start failed.");
  Serial.print("AP IP: ");
  Serial.println(WiFi.softAPIP());

  showApModeLed();
}

void handleRoot() {
  server.send(200, "text/html; charset=utf-8", makePage());
}

void handleStatus() {
  String json = "{";
  json += "\"apMode\":" + String(apMode ? "true" : "false") + ",";
  json += "\"wifiConnected\":" + String(wifiConnected ? "true" : "false") + ",";
  json += "\"ip\":\"" + (wifiConnected ? WiFi.localIP().toString() : WiFi.softAPIP().toString()) + "\",";
  json += "\"ssid\":\"" + htmlEscape(cfg.ssid) + "\",";
  json += "\"apiKey\":\"" + htmlEscape(cfg.apiKey) + "\",";
  json += "\"latitude\":\"" + htmlEscape(cfg.latitude) + "\",";
  json += "\"longitude\":\"" + htmlEscape(cfg.longitude) + "\",";
  json += "\"brightness\":" + String(cfg.brightnessPercent);
  json += "}";
  server.send(200, "application/json", json);
}

void handleScan() {
  WiFi.mode(WIFI_AP_STA);
  delay(100);

  int n = WiFi.scanNetworks();
  String json = "{\"networks\":[";
  for (int i = 0; i < n; i++) {
    if (i > 0) json += ",";
    String ssid = WiFi.SSID(i);
    ssid.replace("\\", "\\\\");
    ssid.replace("\"", "\\\"");
    json += "{";
    json += "\"ssid\":\"" + ssid + "\",";
    json += "\"rssi\":" + String(WiFi.RSSI(i)) + ",";
    json += "\"secure\":" + String((WiFi.encryptionType(i) != WIFI_AUTH_OPEN) ? "true" : "false");
    json += "}";
  }
  json += "]}";

  WiFi.scanDelete();
  server.send(200, "application/json", json);
}

void handlePreviewBrightness() {
  int v = server.arg("value").toInt();
  if (v < 10) v = 10;
  if (v > 100) v = 100;

  cfg.brightnessPercent = (uint8_t)v;
  previewMode = true;

  previewR = 0;
  previewG = 120;
  previewB = 255;
  showApModeLed();

  server.send(200, "text/plain", "OK");
}

String trimmedArg(const char* name) {
  String v = server.arg(name);
  v.trim();
  return v;
}

void handleSave() {
  cfg.wifiMode    = trimmedArg("wifiMode");
  cfg.ssid        = trimmedArg("ssid");
  cfg.password    = trimmedArg("password");
  cfg.identity    = trimmedArg("identity");
  cfg.username    = trimmedArg("username");
  cfg.eapPassword = trimmedArg("eapPassword");

  cfg.apiKey      = trimmedArg("apiKey");
  cfg.latitude    = trimmedArg("latitude");
  cfg.longitude   = trimmedArg("longitude");

  int bright = server.arg("brightness").toInt();
  if (bright < 10) bright = 10;
  if (bright > 100) bright = 100;
  cfg.brightnessPercent = (uint8_t)bright;

  if (cfg.wifiMode != "enterprise") cfg.wifiMode = "personal";

  saveConfig();

  server.send(200, "text/html; charset=utf-8",
              makePage("บันทึกค่าแล้ว กำลังลองเชื่อมต่อ Wi-Fi ใหม่ และจะดึง AQI ทันทีเมื่อเชื่อมต่อสำเร็จ..."));

  delay(800);

  if (connectWiFiWithRetries()) {
    Serial.println("Reconnected after saving config.");
  } else {
    Serial.println("Reconnect failed, return to AP mode.");
    startAPMode();
  }
}

void setupWebServer() {
  server.on("/", HTTP_GET, handleRoot);
  server.on("/status", HTTP_GET, handleStatus);
  server.on("/scan", HTTP_GET, handleScan);
  server.on("/previewBrightness", HTTP_GET, handlePreviewBrightness);
  server.on("/save", HTTP_POST, handleSave);
  server.begin();
  Serial.println("Web server started.");
}

// ---------------------------
// IQAir polling
// ---------------------------
bool fetchIQAirAndUpdateLed() {
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("fetch skipped: WiFi not connected.");
    return false;
  }

  float lat, lon;
  if (cfg.apiKey.isEmpty()) {
    Serial.println("IQAir API key is empty.");
    blinkColor(255, 0, 0, 2);
    return false;
  }
  if (!isValidLatLon(cfg.latitude, cfg.longitude, lat, lon)) {
    Serial.println("Latitude/Longitude invalid.");
    blinkColor(255, 0, 0, 2);
    return false;
  }

  String url = buildIQAirUrl();
  HTTPClient http;
  http.setTimeout(15000);

  Serial.print("GET: ");
  Serial.println(url);

  if (!http.begin(url)) {
    Serial.println("HTTP begin failed.");
    blinkColor(255, 0, 0, 2);
    return false;
  }

  int httpCode = http.GET();
  if (httpCode <= 0) {
    Serial.printf("HTTP GET failed: %d\n", httpCode);
    http.end();
    blinkColor(255, 0, 0, 2);
    return false;
  }

  Serial.printf("HTTP code: %d\n", httpCode);

  if (httpCode != HTTP_CODE_OK) {
    String errBody = http.getString();
    Serial.println(errBody);
    http.end();
    blinkColor(255, 0, 0, 2);
    return false;
  }

  String payload = http.getString();
  http.end();

  DynamicJsonDocument doc(32 * 1024);
  DeserializationError err = deserializeJson(doc, payload);
  if (err) {
    Serial.print("JSON parse failed: ");
    Serial.println(err.c_str());
    blinkColor(255, 0, 0, 2);
    return false;
  }

  const char* status = doc["status"] | "";
  if (String(status) != "success") {
    Serial.print("API status not success: ");
    Serial.println(status);
    blinkColor(255, 0, 0, 2);
    return false;
  }

  int aqius = doc["data"]["current"]["pollution"]["aqius"] | -1;
  int aqicn = doc["data"]["current"]["pollution"]["aqicn"] | -1;
  const char* mainus = doc["data"]["current"]["pollution"]["mainus"] | "";
  const char* ts = doc["data"]["current"]["pollution"]["ts"] | "";
  const char* city = doc["data"]["city"] | "";
  const char* state = doc["data"]["state"] | "";
  const char* country = doc["data"]["country"] | "";

  if (aqius < 0) {
    Serial.println("data.current.pollution.aqius field missing.");
    blinkColor(255, 0, 0, 2);
    return false;
  }

  Serial.printf("Location: %s, %s, %s\n", city, state, country);
  Serial.printf("AQI(US): %d, AQI(CN): %d, mainus: %s, ts: %s\n", aqius, aqicn, mainus, ts);

  showAQIColor(aqius);
  return true;
}

void ensureWiFiAlive() {
  if (apMode) return;
  if (WiFi.status() == WL_CONNECTED) return;

  Serial.println("WiFi lost. Reconnecting...");
  if (!connectWiFiWithRetries()) {
    Serial.println("Reconnect failed. Switching to AP mode.");
    startAPMode();
  }
}

// ---------------------------
// Setup / loop
// ---------------------------
void setup() {
  Serial.begin(115200);
  delay(1000);

  pixel.begin();
  loadConfig();
  applyBrightness();
  ledOff();

  Serial.println("Boot: try saved WiFi first.");
  if (connectWiFiWithRetries()) {
    Serial.println("Connected on boot.");
  } else {
    Serial.println("Boot WiFi failed. Enter AP mode.");
    startAPMode();
  }

  setupWebServer();
}

void loop() {
  server.handleClient();

  if (apMode) {
    delay(10);
    return;
  }

  ensureWiFiAlive();

  if (!apMode && WiFi.status() == WL_CONNECTED) {
    unsigned long now = millis();
    if (now - lastPollMs >= POLL_INTERVAL_MS) {
      lastPollMs = now;
      fetchIQAirAndUpdateLed();
    }
  }

  delay(10);
}