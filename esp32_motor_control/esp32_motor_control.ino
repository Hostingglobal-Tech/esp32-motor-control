// ESP32-S3 연속회전 서보 WiFi 제어 펌웨어
// - ESP32 자체가 HTTP 서버 (포트 80). 외부 서버/MCP 불필요.
// - 시퀀스 명령: "L2,W1,R1" = 왼쪽 2바퀴 → 1초 대기 → 오른쪽 1바퀴
// - 보정값(1바퀴 소요시간, 정지 트림)은 NVS 에 영구 저장
// 보드: ESP32-S3-DevKitC-1 (Arduino-ESP32 core 2.x / 3.x 모두 지원)

#include <WiFi.h>
#include <WebServer.h>
#include <Preferences.h>
#include "config.h"

WebServer server(80);
Preferences prefs;

// ---- PWM (50Hz, 14bit) — 외부 라이브러리 의존 없음 ----
static const int PWM_FREQ = 50;
static const int PWM_RES  = 14;   // 0..16383
static const int PWM_MAX  = (1 << PWM_RES) - 1;

#if ESP_ARDUINO_VERSION_MAJOR >= 3
void pwmInit()            { ledcAttach(SERVO_PIN, PWM_FREQ, PWM_RES); }
void pwmWriteUs(int us)   { ledcWrite(SERVO_PIN, (uint32_t)us * PWM_MAX / 20000); }
#else
static const int PWM_CH = 0;
void pwmInit()            { ledcSetup(PWM_CH, PWM_FREQ, PWM_RES); ledcAttachPin(SERVO_PIN, PWM_CH); }
void pwmWriteUs(int us)   { ledcWrite(PWM_CH, (uint32_t)us * PWM_MAX / 20000); }
#endif

// ---- 보정값 (NVS 영구 저장) ----
int   stopUs         = DEFAULT_STOP_US;
int   leftUs         = DEFAULT_LEFT_US;
int   rightUs        = DEFAULT_RIGHT_US;
float secPerRevLeft  = DEFAULT_SEC_PER_REV_LEFT;
float secPerRevRight = DEFAULT_SEC_PER_REV_RIGHT;

void loadCal() {
  prefs.begin("motor", true);
  stopUs         = prefs.getInt("stop_us", DEFAULT_STOP_US);
  leftUs         = prefs.getInt("left_us", DEFAULT_LEFT_US);
  rightUs        = prefs.getInt("right_us", DEFAULT_RIGHT_US);
  secPerRevLeft  = prefs.getFloat("spr_l", DEFAULT_SEC_PER_REV_LEFT);
  secPerRevRight = prefs.getFloat("spr_r", DEFAULT_SEC_PER_REV_RIGHT);
  prefs.end();
}

void saveCal() {
  prefs.begin("motor", false);
  prefs.putInt("stop_us", stopUs);
  prefs.putInt("left_us", leftUs);
  prefs.putInt("right_us", rightUs);
  prefs.putFloat("spr_l", secPerRevLeft);
  prefs.putFloat("spr_r", secPerRevRight);
  prefs.end();
}

// ---- 명령 큐 상태머신 (loop 논블로킹 — 회전 중에도 서버 응답) ----
enum StepType { STEP_TURN, STEP_WAIT, STEP_SPIN };
struct Step {
  StepType type;
  int  dir;          // -1 = 왼쪽(CCW), +1 = 오른쪽(CW)
  float turns;       // STEP_TURN: 바퀴 수
  unsigned long ms;  // STEP_WAIT: 대기시간 / STEP_SPIN: 지속시간(0=무한)
  int speedPct;      // 1~100
};

void startStep(const Step &s);   // 명시 프로토타입 (Arduino 자동 프로토타입 삽입 위치 문제 회피)

static const int QUEUE_MAX = 32;
Step  queueBuf[QUEUE_MAX];
int   queueLen = 0, queueIdx = 0;
int   repeatTotal = 1, repeatDone = 0;   // 시퀀스 전체 반복
bool  running = false;
unsigned long stepStartMs = 0, stepDurMs = 0;

int dirToUs(int dir, int speedPct) {
  int full = (dir < 0) ? leftUs : rightUs;
  return stopUs + (int)((long)(full - stopUs) * speedPct / 100);
}

void motorStop() { pwmWriteUs(stopUs); }

void startStep(const Step &s) {
  stepStartMs = millis();
  switch (s.type) {
    case STEP_WAIT:
      motorStop();
      stepDurMs = s.ms;
      break;
    case STEP_TURN: {
      float spr = (s.dir < 0) ? secPerRevLeft : secPerRevRight;
      // 속도 낮추면 그만큼 오래 돌아야 같은 바퀴 수 (선형 근사)
      stepDurMs = (unsigned long)(s.turns * spr * 1000.0f * 100 / s.speedPct);
      pwmWriteUs(dirToUs(s.dir, s.speedPct));
      break;
    }
    case STEP_SPIN:
      stepDurMs = s.ms;   // 0 = 무한 (STOP 명령까지)
      pwmWriteUs(dirToUs(s.dir, s.speedPct));
      break;
  }
}

