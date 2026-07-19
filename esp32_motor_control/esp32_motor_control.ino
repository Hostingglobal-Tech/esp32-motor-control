// ESP32-S3 SG90 180도 각도 서보 WiFi 제어 펌웨어 — 각도 0~180도 제어
// - ESP32 자체가 HTTP 서버 (포트 80). 외부 서버 불필요.
// - 각도 명령: /api/angle?deg=90  (0~180)
// - 부팅 시 자동 스윕(0↔180도 3회)으로 서보 동작을 즉시 확인
// - 보정값(펄스폭 min/max)은 NVS 영구 저장
// 보드: ESP32-S3-DevKitC-1 (Arduino-ESP32 core 2.x / 3.x)

#include <WiFi.h>
#include <WebServer.h>
#include <Preferences.h>
#include "config.h"

WebServer server(80);
Preferences prefs;

// ---- PWM (50Hz, 14bit) ----
static const int PWM_FREQ = 50;
static const int PWM_RES  = 14;
static const int PWM_MAX  = (1 << PWM_RES) - 1;

int usMin = DEFAULT_US_MIN;
int usMax = DEFAULT_US_MAX;
int curAngle = DEFAULT_ANGLE;

#if ESP_ARDUINO_VERSION_MAJOR >= 3
void pwmInit()             { ledcAttach(SERVO_PIN, PWM_FREQ, PWM_RES); }
void pwmWriteUs(int us)    { ledcWrite(SERVO_PIN, (uint32_t)us * PWM_MAX / 20000); }
#else
void pwmInit()             { ledcSetup(0, PWM_FREQ, PWM_RES); ledcAttachPin(SERVO_PIN, 0); }
void pwmWriteUs(int us)    { ledcWrite(0, (uint32_t)us * PWM_MAX / 20000); }
#endif

void writeAngle(int deg) {
  deg = constrain(deg, 0, 180);
  curAngle = deg;
  int us = map(deg, 0, 180, usMin, usMax);
  pwmWriteUs(us);
}

// ---- 스윕 상태머신 (논블로킹 — 스윕 중에도 서버 응답) ----
bool sweeping = false;
int  sweepRepsTotal = 0, sweepRepsDone = 0, sweepDir = +1;
unsigned long sweepLastMs = 0;

void startSweep(int reps) {
  sweepRepsTotal = reps; sweepRepsDone = 0;
  sweepDir = +1; sweeping = true; sweepLastMs = millis();
  writeAngle(0);
}

void tickSweep() {
  if (!sweeping) return;
  if (millis() - sweepLastMs < 15) return;
  sweepLastMs = millis();
  int a = curAngle + 5 * sweepDir;
  if (a >= 180) { a = 180; sweepDir = -1; }
  else if (a <= 0) {
    a = 0; sweepDir = +1;
    if (++sweepRepsDone >= sweepRepsTotal) { sweeping = false; writeAngle(DEFAULT_ANGLE); return; }
  }
  writeAngle(a);
}

// ---- 보정값 NVS ----
void loadCal() {
  prefs.begin("servo", true);
  usMin = prefs.getInt("us_min", DEFAULT_US_MIN);
  usMax = prefs.getInt("us_max", DEFAULT_US_MAX);
  prefs.end();
}
void saveCal() {
  prefs.begin("servo", false);
  prefs.putInt("us_min", usMin);
  prefs.putInt("us_max", usMax);
  prefs.end();
}

// ---- 부팅 스윕 데모: 전원+서보 정상이면 팔이 크게 움직인다 ----
void bootSweep() {
  for (int r = 0; r < 3; r++) {
    for (int a = 0;   a <= 180; a += 5) { writeAngle(a); delay(20); }
    for (int a = 180; a >= 0;   a -= 5) { writeAngle(a); delay(20); }
  }
  writeAngle(DEFAULT_ANGLE);
}

// ---- HTTP ----
void sendJson(int code, const String &b) {
  server.sendHeader("Access-Control-Allow-Origin", "*");
  server.send(code, "application/json", b);
}

String statusJson() {
  String s = "{";
  s += "\"pin\":" + String(SERVO_PIN) + ",";
  s += "\"angle\":" + String(curAngle) + ",";
  s += "\"sweeping\":" + String(sweeping ? "true" : "false") + ",";
  s += "\"us_min\":" + String(usMin) + ",";
  s += "\"us_max\":" + String(usMax) + ",";
  s += "\"ip\":\"" + (WiFi.getMode() == WIFI_AP ? WiFi.softAPIP().toString() : WiFi.localIP().toString()) + "\",";
  s += "\"rssi\":" + String(WiFi.RSSI());
  s += "}";
  return s;
}
void handleStatus() { sendJson(200, statusJson()); }

