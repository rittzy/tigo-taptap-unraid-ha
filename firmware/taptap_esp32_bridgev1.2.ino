/*
taptap_esp32_bridge -- BUILD: Tigo TapTap ESP32 Bridge v1.2 (public release)
Transparent RS-485 via MAX485 - TCP bridge for use with https://github.com/willglynn/taptap

IMPORTANT: taptap's --tcp flag connects to a hardcoded default port of 7160 (confirmed via
willglynn/taptap issue #5), NOT an arbitrary port. This bridge now listens on 7160 to match,
so you can simply run: taptap observe --tcp <esp32-ip-address> with no port needed.

Web dashboard & WiFi config page:
- http://<esp32-ip>          -> status dashboard (auto-refreshes)
- http://<esp32-ip>/wifi     -> WiFi settings page

Behaviour:
- Config AP "TapTap-Setup" starts on boot alongside trying saved WiFi.
- Once STA connects, AP turns off automatically.
- If STA is down >60 seconds, AP turns back on automatically.

Config AP:
- SSID: TapTap-Setup
- PASSWORD: taptap123
- IP: 192.168.4.1

Wiring (MAX485 -> ESP32):
- VCC -> 3.3V (or 5V if using a genuine 5V MAX485 board)
- GND -> GND
- DI  -> GPIO17 (TX2)
- RO  -> GPIO16 (RX2)
- DE  -> GND
- RE  -> GND
- A/B -> Tigo GATEWAY port A/B

Bus parameters: 38400 baud, 8N1, half-duplex RS-485.

On your Docker/Unraid host, run: taptap observe --tcp <esp32-ip-address>

v1.2 changes:
- Scan results are now deduplicated by SSID (mesh/dual-band routers that
  broadcast the same name on multiple channels used to show as separate
  duplicate entries - now they're merged into one, keeping the strongest
  signal seen across all channels).
- Each network in the scan list now shows a small signal-strength bar
  based on RSSI, sorted strongest-first.

v1.1 changes:
- Fixed mojibake on the gear/back-arrow symbols by declaring UTF-8 charset
  on every HTML response (was defaulting to Windows-1252 in the browser).
- Reworked the "Scan nearby" button: visible loading state, clickable
  results list instead of a hidden <datalist>, and clear error messages.
*/

#include <WiFi.h>
#include <WebServer.h>
#include <Preferences.h>

const uint16_t TCP_PORT = 7160;      // taptap's hardcoded default --tcp port
const uint16_t HTTP_PORT = 80;

const char* AP_SSID = "TapTap-Setup";
const char* AP_PASSWORD = "taptap123";
const int AP_CHANNEL = 6;
const bool AP_HIDDEN = false;
const int AP_MAX_CLIENTS = 4;

const unsigned long STA_TIMEOUT_MS = 15000;
const unsigned long AP_REENABLE_AFTER_MS = 60000;
const unsigned long STA_RETRY_INTERVAL_MS = 15000;

#define RS485_RX_PIN 16
#define RS485_TX_PIN 17
#define RS485_BAUD 38400

WiFiServer bridgeServer(TCP_PORT);
WiFiClient bridgeClient;
WebServer http(HTTP_PORT);
Preferences prefs;

String savedSSID, savedPASS;
unsigned long totalBytes = 0;
unsigned long lastByteMillis = 0;
unsigned long bootMillis = 0;
unsigned long staDownSince = 0;
bool apCurrentlyOn = true;

void loadCredentials() {
  prefs.begin("wifi", true);
  savedSSID = prefs.getString("ssid", "");
  savedPASS = prefs.getString("pass", "");
  prefs.end();
}

void saveCredentials(const String& ssid, const String& pass) {
  prefs.begin("wifi", false);
  prefs.putString("ssid", ssid);
  prefs.putString("pass", pass);
  prefs.end();
}

void clearCredentials() {
  prefs.begin("wifi", false);
  prefs.clear();
  prefs.end();
}