void queueClearAndStop() {
  queueLen = queueIdx = 0;
  repeatTotal = 1; repeatDone = 0;
  running = false;
  motorStop();
}

void tickQueue() {
  if (!running) return;
  Step &cur = queueBuf[queueIdx];
  if (cur.type == STEP_SPIN && cur.ms == 0) return;  // 무한 회전 (STOP 까지)
  if (millis() - stepStartMs < stepDurMs) return;
  queueIdx++;
  if (queueIdx >= queueLen) {
    repeatDone++;
    if (repeatDone >= repeatTotal) { queueClearAndStop(); return; }
    queueIdx = 0;   // 다음 반복 회차
  }
  startStep(queueBuf[queueIdx]);
}

// ---- 시퀀스 파서 (범용 명령 언어) ----
// L<n>   왼쪽 n바퀴 (소수 가능: L0.25 = 90도)
// R<n>   오른쪽 n바퀴
// W<n>   n초 대기
// S<±pct>       연속회전 (음수=왼쪽, STOP 명령까지)
// S<±pct>X<초>  시간지정 회전 (S-50X3 = 왼쪽 50% 속도로 3초)
// @<pct> 속도 지정 (L2@50 = 왼쪽 2바퀴를 50% 속도로)
// 예: "L2,W1,R1" / "R0.5@30,W2,L0.5@30" / "S100X5,W1,S-100X5"
bool parseSeq(const String &seq, int repeat, String &err) {
  queueClearAndStop();
  repeatTotal = constrain(repeat, 1, 1000);
  int i = 0, n = seq.length();
  while (i < n && queueLen < QUEUE_MAX) {
    while (i < n && (seq[i] == ',' || seq[i] == ' ')) i++;
    if (i >= n) break;
    char op = toupper((unsigned char)seq[i++]);
    int numStart = i;
    while (i < n && (isdigit((unsigned char)seq[i]) || seq[i] == '.' || seq[i] == '-')) i++;
    float val = seq.substring(numStart, i).toFloat();
    int speedPct = 100;
    if (i < n && seq[i] == '@') {
      i++;
      int spStart = i;
      while (i < n && isdigit((unsigned char)seq[i])) i++;
      speedPct = constrain(seq.substring(spStart, i).toInt(), 1, 100);
    }
    Step s = {};
    s.speedPct = speedPct;
    switch (op) {
      case 'L': s.type = STEP_TURN; s.dir = -1; s.turns = (val > 0) ? val : 1; break;
      case 'R': s.type = STEP_TURN; s.dir = +1; s.turns = (val > 0) ? val : 1; break;
      case 'W': s.type = STEP_WAIT; s.ms = (unsigned long)(((val > 0) ? val : 1) * 1000); break;
      case 'S': {
        s.type = STEP_SPIN; s.dir = (val < 0) ? -1 : +1;
        s.speedPct = constrain((int)fabs(val), 1, 100); s.ms = 0;
        if (i < n && toupper((unsigned char)seq[i]) == 'X') {   // 시간지정: S-50X3
          i++;
          int tStart = i;
          while (i < n && (isdigit((unsigned char)seq[i]) || seq[i] == '.')) i++;
          float sec = seq.substring(tStart, i).toFloat();
          if (sec > 0) s.ms = (unsigned long)(sec * 1000);
        }
        break;
      }
      case 'P': continue;
      default:  err = String("unknown op: ") + op; queueClearAndStop(); return false;
    }
    queueBuf[queueLen++] = s;
  }
  if (queueLen == 0) { err = "empty sequence"; return false; }
  queueIdx = 0;
  running = true;
  startStep(queueBuf[0]);
  return true;
}

// ---- HTTP 핸들러 ----
void sendJson(int code, const String &body) {
  server.sendHeader("Access-Control-Allow-Origin", "*");
  server.send(code, "application/json", body);
}

void handleStatus() {
  String s = "{";
  s += "\"state\":\"" + String(running ? "running" : "idle") + "\",";
  s += "\"queue_len\":" + String(queueLen) + ",";
  s += "\"queue_idx\":" + String(queueIdx) + ",";
  s += "\"repeat\":\"" + String(repeatDone) + "/" + String(repeatTotal) + "\",";
  s += "\"stop_us\":" + String(stopUs) + ",";
  s += "\"sec_per_rev_left\":" + String(secPerRevLeft, 3) + ",";
  s += "\"sec_per_rev_right\":" + String(secPerRevRight, 3) + ",";
  s += "\"ip\":\"" + (WiFi.getMode() == WIFI_AP ? WiFi.softAPIP().toString() : WiFi.localIP().toString()) + "\",";
  s += "\"rssi\":" + String(WiFi.RSSI());
  s += "}";
  sendJson(200, s);
}

