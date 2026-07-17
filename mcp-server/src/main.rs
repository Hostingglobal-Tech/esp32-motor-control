// esp32-motor-mcp — ESP32 모터 제어 MCP 서버 (stdio, JSON-RPC 2.0)
// ESP32 펌웨어의 HTTP API 를 MCP tools 로 노출. 외부 SDK 의존 없음.
// 접속 대상: env ESP32_MOTOR_URL (예: http://192.168.1.50) — 각 tool 의 host 인자로 호출별 재정의 가능.

use serde_json::{json, Value};
use std::io::{self, BufRead, Read, Write};
use std::net::TcpStream;
use std::time::Duration;

const SERVER_NAME: &str = "esp32-motor";
const SERVER_VERSION: &str = "0.1.0";

fn base_url(args: &Value) -> Result<String, String> {
    if let Some(h) = args.get("host").and_then(|v| v.as_str()) {
        let h = h.trim();
        if !h.is_empty() {
            return Ok(if h.starts_with("http://") { h.to_string() } else { format!("http://{}", h) });
        }
    }
    std::env::var("ESP32_MOTOR_URL")
        .map_err(|_| "ESP32_MOTOR_URL env not set and no host argument given".to_string())
}

fn http_get(base: &str, path: &str) -> Result<String, String> {
    let stripped = base
        .strip_prefix("http://")
        .ok_or_else(|| format!("only http:// supported, got: {}", base))?;
    let hostport = stripped.trim_end_matches('/');
    let (host, port) = match hostport.rsplit_once(':') {
        Some((h, p)) => (h.to_string(), p.parse::<u16>().map_err(|_| format!("bad port in {}", hostport))?),
        None => (hostport.to_string(), 80u16),
    };
    let mut stream = TcpStream::connect((host.as_str(), port))
        .map_err(|e| format!("connect {}:{} failed: {}", host, port, e))?;
    stream.set_read_timeout(Some(Duration::from_secs(10))).ok();
    stream.set_write_timeout(Some(Duration::from_secs(10))).ok();
    let req = format!(
        "GET {} HTTP/1.1\r\nHost: {}\r\nConnection: close\r\n\r\n",
        path, host
    );
    stream.write_all(req.as_bytes()).map_err(|e| e.to_string())?;
    let mut raw = Vec::new();
    stream.read_to_end(&mut raw).map_err(|e| e.to_string())?;
    let text = String::from_utf8_lossy(&raw);
    let (headers, body) = match text.split_once("\r\n\r\n") {
        Some((h, b)) => (h.to_string(), b.to_string()),
        None => return Err(format!("malformed response from {}{}", base, path)),
    };
    let body = if headers.to_ascii_lowercase().contains("transfer-encoding: chunked") {
        decode_chunked(&body)
    } else {
        body
    };
    let body = body.trim().to_string();
    if body.is_empty() {
        return Err(format!("empty response from {}{}", base, path));
    }
    Ok(body)
}

fn decode_chunked(s: &str) -> String {
    let mut out = String::new();
    let mut rest = s;
    loop {
        let Some((size_line, after)) = rest.split_once("\r\n") else { break };
        let Ok(size) = usize::from_str_radix(size_line.trim(), 16) else { break };
        if size == 0 {
            break;
        }
        // chunk 크기는 바이트 단위 — ASCII JSON 응답이라 char 경계 안전
        let chunk: String = after.chars().take(size).collect();
        out.push_str(&chunk);
        rest = &after[chunk.len()..];
        rest = rest.strip_prefix("\r\n").unwrap_or(rest);
    }
    out
}

// 쿼리값 최소 percent-encode (시퀀스 문자 L R W S @ , . - 숫자는 그대로 안전)
fn enc(s: &str) -> String {
    let mut out = String::new();
    for c in s.chars() {
        match c {
            'A'..='Z' | 'a'..='z' | '0'..='9' | ',' | '.' | '-' | '@' | '_' => out.push(c),
            _ => out.push_str(&format!("%{:02X}", c as u32)),
        }
    }
    out
}

