#pragma once

// ===== WiFi 설정 =====
// 비밀번호는 소스코드에 넣지 않는다. 공유기 자격정보는 기기 NVS 에 1회 저장:
//   방법 A: 시리얼 모니터(115200)에 "WIFI <ssid> <password>" 입력
//   방법 B: AP 폴백 모드(ESP32-MOTOR 접속) 웹페이지의 WiFi 설정 폼
// 부팅 시 NVS 공유기 → 공개 AP 스캔 → AP 폴백 순으로 자동 연결.

// 공개 AP 자동 연결 사용 여부 (1=사용). 신호 순으로 최대 5곳 시도.
#define AUTO_OPEN_AP 1

// WiFi 연결 실패 시 ESP32 가 직접 AP 를 띄움 (폰에서 이 SSID 로 접속 → http://192.168.4.1)
#define AP_SSID     "ESP32-MOTOR"
#define AP_PASS     "motor1234"

// ===== 하드웨어 =====
// SG90 등 180도 각도 서보 (연속회전 아님) — 각도 0~180도 제어.
// 신호선을 ESP32-S3-DevKitC-1 의 J1 헤더 끝 [GPIO14][5V][GND] 3핀에 직결
// (서보 커넥터 신호-전원-접지 순과 일치, 중앙이 5V). 서보 1개는 USB 5V 로 구동.
// GPIO14 는 strapping(0,3,45,46)·USB(19,20)·UART0(43,44)·SPI Flash(26-32) 어디와도
// 안 겹치는 범용 핀 (FSPIWP 는 외부 옥탈 SPI 전용 대체기능일 뿐, 기본 GPIO 자유). 안전.
#define SERVO_PIN 14

// ===== SG90 각도 서보 펄스폭 (µs) =====
// SG90 표준: 500µs(0도) ~ 2400µs(180도). 개체 편차 시 /api/cal 로 트림.
// (연속회전 FS90R 은 1000~2000µs 였으나, SG90 은 더 넓은 범위라야 0~180도 풀스윙)
#define DEFAULT_US_MIN  500    // 0도
#define DEFAULT_US_MAX  2400   // 180도

// ===== 기본/시작 각도 =====
#define DEFAULT_ANGLE   90     // 부팅 후 중립 위치