void handleSeq() {
  String seq = server.hasArg("seq") ? server.arg("seq") : server.arg("plain");
  int repeat = server.hasArg("repeat") ? server.arg("repeat").toInt() : 1;
  String err;
  if (!parseSeq(seq, repeat, err)) { sendJson(400, "{\"ok\":false,\"error\":\"" + err + "\"}"); return; }
  sendJson(200, "{\"ok\":true,\"steps\":" + String(queueLen) + ",\"repeat\":" + String(repeatTotal) + "}");
}

void handleTurn() {
  String dir = server.arg("dir");
  float turns = server.hasArg("turns") ? server.arg("turns").toFloat() : 1.0f;
  int speed = server.hasArg("speed") ? constrain(server.arg("speed").toInt(), 1, 100) : 100;
  if (turns <= 0) turns = 1;
  char d = (dir == "left" || dir == "l" || dir == "ccw") ? 'L'
         : (dir == "right" || dir == "r" || dir == "cw") ? 'R' : 0;
  if (!d) { sendJson(400, "{\"ok\":false,\"error\":\"dir=left|right\"}"); return; }
  String err;
  parseSeq(String(d) + String(turns, 2) + "@" + String(speed), 1, err);
  sendJson(200, "{\"ok\":true}");
}

void handleStop() {
  queueClearAndStop();
  sendJson(200, "{\"ok\":true,\"state\":\"idle\"}");
}

void handleCal() {
  bool changed = false;
  if (server.hasArg("stop_us"))  { stopUs = constrain(server.arg("stop_us").toInt(), 1200, 1800); changed = true; }
  if (server.hasArg("left_us"))  { leftUs = constrain(server.arg("left_us").toInt(), 500, 2500); changed = true; }
  if (server.hasArg("right_us")) { rightUs = constrain(server.arg("right_us").toInt(), 500, 2500); changed = true; }
  if (server.hasArg("spr_left"))  { secPerRevLeft = server.arg("spr_left").toFloat(); changed = true; }
  if (server.hasArg("spr_right")) { secPerRevRight = server.arg("spr_right").toFloat(); changed = true; }
  if (changed) saveCal();
  handleStatus();
}

// 보정 도우미: 지정 방향으로 정확히 10초 회전 → 바퀴 수를 세서 spr = 10/바퀴수
void handleCalRun() {
  String dir = server.arg("dir");
  int d = (dir == "right") ? +1 : -1;
  queueClearAndStop();
  Step s = {}; s.type = STEP_SPIN; s.dir = d; s.speedPct = 100; s.ms = 10000;
  queueBuf[0] = s; queueLen = 1; queueIdx = 0; running = true;
  startStep(s);
  sendJson(200, "{\"ok\":true,\"note\":\"10s spin - count revolutions, then /api/cal?spr_" + String(d < 0 ? "left" : "right") + "=10/count\"}");
}