void handleAngle() {
  if (!server.hasArg("deg")) { sendJson(400, "{\"ok\":false,\"error\":\"deg required (0-180)\"}"); return; }
  int deg = constrain(server.arg("deg").toInt(), 0, 180);
  sweeping = false;   // 진행 중 스윕 취소 후 지정 각도로
  writeAngle(deg);
  sendJson(200, "{\"ok\":true,\"angle\":" + String(deg) + "}");
}

// 스윕 테스트 — 논블로킹 상태머신 시작만 하고 즉시 응답
void handleSweep() {
  int reps = server.hasArg("reps") ? constrain(server.arg("reps").toInt(), 1, 10) : 2;
  startSweep(reps);
  sendJson(200, "{\"ok\":true,\"sweep\":" + String(reps) + "}");
}

void handleStop() {
  sweeping = false;   // 스윕 취소, 현재 각도 유지
  sendJson(200, "{\"ok\":true,\"angle\":" + String(curAngle) + "}");
}

void handleCal() {
  bool ch = false;
  if (server.hasArg("us_min")) { usMin = constrain(server.arg("us_min").toInt(), 300, 1500); ch = true; }
  if (server.hasArg("us_max")) { usMax = constrain(server.arg("us_max").toInt(), 1500, 2700); ch = true; }
  if (ch) { saveCal(); writeAngle(curAngle); }
  handleStatus();
}

// ---- WiFi 자격정보 NVS ----
unsigned long restartAtMs = 0;
void saveWifiCreds(const String &ssid, const String &pass) {
  prefs.begin("wifi", false);
  prefs.putString("ssid", ssid);
  prefs.putString("pass", pass);
  prefs.end();
}
void handleWifiSet() {
  String ssid = server.arg("ssid");
  String pass = server.arg("pass");
  if (ssid.length() == 0) { sendJson(400, "{\"ok\":false,\"error\":\"ssid required\"}"); return; }
  saveWifiCreds(ssid, pass);
  sendJson(200, "{\"ok\":true,\"note\":\"saved to NVS, rebooting in 1s\"}");
  restartAtMs = millis() + 1000;
}
void pollSerialProvision() {
  if (!Serial.available()) return;
  String line = Serial.readStringUntil('\n');
  line.trim();
  if (!line.startsWith("WIFI ")) return;
  String rest = line.substring(5);
  rest.trim();
  int sp = rest.indexOf(' ');
  String ssid = (sp < 0) ? rest : rest.substring(0, sp);
  String pass = (sp < 0) ? "" : rest.substring(sp + 1);
  if (ssid.length() == 0) { Serial.println("usage: WIFI <ssid> <password>"); return; }
  saveWifiCreds(ssid, pass);
  Serial.printf("WiFi saved to NVS: %s (rebooting)\n", ssid.c_str());
  delay(300);
  ESP.restart();
}

