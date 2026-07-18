// ESP32-S3 연속회전 서보 WiFi 제어 펌웨어 — 최대 3개 서보 독립 제어
// - ESP32 자체가 HTTP 서버 (포트 80). 외부 서버/MCP 불필요.
// - 시퀀스 명령: "L2,W1,R1" = 왼쪽 2바퀴 → 1초 대기 → 오른쪽 1바퀴
// - servo=0|1|2 쿼리 파라미터로 서보 선택 (생략 시 0번 = 기존 단일서보 동작과 100% 호환)
// - 보정값(1바퀴 소요시간, 정지 트림)은 서보별로 NVS 에 영구 저장
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
void pwmInit(int idx)              { ledcAttach(SERVO_PINS[idx], PWM_FREQ, PWM_RES); }
void pwmWriteUs(int idx, int us)   { ledcWrite(SERVO_PINS[idx], (uint32_t)us * PWM_MAX / 20000); }
#else
// v2.x 는 채널 번호로 attach/write — 서보 idx 를 그대로 채널 번호로 사용 (0,1,2 = 서로 다른 채널)
void pwmInit(int idx)              { ledcSetup(idx, PWM_FREQ, PWM_RES); ledcAttachPin(SERVO_PINS[idx], idx); }
void pwmWriteUs(int idx, int us)   { ledcWrite(idx, (uint32_t)us * PWM_MAX / 20000); }
#endif

// ---- 보정값 (NVS 영구 저장, 서보별) ----
// idx=0 은 기존 단일서보 시절 키 이름을 그대로 사용 — 이미 보정해둔 값이 있으면 유실되지 않는다.
struct MotorCal { int stopUs; int leftUs; int rightUs; float secPerRevLeft; float secPerRevRight; };
MotorCal cal[SERVO_COUNT];

String calKey(const char *base, int idx) {
  if (idx == 0) return String(base);           // 기존 키 이름 그대로 (하위호환)
  return String(base) + String(idx);            // stop_us1, spr_l2 등
}

void loadCal(int idx) {
  prefs.begin("motor", true);
  cal[idx].stopUs         = prefs.getInt(calKey("stop_us", idx).c_str(), DEFAULT_STOP_US);
  cal[idx].leftUs         = prefs.getInt(calKey("left_us", idx).c_str(), DEFAULT_LEFT_US);
  cal[idx].rightUs        = prefs.getInt(calKey("right_us", idx).c_str(), DEFAULT_RIGHT_US);
  cal[idx].secPerRevLeft  = prefs.getFloat(calKey("spr_l", idx).c_str(), DEFAULT_SEC_PER_REV_LEFT);
  cal[idx].secPerRevRight = prefs.getFloat(calKey("spr_r", idx).c_str(), DEFAULT_SEC_PER_REV_RIGHT);
  prefs.end();
}

void saveCal(int idx) {
  prefs.begin("motor", false);
  prefs.putInt(calKey("stop_us", idx).c_str(), cal[idx].stopUs);
  prefs.putInt(calKey("left_us", idx).c_str(), cal[idx].leftUs);
  prefs.putInt(calKey("right_us", idx).c_str(), cal[idx].rightUs);
  prefs.putFloat(calKey("spr_l", idx).c_str(), cal[idx].secPerRevLeft);
  prefs.putFloat(calKey("spr_r", idx).c_str(), cal[idx].secPerRevRight);
  prefs.end();
}

// ---- 명령 큐 상태머신 (loop 논블로킹 — 회전 중에도 서버 응답), 서보별 독립 ----
enum StepType { STEP_TURN, STEP_WAIT, STEP_SPIN };
struct Step {
  StepType type;
  int  dir;          // -1 = 왼쪽(CCW), +1 = 오른쪽(CW)
  float turns;       // STEP_TURN: 바퀴 수
  unsigned long ms;  // STEP_WAIT: 대기시간 / STEP_SPIN: 지속시간(0=무한)
  int speedPct;      // 1~100
};

void startStep(int idx, const Step &s);   // 명시 프로토타입 (Arduino 자동 프로토타입 삽입 위치 문제 회피)

static const int QUEUE_MAX = 32;
struct MotorState {
  Step queueBuf[QUEUE_MAX];
  int   queueLen = 0, queueIdx = 0;
  int   repeatTotal = 1, repeatDone = 0;
  bool  running = false;
  unsigned long stepStartMs = 0, stepDurMs = 0;
};
MotorState mstate[SERVO_COUNT];