void startWiFi() {
  WiFi.mode(WIFI_AP_STA);
  WiFi.setSleep(false);

  apCurrentlyOn = true;
  bool apOk = WiFi.softAP(AP_SSID, AP_PASSWORD, AP_CHANNEL, AP_HIDDEN, AP_MAX_CLIENTS);
  Serial.println("BUILD: Tigo TapTap ESP32 Bridge v1.2 (public release)");
  Serial.println(apOk ? "Config AP started successfully" : "Config AP FAILED to start");
  Serial.print("AP SSID: "); Serial.println(AP_SSID);
  Serial.print("AP IP: "); Serial.println(WiFi.softAPIP());
  Serial.print("Bridge listening on TCP port: "); Serial.println(TCP_PORT);

  if (savedSSID.length() == 0) {
    Serial.println("No saved WiFi credentials - AP is ready, go to http://192.168.4.1/wifi to configure");
    return;
  }

  Serial.print("Attempting STA connection to: "); Serial.println(savedSSID);
  WiFi.begin(savedSSID.c_str(), savedPASS.c_str());

  unsigned long start = millis();
  while (WiFi.status() != WL_CONNECTED && (millis() - start) < STA_TIMEOUT_MS) {
    delay(250);
    Serial.print(".");
  }
  Serial.println();

  if (WiFi.status() == WL_CONNECTED) {
    Serial.print("STA connected, IP: "); Serial.println(WiFi.localIP());
    Serial.println("Turning off config AP now that STA is up");
    WiFi.mode(WIFI_STA);
    apCurrentlyOn = false;
    staDownSince = 0;
  } else {
    Serial.println("STA connection failed/timed out after 15s - keeping AP on");
    staDownSince = millis();
  }
}

void manageWiFiAndAP() {
  static unsigned long lastRetry = 0;
  bool connected = (WiFi.status() == WL_CONNECTED);

  if (connected) {
    staDownSince = 0;
    if (apCurrentlyOn) {
      Serial.println("STA reconnected - turning AP off again");
      WiFi.mode(WIFI_STA);
      apCurrentlyOn = false;
    }
    return;
  }

  if (staDownSince == 0) staDownSince = millis();

  if (savedSSID.length() > 0 && (millis() - lastRetry) > STA_RETRY_INTERVAL_MS) {
    lastRetry = millis();
    WiFi.begin(savedSSID.c_str(), savedPASS.c_str());
  }

  if (!apCurrentlyOn && (millis() - staDownSince) > AP_REENABLE_AFTER_MS) {
    Serial.println("STA down for 60s, re-enabling config AP");
    WiFi.mode(WIFI_AP_STA);
    WiFi.softAP(AP_SSID, AP_PASSWORD, AP_CHANNEL, AP_HIDDEN, AP_MAX_CLIENTS);
    apCurrentlyOn = true;
  }
}

String htmlHeader(const String& title) {
  String s = "<!DOCTYPE html><html><head>";
  s += "<meta charset='UTF-8'>";
  s += "<meta name='viewport' content='width=device-width,initial-scale=1'>";
  s += "<title>" + title + "</title>";
  s += "<style>";
  s += "body{font-family:sans-serif;max-width:480px;margin:20px auto;padding:0 12px;background:#f4f4f4}";
  s += "h2{color:#222}";
  s += ".card{background:#fff;border-radius:8px;padding:16px;margin-bottom:14px;box-shadow:0 1px 3px rgba(0,0,0,.15)}";
  s += "table{width:100%;border-collapse:collapse}";
  s += "td{padding:6px 0;border-bottom:1px solid #eee}";
  s += "td:first-child{color:#666}";
  s += "input{width:100%;padding:8px;margin:6px 0;box-sizing:border-box}";
  s += "button{background:#2b7de9;color:#fff;border:0;padding:10px 16px;border-radius:6px;cursor:pointer}";
  s += "a{color:#2b7de9;text-decoration:none}";
  s += ".ok{color:#1a8a3c;font-weight:bold}";
  s += ".bad{color:#c0392b;font-weight:bold}";
  s += ".hint{color:#888;font-size:0.85em}";
  s += "#scanStatus{margin:8px 0;font-size:0.9em;color:#555}";
  s += "#ssidResults{list-style:none;padding:0;margin:8px 0}";
  s += "#ssidResults li{display:flex;align-items:center;justify-content:space-between;padding:8px 10px;border:1px solid #ddd;border-radius:6px;margin-bottom:6px;cursor:pointer;background:#fafafa}";
  s += "#ssidResults li:active{background:#e6f0ff}";
  s += ".sigbars{display:inline-flex;align-items:flex-end;height:14px;gap:2px}";
  s += ".sigbars span{display:block;width:4px;background:#ccc;border-radius:1px}";
  s += ".sigbars span:nth-child(1){height:4px}";
  s += ".sigbars span:nth-child(2){height:7px}";
  s += ".sigbars span:nth-child(3){height:10px}";
  s += ".sigbars span:nth-child(4){height:14px}";
  s += ".sigbars span.on{background:#2b7de9}";
  s += "</style></head><body>";
  return s;
}