// ---- 웹 UI ----
static const char PAGE[] PROGMEM = R"HTML(<!DOCTYPE html>
<html lang="ko"><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>SG90 서보 제어</title>
<style>
body{font-family:sans-serif;max-width:420px;margin:16px auto;padding:0 12px}
button{font-size:17px;padding:12px 8px;margin:4px 0;width:100%;border-radius:8px;border:1px solid #999;background:#f4f4f4}
button:active{background:#ddd}
.row{display:flex;gap:8px}.row button{flex:1}
input[type=range]{width:100%;height:36px}
input[type=text],input[type=password]{font-size:16px;padding:10px;width:100%;box-sizing:border-box;border-radius:8px;border:1px solid #999}
#st{padding:10px;background:#eef;border-radius:8px;font-size:13px;white-space:pre-wrap}
h3{margin:14px 0 6px}
.big{font-size:28px;font-weight:700;text-align:center}
</style></head><body>
<h2>SG90 각도 서보 제어</h2>
<div id="st">상태 로딩중...</div>
<h3>각도: <span id="av" class="big">90</span>도</h3>
<input type="range" id="sl" min="0" max="180" value="90" oninput="setA(this.value)">
<div class="row">
<button onclick="setA(0)">0도</button>
<button onclick="setA(45)">45도</button>
<button onclick="setA(90)">90도</button>
<button onclick="setA(135)">135도</button>
<button onclick="setA(180)">180도</button>
</div>
<button style="background:#dfe" id="swb" onclick="sweepToggle()">스윕 테스트 (0-180 2회)</button>
<h3>WiFi 공유기 등록 (NVS 저장 후 재부팅)</h3>
<input type="text" id="wssid" placeholder="SSID (2.4GHz)">
<input type="password" id="wpass" placeholder="비밀번호">
<button onclick="fetch('/api/wifi?ssid='+encodeURIComponent(wssid.value)+'&pass='+encodeURIComponent(wpass.value)).then(refresh)">저장 후 재부팅</button>
<script>
var sweeping=false;
function setA(d){document.getElementById('av').textContent=d;document.getElementById('sl').value=d;fetch('/api/angle?deg='+d).then(refresh);}
function sweepToggle(){fetch(sweeping?'/api/stop':'/api/sweep?reps=2').then(refresh);}
async function refresh(){try{
 const r=await fetch('/api/status');const j=await r.json();
 sweeping=j.sweeping;
 document.getElementById('av').textContent=j.angle;
 document.getElementById('sl').value=j.angle;
 document.getElementById('swb').textContent=sweeping?'스윕 중지 (지금 0-180 왕복중)':'스윕 테스트 (0-180 2회)';
 document.getElementById('swb').style.background=sweeping?'#fdd':'#dfe';
 document.getElementById('st').textContent='IP: '+j.ip+' | 각도 '+j.angle+'도'+(sweeping?' | 스윕중':'')+' | RSSI '+j.rssi+' | 펄스 '+j.us_min+'~'+j.us_max+'us(GPIO'+j.pin+')';
}catch(e){document.getElementById('st').textContent='연결 끊김';}}
setInterval(refresh,1000);refresh();
</script></body></html>)HTML";
void handleRoot() { server.send(200, "text/html", PAGE); }

// ---- WiFi 연결 도우미 ----
bool waitConnect(unsigned long timeoutMs) {
  unsigned long t0 = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - t0 < timeoutMs) { delay(300); Serial.print("."); }
  Serial.println();
  return WiFi.status() == WL_CONNECTED;
}
bool connectStrongestOpenAp() {
  Serial.println("Scanning for open APs...");
  int n = WiFi.scanNetworks();
  if (n <= 0) { Serial.println("no networks"); return false; }
  int idx[16], cnt = 0;
  for (int i = 0; i < n && cnt < 16; i++)
    if (WiFi.encryptionType(i) == WIFI_AUTH_OPEN && WiFi.SSID(i).length() > 0) idx[cnt++] = i;
  if (cnt == 0) { Serial.println("no open APs"); WiFi.scanDelete(); return false; }
  for (int a = 0; a < cnt - 1; a++)
    for (int b = a + 1; b < cnt; b++)
      if (WiFi.RSSI(idx[b]) > WiFi.RSSI(idx[a])) { int t = idx[a]; idx[a] = idx[b]; idx[b] = t; }
  int tries = (cnt < 5) ? cnt : 5;
  for (int a = 0; a < tries; a++) {
    String ssid = WiFi.SSID(idx[a]);
    Serial.printf("Open AP try %d/%d: %s (%d dBm)\n", a + 1, tries, ssid.c_str(), WiFi.RSSI(idx[a]));
    WiFi.begin(ssid.c_str());
    if (waitConnect(10000)) { WiFi.scanDelete(); return true; }
    WiFi.disconnect(true); delay(200);
  }
  WiFi.scanDelete();
  return false;
}

void setup() {
  Serial.begin(115200);
  pwmInit();
  loadCal();
  writeAngle(DEFAULT_ANGLE);
  delay(300);

  // 부팅 스윕 — WiFi 연결 전에 서보를 크게 흔들어 동작 즉시 확인
  Serial.println("\n=== SG90 부팅 스윕 (0<->180도 3회) ===");
  bootSweep();
  Serial.println("=== 스윕 완료 ===");

  WiFi.mode(WIFI_STA);
  bool connected = false;
  prefs.begin("wifi", true);
  String nvSsid = prefs.getString("ssid", "");
  String nvPass = prefs.getString("pass", "");
  prefs.end();
  if (nvSsid.length() > 0) {
    Serial.printf("Trying saved AP: %s\n", nvSsid.c_str());
    WiFi.begin(nvSsid.c_str(), nvPass.c_str());
    connected = waitConnect(12000);
  }
#if AUTO_OPEN_AP
  if (!connected) connected = connectStrongestOpenAp();
#endif
  if (connected) {
    Serial.printf("\nSTA connected: SSID=%s http://%s/\n", WiFi.SSID().c_str(), WiFi.localIP().toString().c_str());
  } else {
    WiFi.mode(WIFI_AP);
    WiFi.softAP(AP_SSID, AP_PASS);
    Serial.printf("\nSTA failed -> AP mode: SSID=%s http://%s/\n", AP_SSID, WiFi.softAPIP().toString().c_str());
  }

  server.on("/", handleRoot);
  server.on("/api/status", handleStatus);
  server.on("/api/angle", handleAngle);
  server.on("/api/sweep", handleSweep);
  server.on("/api/stop", handleStop);
  server.on("/api/cal", handleCal);
  server.on("/api/wifi", handleWifiSet);
  server.onNotFound([]() { sendJson(404, "{\"ok\":false,\"error\":\"not found\"}"); });
  server.begin();
  Serial.println("HTTP server started");
}

void loop() {
  server.handleClient();
  tickSweep();
  pollSerialProvision();
  if (restartAtMs && millis() >= restartAtMs) ESP.restart();
}