int dirToUs(int idx, int dir, int speedPct) {
  int full = (dir < 0) ? cal[idx].leftUs : cal[idx].rightUs;
  return cal[idx].stopUs + (int)((long)(full - cal[idx].stopUs) * speedPct / 100);
}

void motorStop(int idx) { pwmWriteUs(idx, cal[idx].stopUs); }

void startStep(int idx, const Step &s) {
  MotorState &m = mstate[idx];
  m.stepStartMs = millis();
  switch (s.type) {
    case STEP_WAIT:
      motorStop(idx);
      m.stepDurMs = s.ms;
      break;
    case STEP_TURN: {
      float spr = (s.dir < 0) ? cal[idx].secPerRevLeft : cal[idx].secPerRevRight;
      // 속도 낮추면 그만큼 오래 돌아야 같은 바퀴 수 (선형 근사)
      m.stepDurMs = (unsigned long)(s.turns * spr * 1000.0f * 100 / s.speedPct);
      pwmWriteUs(idx, dirToUs(idx, s.dir, s.speedPct));
      break;
    }
    case STEP_SPIN:
      m.stepDurMs = s.ms;   // 0 = 무한 (STOP 명령까지)
      pwmWriteUs(idx, dirToUs(idx, s.dir, s.speedPct));
      break;
  }
}

void queueClearAndStop(int idx) {
  MotorState &m = mstate[idx];
  m.queueLen = m.queueIdx = 0;
  m.repeatTotal = 1; m.repeatDone = 0;
  m.running = false;
  motorStop(idx);
}

void tickQueue(int idx) {
  MotorState &m = mstate[idx];
  if (!m.running) return;
  Step &cur = m.queueBuf[m.queueIdx];
  if (cur.type == STEP_SPIN && cur.ms == 0) return;  // 무한 회전 (STOP 까지)
  if (millis() - m.stepStartMs < m.stepDurMs) return;
  m.queueIdx++;
  if (m.queueIdx >= m.queueLen) {
    m.repeatDone++;
    if (m.repeatDone >= m.repeatTotal) { queueClearAndStop(idx); return; }
    m.queueIdx = 0;   // 다음 반복 회차
  }
  startStep(idx, m.queueBuf[m.queueIdx]);
}

// ---- 시퀀스 파서 (범용 명령 언어) ----
// L<n>   왼쪽 n바퀴 (소수 가능: L0.25 = 90도)
// R<n>   오른쪽 n바퀴
// W<n>   n초 대기
// S<±pct>       연속회전 (음수=왼쪽, STOP 명령까지)
// S<±pct>X<초>  시간지정 회전 (S-50X3 = 왼쪽 50% 속도로 3초)
// @<pct> 속도 지정 (L2@50 = 왼쪽 2바퀴를 50% 속도로)
// 예: "L2,W1,R1" / "R0.5@30,W2,L0.5@30" / "S100X5,W1,S-100X5"
bool parseSeq(int idx, const String &seq, int repeat, String &err) {
  queueClearAndStop(idx);
  MotorState &m = mstate[idx];
  m.repeatTotal = constrain(repeat, 1, 1000);
  int i = 0, n = seq.length();
  while (i < n && m.queueLen < QUEUE_MAX) {
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
      default:  err = String("unknown op: ") + op; queueClearAndStop(idx); return false;
    }
    m.queueBuf[m.queueLen++] = s;
  }
  if (m.queueLen == 0) { err = "empty sequence"; return false; }
  m.queueIdx = 0;
  m.running = true;
  startStep(idx, m.queueBuf[0]);
  return true;
}

// ---- HTTP 핸들러 ----
void sendJson(int code, const String &body) {
  server.sendHeader("Access-Control-Allow-Origin", "*");
  server.send(code, "application/json", body);
}

// servo=0|1|2 쿼리 파라미터. 생략 시 0번(기존 단일서보 호출과 100% 호환).
int argServo() {
  if (!server.hasArg("servo")) return 0;
  int s = server.arg("servo").toInt();
  return constrain(s, 0, SERVO_COUNT - 1);
}

String servoStatusJson(int idx) {
  MotorState &m = mstate[idx];
  String s = "{";
  s += "\"pin\":" + String(SERVO_PINS[idx]) + ",";
  s += "\"state\":\"" + String(m.running ? "running" : "idle") + "\",";
  s += "\"queue_len\":" + String(m.queueLen) + ",";
  s += "\"queue_idx\":" + String(m.queueIdx) + ",";
  s += "\"repeat\":\"" + String(m.repeatDone) + "/" + String(m.repeatTotal) + "\",";
  s += "\"stop_us\":" + String(cal[idx].stopUs) + ",";
  s += "\"sec_per_rev_left\":" + String(cal[idx].secPerRevLeft, 3) + ",";
  s += "\"sec_per_rev_right\":" + String(cal[idx].secPerRevRight, 3);
  s += "}";
  return s;
}