fn tool_defs() -> Value {
    let host_prop = json!({"type": "string", "description": "ESP32 주소 재정의 (예: 192.168.1.50). 생략 시 env ESP32_MOTOR_URL 사용"});
    json!([
        {
            "name": "motor_status",
            "description": "모터/ESP32 상태 조회 (회전중 여부, 큐, 보정값, IP, WiFi 신호)",
            "inputSchema": {"type": "object", "properties": {"host": host_prop}}
        },
        {
            "name": "motor_turn",
            "description": "모터를 지정 방향으로 n바퀴 회전. 소수 바퀴 가능 (0.25 = 90도)",
            "inputSchema": {"type": "object", "properties": {
                "dir": {"type": "string", "enum": ["left", "right"], "description": "회전 방향"},
                "turns": {"type": "number", "description": "바퀴 수 (기본 1, 소수 가능)"},
                "speed": {"type": "integer", "minimum": 1, "maximum": 100, "description": "속도 % (기본 100)"},
                "host": host_prop
            }, "required": ["dir"]}
        },
        {
            "name": "motor_seq",
            "description": "복합 동작 시퀀스 실행. 어떤 응용 명령이든 토큰 조합으로 표현: L<바퀴>=왼쪽, R<바퀴>=오른쪽, W<초>=대기, S<±속도%>=연속회전(음수=왼쪽, stop까지), S<±속도%>X<초>=시간지정 회전, @<속도%>=스텝 속도. 예: '왼쪽 2바퀴 돌고 1초 쉬고 오른쪽 1바퀴'=L2,W1,R1 / '좌우 반바퀴 3번 왕복'=L0.5,R0.5 + repeat=3 / '천천히 오른쪽 90도'=R0.25@20 / '왼쪽 5초 돌고 오른쪽 3바퀴'=S-100X5,W0.5,R3",
            "inputSchema": {"type": "object", "properties": {
                "seq": {"type": "string", "description": "시퀀스 문자열 (예: L2,W1,R1)"},
                "repeat": {"type": "integer", "minimum": 1, "maximum": 1000, "description": "전체 반복 횟수 (기본 1)"},
                "host": host_prop
            }, "required": ["seq"]}
        },
        {
            "name": "motor_stop",
            "description": "모터 즉시 정지 + 남은 시퀀스 취소",
            "inputSchema": {"type": "object", "properties": {"host": host_prop}}
        },
        {
            "name": "motor_calibrate",
            "description": "보정값 설정 (NVS 영구저장). spr_left/spr_right=1바퀴 소요초, stop_us=정지 펄스 트림(1500 근처, 정지시 미세회전 보정)",
            "inputSchema": {"type": "object", "properties": {
                "spr_left": {"type": "number", "description": "왼쪽 1바퀴 소요 초"},
                "spr_right": {"type": "number", "description": "오른쪽 1바퀴 소요 초"},
                "stop_us": {"type": "integer", "description": "정지 펄스폭 µs (1200~1800)"},
                "left_us": {"type": "integer", "description": "왼쪽 최대속도 펄스폭 µs"},
                "right_us": {"type": "integer", "description": "오른쪽 최대속도 펄스폭 µs"},
                "host": host_prop
            }}
        },
        {
            "name": "motor_cal_run",
            "description": "보정 도우미: 지정 방향으로 정확히 10초 회전. 바퀴 수를 센 뒤 motor_calibrate 로 spr=10/바퀴수 설정",
            "inputSchema": {"type": "object", "properties": {
                "dir": {"type": "string", "enum": ["left", "right"], "description": "회전 방향"},
                "host": host_prop
            }, "required": ["dir"]}
        }
    ])
}