String formatBytes(unsigned long bytes) {
  if (bytes < 1000) {
    return String(bytes) + " B";
  } else {
    float kb = bytes / 1000.0;
    if (kb < 1000) {
      return String(kb, 1) + " kB";
    } else {
      float mb = kb / 1000.0;
      return String(mb, 1) + " MB";
    }
  }
}

String formatUptime(unsigned long seconds) {
  unsigned long h = seconds / 3600;
  unsigned long m = (seconds % 3600) / 60;
  unsigned long s = seconds % 60;

  String mm = (m < 10) ? "0" + String(m) : String(m);
  String ss = (s < 10) ? "0" + String(s) : String(s);

  return String(h) + ":" + mm + ":" + ss;
}

void handleRoot() {
  bool staConnected = (WiFi.status() == WL_CONNECTED);
  unsigned long sinceLast = (lastByteMillis == 0) ? 0 : ((millis() - lastByteMillis) / 1000);
  unsigned long uptime = (millis() - bootMillis) / 1000;

  String s = htmlHeader("TapTap Bridge Status");
  s += "<meta http-equiv='refresh' content='3'>";
  s += "<h2>TapTap RS-485 Bridge</h2>";
  s += "<p class='hint'>Build: Tigo TapTap ESP32 Bridge v1.2</p>";

  s += "<div class='card'><table>";
  s += "<tr><td>WiFi station</td><td>" + String(staConnected ? "<span class='ok'>Connected</span>" : "<span class='bad'>Not connected</span>") + "</td></tr>";
  if (staConnected) {
    s += "<tr><td>SSID</td><td>" + WiFi.SSID() + "</td></tr>";
    s += "<tr><td>IP address</td><td>" + WiFi.localIP().toString() + "</td></tr>";
    s += "<tr><td>Signal (RSSI)</td><td>" + String(WiFi.RSSI()) + " dBm</td></tr>";
  }

  s += "<tr><td>Config AP</td><td>" + String(apCurrentlyOn ? "<span class='ok'>ON</span> - " + String(AP_SSID) + " @ " + WiFi.softAPIP().toString() : "<span class='ok'>Wi-Fi Connected</span>") + " <span class='hint'>(off when STA connected)</span></td></tr>";
  if (apCurrentlyOn) {
    s += "<tr><td>AP clients connected</td><td>" + String(WiFi.softAPgetStationNum()) + "</td></tr>";
  }
  s += "</table></div>";

  s += "<div class='card'><table>";
  s += "<tr><td>Bridge TCP port</td><td>" + String(TCP_PORT) + " (taptap default)</td></tr>";
  s += "<tr><td>taptap client</td><td>" + String((bridgeClient && bridgeClient.connected()) ? "<span class='ok'>Connected</span>" : "Not connected") + "</td></tr>";
  s += "<tr><td>Serial baud</td><td>" + String(RS485_BAUD) + " 8N1</td></tr>";
  s += "<tr><td>Bytes relayed</td><td>" + formatBytes(totalBytes) + "</td></tr>";
  s += "<tr><td>Last byte seen</td><td>" + (lastByteMillis == 0 ? String("never") : String(sinceLast) + "s ago") + "</td></tr>";
  s += "<tr><td>Uptime</td><td>" + formatUptime(uptime) + "</td></tr>";
  s += "</table></div>";

  s += "<p><a href='/wifi'>&#9881; WiFi settings</a></p>";
  s += "</body></html>";

  http.send(200, "text/html; charset=utf-8", s);
}