void handleStatus() {
  String s = "{\"servos\":[";
  for (int i = 0; i < SERVO_COUNT; i++) {
    if (i) s += ",";
    s += servoStatusJson(i);
  }
  s += "],";
  s += "\"ip\":\"" + (WiFi.getMode() == WIFI_AP ? WiFi.softAPIP().toString() : WiFi.localIP().toString()) + "\",";
  s += "\"rssi\":" + String(WiFi.RSSI());
  s += "}";
  sendJson(200, s);
}

void handleSeq() {
  int idx = argServo();
  String seq = server.hasArg("seq") ? server.arg("seq") : server.arg("plain");
  int repeat = server.hasArg("repeat") ? server.arg("repeat").toInt() : 1;
  String err;
  if (!parseSeq(idx, seq, repeat, err)) { sendJson(400, "{\"ok\":false,\"error\":\"" + err + "\"}"); return; }
  sendJson(200, "{\"ok\":true,\"servo\":" + String(idx) + ",\"steps\":" + String(mstate[idx].queueLen) + ",\"repeat\":" + String(mstate[idx].repeatTotal) + "}");
}

void handleTurn() {
  int idx = argServo();
  String dir = server.arg("dir");
  float turns = server.hasArg("turns") ? server.arg("turns").toFloat() : 1.0f;
  int speed = server.hasArg("speed") ? constrain(server.arg("speed").toInt(), 1, 100) : 100;
  if (turns <= 0) turns = 1;
  char d = (dir == "left" || dir == "l" || dir == "ccw") ? 'L'
         : (dir == "right" || dir == "r" || dir == "cw") ? 'R' : 0;
  if (!d) { sendJson(400, "{\"ok\":false,\"error\":\"dir=left|right\"}"); return; }
  String err;
  parseSeq(idx, String(d) + String(turns, 2) + "@" + String(speed), 1, err);
  sendJson(200, "{\"ok\":true,\"servo\":" + String(idx) + "}");
}

void handleStop() {
  // servo 파라미터 생략 = 전체 서보 정지(안전 기본값). 지정 시 해당 서보만.
  if (server.hasArg("servo")) {
    queueClearAndStop(argServo());
  } else {
    for (int i = 0; i < SERVO_COUNT; i++) queueClearAndStop(i);
  }
  sendJson(200, "{\"ok\":true,\"state\":\"idle\"}");
}

void handleCal() {
  int idx = argServo();
  bool changed = false;
  if (server.hasArg("stop_us"))   { cal[idx].stopUs = constrain(server.arg("stop_us").toInt(), 1200, 1800); changed = true; }
  if (server.hasArg("left_us"))   { cal[idx].leftUs = constrain(server.arg("left_us").toInt(), 500, 2500); changed = true; }
  if (server.hasArg("right_us"))  { cal[idx].rightUs = constrain(server.arg("right_us").toInt(), 500, 2500); changed = true; }
  if (server.hasArg("spr_left"))  { cal[idx].secPerRevLeft = server.arg("spr_left").toFloat(); changed = true; }
  if (server.hasArg("spr_right")) { cal[idx].secPerRevRight = server.arg("spr_right").toFloat(); changed = true; }
  if (changed) saveCal(idx);
  handleStatus();
}

// ---- WiFi 자격정보 NVS 저장 (소스코드에 비밀번호 없음) ----
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

// 시리얼 프로비저닝: "WIFI <ssid> <password>" 한 줄 입력 → NVS 저장 → 재부팅
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