fn call_tool(name: &str, args: &Value) -> Result<String, String> {
    let base = base_url(args)?;
    match name {
        "motor_status" => http_get(&base, "/api/status"),
        "motor_stop" => http_get(&base, "/api/stop"),
        "motor_turn" => {
            let dir = args.get("dir").and_then(|v| v.as_str()).ok_or("dir required (left|right)")?;
            if dir != "left" && dir != "right" {
                return Err("dir must be left or right".into());
            }
            let turns = args.get("turns").and_then(|v| v.as_f64()).unwrap_or(1.0);
            let speed = args.get("speed").and_then(|v| v.as_i64()).unwrap_or(100).clamp(1, 100);
            http_get(&base, &format!("/api/turn?dir={}&turns={}&speed={}", dir, turns, speed))
        }
        "motor_seq" => {
            let seq = args.get("seq").and_then(|v| v.as_str()).ok_or("seq required")?;
            let repeat = args.get("repeat").and_then(|v| v.as_i64()).unwrap_or(1).clamp(1, 1000);
            http_get(&base, &format!("/api/seq?seq={}&repeat={}", enc(seq), repeat))
        }
        "motor_calibrate" => {
            let mut q = Vec::new();
            for k in ["spr_left", "spr_right", "stop_us", "left_us", "right_us"] {
                if let Some(v) = args.get(k) {
                    if v.is_number() {
                        q.push(format!("{}={}", k, v));
                    }
                }
            }
            if q.is_empty() {
                return Err("at least one calibration parameter required".into());
            }
            http_get(&base, &format!("/api/cal?{}", q.join("&")))
        }
        "motor_cal_run" => {
            let dir = args.get("dir").and_then(|v| v.as_str()).ok_or("dir required (left|right)")?;
            http_get(&base, &format!("/api/cal_run?dir={}", dir))
        }
        _ => Err(format!("unknown tool: {}", name)),
    }
}

fn rpc_result(id: &Value, result: Value) -> Value {
    json!({"jsonrpc": "2.0", "id": id, "result": result})
}

fn rpc_error(id: &Value, code: i64, msg: &str) -> Value {
    json!({"jsonrpc": "2.0", "id": id, "error": {"code": code, "message": msg}})
}

fn handle(msg: &Value) -> Option<Value> {
    let method = msg.get("method").and_then(|v| v.as_str()).unwrap_or("");
    let id = msg.get("id").cloned();
    let id = match id {
        Some(v) if !v.is_null() => v,
        _ => return None, // notification — 응답 없음
    };
    let params = msg.get("params").cloned().unwrap_or(json!({}));

    match method {
        "initialize" => {
            let proto = params
                .get("protocolVersion")
                .and_then(|v| v.as_str())
                .unwrap_or("2025-06-18");
            Some(rpc_result(&id, json!({
                "protocolVersion": proto,
                "capabilities": {"tools": {}},
                "serverInfo": {"name": SERVER_NAME, "version": SERVER_VERSION}
            })))
        }
        "ping" => Some(rpc_result(&id, json!({}))),
        "tools/list" => Some(rpc_result(&id, json!({"tools": tool_defs()}))),
        "tools/call" => {
            let name = params.get("name").and_then(|v| v.as_str()).unwrap_or("");
            let args = params.get("arguments").cloned().unwrap_or(json!({}));
            match call_tool(name, &args) {
                Ok(body) => Some(rpc_result(&id, json!({
                    "content": [{"type": "text", "text": body}],
                    "isError": false
                }))),
                Err(e) => Some(rpc_result(&id, json!({
                    "content": [{"type": "text", "text": format!("ERROR: {}", e)}],
                    "isError": true
                }))),
            }
        }
        _ => Some(rpc_error(&id, -32601, &format!("method not found: {}", method))),
    }
}

fn main() {
    let stdin = io::stdin();
    let stdout = io::stdout();
    for line in stdin.lock().lines() {
        let line = match line {
            Ok(l) => l,
            Err(_) => break,
        };
        let trimmed = line.trim();
        if trimmed.is_empty() {
            continue;
        }
        let msg: Value = match serde_json::from_str(trimmed) {
            Ok(v) => v,
            Err(e) => {
                let resp = rpc_error(&Value::Null, -32700, &format!("parse error: {}", e));
                let mut out = stdout.lock();
                let _ = writeln!(out, "{}", resp);
                let _ = out.flush();
                continue;
            }
        };
        if let Some(resp) = handle(&msg) {
            let mut out = stdout.lock();
            let _ = writeln!(out, "{}", resp);
            let _ = out.flush();
        }
    }
}