static const char PAGE[] PROGMEM = R"HTML(<!DOCTYPE html>
<html lang="ko"><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>ESP32 모터 제어</title>
<style>
body{font-family:sans-serif;max-width:420px;margin:16px auto;padding:0 12px}
button{font-size:17px;padding:12px 8px;margin:4px 0;width:100%;border-radius:8px;border:1px solid #999;background:#f4f4f4}
button:active{background:#ddd}
.row{display:flex;gap:8px}.row button{flex:1}
input{font-size:16px;padding:10px;width:100%;box-sizing:border-box;border-radius:8px;border:1px solid #999}
#st{padding:10px;background:#eef;border-radius:8px;font-size:14px;white-space:pre-wrap;word-break:break-all}
h3{margin:14px 0 6px}
</style></head><body>
<h2>ESP32 모터 제어</h2>
<div id="st">상태 로딩중...</div>
<h3>기본 동작</h3>
<div class="row">
<button onclick="api('/api/turn?dir=left&turns=1')">왼쪽 1바퀴</button>
<button onclick="api('/api/turn?dir=right&turns=1')">오른쪽 1바퀴</button>
</div>
<div class="row">
<button onclick="api('/api/turn?dir=left&turns=2')">왼쪽 2바퀴</button>
<button onclick="api('/api/turn?dir=right&turns=2')">오른쪽 2바퀴</button>
</div>
<div class="row">
<button onclick="api('/api/seq?seq=S-100')">왼쪽 연속</button>
<button onclick="api('/api/seq?seq=S100')">오른쪽 연속</button>
</div>
<button style="background:#fdd" onclick="api('/api/stop')">정지</button>
<h3>시퀀스 (예: L2,W1,R1 = 왼쪽2바퀴, 1초 쉬고, 오른쪽1바퀴)</h3>
<input id="seq" value="L2,W1,R1">
<button onclick="api('/api/seq?seq='+encodeURIComponent(document.getElementById('seq').value))">시퀀스 실행</button>
<script>
async function api(u){try{await fetch(u)}catch(e){} refresh()}
async function refresh(){try{
 const r=await fetch('/api/status');const j=await r.json();
 document.getElementById('st').textContent=
  '상태: '+(j.state==='running'?'회전중 ('+(j.queue_idx+1)+'/'+j.queue_len+')':'대기')+
  ' | IP: '+j.ip+' | 1바퀴: L '+j.sec_per_rev_left+'s / R '+j.sec_per_rev_right+'s';
}catch(e){document.getElementById('st').textContent='연결 끊김'}}
setInterval(refresh,1000);refresh();
</script></body></html>)HTML";

void handleRoot() { server.send(200, "text/html", PAGE); }

// ---- WiFi 연결 도우미 ----
bool waitConnect(unsigned long timeoutMs) {
  unsigned long t0 = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - t0 < timeoutMs) {
    delay(300);
    Serial.print(".");
  }
  Serial.println();
  return WiFi.status() == WL_CONNECTED;
}

// 공개(암호 없는) AP 스캔 → RSSI 내림차순 최대 5곳 시도
bool connectStrongestOpenAp() {
  Serial.println("Scanning for open APs...");
  int n = WiFi.scanNetworks();
  if (n <= 0) { Serial.println("no networks found"); return false; }

  int idx[16];
  int cnt = 0;
  for (int i = 0; i < n && cnt < 16; i++) {
    if (WiFi.encryptionType(i) == WIFI_AUTH_OPEN && WiFi.SSID(i).length() > 0) idx[cnt++] = i;
  }
  if (cnt == 0) { Serial.println("no open APs"); WiFi.scanDelete(); return false; }
  // RSSI 내림차순 정렬
  for (int a = 0; a < cnt - 1; a++)
    for (int b = a + 1; b < cnt; b++)
      if (WiFi.RSSI(idx[b]) > WiFi.RSSI(idx[a])) { int t = idx[a]; idx[a] = idx[b]; idx[b] = t; }

  int tries = (cnt < 5) ? cnt : 5;
  for (int a = 0; a < tries; a++) {
    String ssid = WiFi.SSID(idx[a]);
    Serial.printf("Open AP try %d/%d: %s (%d dBm)\n", a + 1, tries, ssid.c_str(), WiFi.RSSI(idx[a]));
    WiFi.begin(ssid.c_str());
    if (waitConnect(10000)) { WiFi.scanDelete(); return true; }
    WiFi.disconnect(true);
    delay(200);
  }
  WiFi.scanDelete();
  return false;
}

void setup() {
  Serial.begin(115200);
  pwmInit();
  loadCal();
  motorStop();

  WiFi.mode(WIFI_STA);
  bool connected = false;
  // 1순위: config.h 에 지정된 공유기 (설정된 경우만)
  if (strlen(WIFI_SSID) > 0) {
    Serial.printf("Trying configured AP: %s\n", WIFI_SSID);
    WiFi.begin(WIFI_SSID, WIFI_PASS);
    connected = waitConnect(12000);
  }
  // 2순위: 공개 AP 스캔 → 신호 센 순으로 자동 연결
#if AUTO_OPEN_AP
  if (!connected) connected = connectStrongestOpenAp();
#endif
  if (connected) {
    Serial.printf("\nSTA connected: SSID=%s http://%s/\n",
                  WiFi.SSID().c_str(), WiFi.localIP().toString().c_str());
  } else {
    WiFi.mode(WIFI_AP);
    WiFi.softAP(AP_SSID, AP_PASS);
    Serial.printf("\nSTA failed -> AP mode: SSID=%s PASS=%s http://%s/\n",
                  AP_SSID, AP_PASS, WiFi.softAPIP().toString().c_str());
  }

  server.on("/", handleRoot);
  server.on("/api/status", handleStatus);
  server.on("/api/seq", handleSeq);
  server.on("/api/turn", handleTurn);
  server.on("/api/stop", handleStop);
  server.on("/api/cal", handleCal);
  server.on("/api/cal_run", handleCalRun);
  server.onNotFound([]() { sendJson(404, "{\"ok\":false,\"error\":\"not found\"}"); });
  server.begin();
  Serial.println("HTTP server started");
}

void loop() {
  server.handleClient();
  tickQueue();
}