void handleWifiPage() {
  String s = htmlHeader("WiFi Settings");
  s += "<h2>WiFi Settings</h2>";

  s += "<div class='card'>";
  s += "<form action='/save' method='POST'>";
  s += "<label>Network name (SSID)</label>";
  s += "<input type='text' id='ssidInput' name='ssid' autocomplete='off' value='" + savedSSID + "' placeholder='Type SSID, incl. hidden networks'>";
  s += "<button type='button' onclick='scanNetworks()'>Scan nearby (visible networks only)</button>";
  s += "<div id='scanStatus'></div>";
  s += "<ul id='ssidResults'></ul>";
  s += "<p class='hint'>Hidden home networks (SSID broadcast off) never appear by name in a scan - that's a router privacy setting, not a bug. Just type the exact name here. Networks broadcasting on multiple channels/bands are merged into one entry, showing the strongest signal.</p>";
  s += "<label>Password</label>";
  s += "<input type='password' name='pass' placeholder='WiFi password'>";
  s += "<br><br>";
  s += "<button type='submit'>Save &amp; Reboot</button>";
  s += "</form>";
  s += "</div>";

  s += "<div class='card'>";
  s += "<form action='/reset' method='POST'>";
  s += "<button type='submit' style='background:#c0392b'>Clear saved WiFi & reboot</button>";
  s += "</form>";
  s += "</div>";

  s += "<p><a href='/'>&#8592; Back to status</a></p>";

  s += "<script>";
  s += "function sigBars(rssi) {";
  s += "  let level = 1;";
  s += "  if (rssi >= -55) level = 4;";
  s += "  else if (rssi >= -65) level = 3;";
  s += "  else if (rssi >= -75) level = 2;";
  s += "  else level = 1;";
  s += "  let html = '<span class=\"sigbars\">';";
  s += "  for (let i = 1; i <= 4; i++) {";
  s += "    html += '<span' + (i <= level ? ' class=\"on\"' : '') + '></span>';";
  s += "  }";
  s += "  html += '</span>';";
  s += "  return html;";
  s += "}";
  s += "async function scanNetworks() {";
  s += "  const statusEl = document.getElementById('scanStatus');";
  s += "  const listEl = document.getElementById('ssidResults');";
  s += "  listEl.innerHTML = '';";
  s += "  statusEl.textContent = 'Scanning...';";
  s += "  try {";
  s += "    const r = await fetch('/scan');";
  s += "    if (!r.ok) throw new Error('HTTP ' + r.status);";
  s += "    const list = await r.json();";
  s += "    if (!Array.isArray(list) || list.length === 0) {";
  s += "      statusEl.textContent = 'No networks found. Try again, or move closer to your router.';";
  s += "      return;";
  s += "    }";
  s += "    statusEl.textContent = 'Found ' + list.length + ' network(s) - tap one to fill it in:';";
  s += "    list.forEach(net => {";
  s += "      if (!net.ssid || !net.ssid.length) return;";
  s += "      const li = document.createElement('li');";
  s += "      const nameSpan = document.createElement('span');";
  s += "      nameSpan.textContent = net.ssid;";
  s += "      li.appendChild(nameSpan);";
  s += "      const sigSpan = document.createElement('span');";
  s += "      sigSpan.innerHTML = sigBars(net.rssi);";
  s += "      li.appendChild(sigSpan);";
  s += "      li.onclick = () => { document.getElementById('ssidInput').value = net.ssid; };";
  s += "      listEl.appendChild(li);";
  s += "    });";
  s += "  } catch (err) {";
  s += "    statusEl.textContent = 'Scan failed: ' + err.message + '. Try again in a few seconds.';";
  s += "  }";
  s += "}";
  s += "</script></body></html>";

  http.send(200, "text/html; charset=utf-8", s);
}

