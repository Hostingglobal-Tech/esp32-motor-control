#pragma once

// ===== WiFi 설정 =====
// 기본 동작: 공개(암호 없는) AP 를 스캔해 신호 가장 센 곳에 자동 연결.
// 특정 공유기를 쓰려면 아래 두 값을 채우면 그쪽을 1순위로 시도한다 (2.4GHz 만 지원).
#define WIFI_SSID   ""
#define WIFI_PASS   ""

// 공개 AP 자동 연결 사용 여부 (1=사용). 신호 순으로 최대 5곳 시도.
#define AUTO_OPEN_AP 1

// WiFi 연결 실패 시 ESP32 가 직접 AP 를 띄움 (폰에서 이 SSID 로 접속 → http://192.168.4.1)
#define AP_SSID     "ESP32-MOTOR"
#define AP_PASS     "motor1234"

// ===== 하드웨어 =====
// 연속회전 서보 (FS90R 등) 신호선 GPIO
#define SERVO_PIN   4

// ===== 서보 펄스폭 (µs) =====
// 1500 = 정지, 1500 미만 = 한 방향, 초과 = 반대 방향.
// 정지 상태에서 미세하게 도는 개체는 /api/cal?stop_us=1480 식으로 트림.
#define DEFAULT_STOP_US   1500
#define DEFAULT_LEFT_US   1000   // 반시계(왼쪽) 최대 속도
#define DEFAULT_RIGHT_US  2000   // 시계(오른쪽) 최대 속도

// ===== 1바퀴 도는 데 걸리는 시간 (초) — 실측 보정값 =====
// FS90R 5V 최대속도 기준 약 0.55~0.65s. 실측 후 /api/cal 로 갱신 (NVS 영구저장).
#define DEFAULT_SEC_PER_REV_LEFT   0.60f
#define DEFAULT_SEC_PER_REV_RIGHT  0.60f