// 보정 도우미: 지정 방향으로 정확히 10초 회전 → 바퀴 수를 세서 spr = 10/바퀴수
void handleCalRun() {
  int idx = argServo();
  String dir = server.arg("dir");
  int d = (dir == "right") ? +1 : -1;
  queueClearAndStop(idx);
  MotorState &m = mstate[idx];
  Step s = {}; s.type = STEP_SPIN; s.dir = d; s.speedPct = 100; s.ms = 10000;
  m.queueBuf[0] = s; m.queueLen = 1; m.queueIdx = 0; m.running = true;
  startStep(idx, s);
  sendJson(200, "{\"ok\":true,\"servo\":" + String(idx) + ",\"note\":\"10s spin - count revolutions, then /api/cal?spr_" + String(d < 0 ? "left" : "right") + "=10/count&servo=" + String(idx) + "\"}");
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
input,select{font-size:16px;padding:10px;width:100%;box-sizing:border-box;border-radius:8px;border:1px solid #999}
#st{padding:10px;background:#eef;border-radius:8px;font-size:13px;white-space:pre-wrap;word-break:break-all}
h3{margin:14px 0 6px}
.tabs{display:flex;gap:6px;margin:8px 0}
.tabs button{width:auto;flex:1;background:#eee}
.tabs button.on{background:#88c;color:#fff;border-color:#66a}
</style></head><body>
<h2>ESP32 모터 제어</h2>
<div class="tabs" id="tabs"></div>
<div id="st">상태 로딩중...</div>
<h3>기본 동작 (선택된 서보)</h3>
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
<button style="background:#fdd" onclick="fetch('/api/stop').then(refresh)">전체 정지</button>
<h3>시퀀스 (예: L2,W1,R1 = 왼쪽2바퀴, 1초 쉬고, 오른쪽1바퀴)</h3>
<input id="seq" value="L2,W1,R1">
<button onclick="api('/api/seq?seq='+encodeURIComponent(document.getElementById('seq').value))">시퀀스 실행 (선택된 서보)</button>
<h3>WiFi 공유기 등록 (NVS 저장 후 재부팅)</h3>
<input id="wssid" placeholder="SSID (2.4GHz)">
<input id="wpass" type="password" placeholder="비밀번호">
<button onclick="fetch('/api/wifi?ssid='+encodeURIComponent(document.getElementById('wssid').value)+'&pass='+encodeURIComponent(document.getElementById('wpass').value)).then(refresh)">저장 후 재부팅</button>
<script>
var SERVO_COUNT = 1, sel = 0;   // /api/status 의 servos 개수로 자동 갱신 (config.h SERVO_COUNT 를 그대로 따름)
function buildTabs(){
  var t = document.getElementById('tabs'); t.innerHTML = '';
  for (var i=0;i<SERVO_COUNT;i++){
    var b = document.createElement('button');
    b.textContent = '서보 '+i; b.className = (i===sel?'on':'');
    b.onclick = (function(i){ return function(){ sel=i; buildTabs(); }; })(i);
    t.appendChild(b);
  }
}
async function api(u){try{await fetch(u+'&servo='+sel)}catch(e){} refresh()}
async function refresh(){try{
 const r=await fetch('/api/status');const j=await r.json();
 if (j.servos.length !== SERVO_COUNT){ SERVO_COUNT = j.servos.length; if (sel >= SERVO_COUNT) sel = 0; buildTabs(); }
 var lines = j.servos.map(function(s,i){
   return '서보'+i+'(GPIO'+s.pin+'): '+(s.state==='running'?'회전중('+(s.queue_idx+1)+'/'+s.queue_len+')':'대기')+
     ' | 1바퀴 L'+s.sec_per_rev_left+'s/R'+s.sec_per_rev_right+'s';
 });
 document.getElementById('st').textContent = 'IP: '+j.ip+' | RSSI '+j.rssi+'\n'+lines.join('\n');
}catch(e){document.getElementById('st').textContent='연결 끊김'}}
buildTabs();
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
  for (int i = 0; i < SERVO_COUNT; i++) {
    pwmInit(i);
    loadCal(i);
    motorStop(i);
  }

  WiFi.mode(WIFI_STA);
  bool connected = false;
  // 1순위: NVS 에 저장된 공유기 (시리얼 "WIFI <ssid> <pass>" 또는 웹 설정폼으로 저장)
  prefs.begin("wifi", true);
  String nvSsid = prefs.getString("ssid", "");
  String nvPass = prefs.getString("pass", "");
  prefs.end();
  if (nvSsid.length() > 0) {
    Serial.printf("Trying saved AP: %s\n", nvSsid.c_str());
    WiFi.begin(nvSsid.c_str(), nvPass.c_str());
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
  server.on("/api/wifi", handleWifiSet);
  server.onNotFound([]() { sendJson(404, "{\"ok\":false,\"error\":\"not found\"}"); });
  server.begin();
  Serial.println("HTTP server started");
}

void loop() {
  server.handleClient();
  for (int i = 0; i < SERVO_COUNT; i++) tickQueue(i);
  pollSerialProvision();
  if (restartAtMs && millis() >= restartAtMs) ESP.restart();
}