void handleScan() {
  int n = WiFi.scanNetworks(false, true);

  if (n < 0) {
    http.send(200, "application/json", "[]");
    return;
  }

  // Deduplicate by SSID, keeping the strongest RSSI seen across all
  // channels/bands (handles mesh systems / dual-band APs broadcasting
  // the same name multiple times).
  const int MAX_UNIQUE = 64;
  String uniqueSSIDs[MAX_UNIQUE];
  int uniqueRSSI[MAX_UNIQUE];
  int uniqueCount = 0;

  for (int i = 0; i < n; i++) {
    String ssid = WiFi.SSID(i);
    if (ssid.length() == 0) continue;
    int rssi = WiFi.RSSI(i);

    int existing = -1;
    for (int j = 0; j < uniqueCount; j++) {
      if (uniqueSSIDs[j] == ssid) { existing = j; break; }
    }

    if (existing >= 0) {
      if (rssi > uniqueRSSI[existing]) uniqueRSSI[existing] = rssi;
    } else if (uniqueCount < MAX_UNIQUE) {
      uniqueSSIDs[uniqueCount] = ssid;
      uniqueRSSI[uniqueCount] = rssi;
      uniqueCount++;
    }
  }

  // Sort by RSSI descending (strongest signal first) - simple insertion
  // sort, fine for the small list sizes involved here.
  for (int i = 1; i < uniqueCount; i++) {
    String ssidKey = uniqueSSIDs[i];
    int rssiKey = uniqueRSSI[i];
    int j = i - 1;
    while (j >= 0 && uniqueRSSI[j] < rssiKey) {
      uniqueSSIDs[j + 1] = uniqueSSIDs[j];
      uniqueRSSI[j + 1] = uniqueRSSI[j];
      j--;
    }
    uniqueSSIDs[j + 1] = ssidKey;
    uniqueRSSI[j + 1] = rssiKey;
  }

  String json = "[";
  for (int i = 0; i < uniqueCount; i++) {
    if (i) json += ",";
    String ssid = uniqueSSIDs[i];
    ssid.replace("\\", "\\\\");
    ssid.replace("\"", "\\\"");
    json += "{\"ssid\":\"" + ssid + "\",\"rssi\":" + String(uniqueRSSI[i]) + "}";
  }
  json += "]";
  WiFi.scanDelete();
  http.send(200, "application/json; charset=utf-8", json);
}

void handleSave() {
  String ssid = http.arg("ssid");
  String pass = http.arg("pass");
  saveCredentials(ssid, pass);

  String s = htmlHeader("Saving");
  s += "<div class='card'><p>Saved. Rebooting...</p></div></body></html>";
  http.send(200, "text/html; charset=utf-8", s);
  delay(1000);
  ESP.restart();
}

void handleReset() {
  clearCredentials();

  String s = htmlHeader("Reset");
  s += "<div class='card'><p>Cleared. Rebooting into setup mode...</p></div></body></html>";
  http.send(200, "text/html; charset=utf-8", s);
  delay(1000);
  ESP.restart();
}

void setup() {
  bootMillis = millis();

  Serial.begin(115200);
  delay(500);
  Serial2.begin(RS485_BAUD, SERIAL_8N1, RS485_RX_PIN, RS485_TX_PIN);

  loadCredentials();
  startWiFi();

  bridgeServer.begin();
  bridgeServer.setNoDelay(true);

  http.on("/", handleRoot);
  http.on("/wifi", handleWifiPage);
  http.on("/scan", handleScan);
  http.on("/save", HTTP_POST, handleSave);
  http.on("/reset", HTTP_POST, handleReset);
  http.begin();

  Serial.println("HTTP dashboard on port 80, RS-485 bridge on port " + String(TCP_PORT));
}

void loop() {
  http.handleClient();
  manageWiFiAndAP();

  if (bridgeServer.hasClient()) {
    if (!bridgeClient || !bridgeClient.connected()) {
      if (bridgeClient) bridgeClient.stop();
      bridgeClient = bridgeServer.available();
    } else {
      WiFiClient rejected = bridgeServer.available();
      rejected.stop();
    }
  }

  if (Serial2.available()) {
    uint8_t buf[256];
    size_t n = Serial2.available();
    if (n > sizeof(buf)) n = sizeof(buf);
    n = Serial2.readBytes(buf, n);

    totalBytes += n;
    lastByteMillis = millis();

    if (bridgeClient && bridgeClient.connected()) {
      bridgeClient.write(buf, n);
    }
  }

  if (bridgeClient && bridgeClient.connected() && bridgeClient.available()) {
    uint8_t buf[256];
    size_t n = bridgeClient.available();
    if (n > sizeof(buf)) n = sizeof(buf);
    n = bridgeClient.read(buf, n);
    Serial2.write(buf, n);
  }
}