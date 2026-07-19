// GPIO14 신호 진단 — self-read(핀 신호 출력 확인) + 서보 PWM 좌/정지/우 스윕
// ESP32-S3-DevKitC-1, Arduino-ESP32 core 3.x
#define PIN 14

void setup() {
  Serial.begin(115200);
  delay(900);
  Serial.println("\n=== GPIO14 진단 시작 (self-read + PWM sweep) ===");
}

void loop() {
  // A) 디지털 토글 self-read: 출력한 레벨이 핀에서 실제로 읽히는가 = 신호가 물리적으로 나오는가
  pinMode(PIN, OUTPUT);
  digitalWrite(PIN, HIGH); delay(2); int h = digitalRead(PIN);
  digitalWrite(PIN, LOW);  delay(2); int l = digitalRead(PIN);
  Serial.printf("[A] digital self-read: HIGH->%d LOW->%d  %s\n",
                h, l, (h == 1 && l == 0) ? "PIN_OK(신호 정상 출력)" : "PIN_BAD(핀 이상/단락 의심)");

  // B) 서보 PWM(50Hz) 좌->정지->우 스윕: 서보가 물리적으로 반응하는지
  ledcAttach(PIN, 50, 14);
  int us[3]   = {1000, 1500, 2000};
  const char* nm[3] = {"LEFT(1000us)", "STOP(1500us)", "RIGHT(2000us)"};
  for (int i = 0; i < 3; i++) {
    ledcWrite(PIN, (uint32_t)us[i] * 16383 / 20000);
    Serial.printf("    [B] PWM %s 출력중...\n", nm[i]);
    delay(1500);
  }
  ledcDetach(PIN);
  Serial.println("--- 1사이클 완료, 반복 ---");
  delay(400);
}
