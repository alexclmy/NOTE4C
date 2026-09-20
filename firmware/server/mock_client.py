#!/usr/bin/env python3
"""
mock_client.py — 模拟 ESP32 固件与 llmserve.py 进行端到端测试

用法:
    python3 mock_client.py                          # 默认 localhost:9001
    python3 mock_client.py --server ws://<server-ip>:9001  # 指定服务器

测试流程:
    1. (可选) UDP Discovery
    2. WebSocket 连接 + hello 握手
    3. 发送 ptt_start + 模拟 PCM16 音频
    4. 发送 ptt_stop
    5. 接收并打印所有服务器响应 (ASR → LLM → TTS → summary)
"""

import asyncio
import json
import logging
import socket
import struct
import sys
import time
import wave
from pathlib import Path

import websockets

logging.basicConfig(level=logging.INFO, format="%(asctime)s [%(levelname)s] %(message)s")
logger = logging.getLogger("mock_client")

# ─── 配置 ───────────────────────────────────────────────
DEFAULT_SERVER = "ws://127.0.0.1:9001"
DISCOVERY_PORT = 8766
SIMULATED_AUDIO_DURATION_MS = 2000  # 模拟 2 秒录音
SAMPLE_RATE = 16000
CHANNELS = 1
BITS_PER_SAMPLE = 16


def generate_dummy_audio(duration_ms: int) -> bytes:
    """生成模拟的 PCM16 16kHz 单声道音频（静音 + 轻微噪音）"""
    import random
    num_samples = int(duration_ms * SAMPLE_RATE / 1000)
    # 半静音（低振幅噪音模拟语音），让 ASR 不会立即返回空
    data = struct.pack(f"<{num_samples}h", *[random.randint(-100, 100) for _ in range(num_samples)])
    return data


def sign_discovery_reply(host_id, host_name, ws_url, nonce, secret):
    """生成 HMAC-SHA256 签名"""
    import hmac
    import hashlib
    message = f"discover_reply|{host_id}|{host_name}|{ws_url}|{nonce}"
    return hmac.new(
        secret.encode("utf-8"),
        message.encode("utf-8"),
        hashlib.sha256
    ).hexdigest()


async def run_discovery() -> str | None:
    """UDP 设备发现，返回 wsUrl 或 None"""
    logger.info("📡 发送 UDP discover_host...")
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sock.setsockopt(socket.SOL_SOCKET, socket.SO_BROADCAST, 1)
    sock.settimeout(3.0)

    request = json.dumps({
        "type": "discover_host",
        "service": "vibecoding-voice",
        "deviceId": "mock-client-001",
        "nonce": "mock-nonce-1234",
    }).encode("utf-8")

    sock.sendto(request, ("255.255.255.255", DISCOVERY_PORT))

    try:
        data, addr = sock.recvfrom(1024)
        reply = json.loads(data.decode("utf-8"))
        logger.info("📡 Discovery reply from %s: %s", addr, json.dumps(reply, ensure_ascii=False))
        return reply.get("wsUrl")
    except socket.timeout:
        logger.warning("📡 Discovery 超时")
        return None
    finally:
        sock.close()


async def run_full_test(server_url: str):
    """执行完整的 PTT → ASR → LLM → TTS 测试流程"""

    results = {
        "hello_ack": False,
        "server_ready": False,
        "asr_result": None,
        "llm_chunks": 0,
        "llm_full_text": "",
        "tts_audio_received": False,
        "cli_summary": False,
        "intent_response": False,
        "intent_actions": [],
        "errors": [],
        "timing": {},
    }

    t0 = time.time()

    try:
        logger.info(f"🔌 连接 WebSocket: {server_url}")
        async with websockets.connect(server_url) as ws:

            # ── Step 1: Hello 握手 ──
            hello = json.dumps({
                "type": "hello",
                "deviceId": "mock-client-001",
                "boardType": "mock-test",
            })
            await ws.send(hello)
            logger.info("📨 发送 hello")

            # ── Step 2: 接收 hello_ack + server_ready ──
            for _ in range(5):
                msg = await asyncio.wait_for(ws.recv(), timeout=10)
                if isinstance(msg, str):
                    data = json.loads(msg)
                    msg_type = data.get("type", "")
                    logger.info(f"📥 {msg_type}: {json.dumps(data, ensure_ascii=False)[:200]}")

                    if msg_type == "hello_ack":
                        results["hello_ack"] = True
                    elif msg_type == "server_ready":
                        results["server_ready"] = True
                        break

            if not results["hello_ack"]:
                results["errors"].append("未收到 hello_ack")
                return results
            if not results["server_ready"]:
                results["errors"].append("未收到 server_ready")
                return results

            results["timing"]["hello_ms"] = int((time.time() - t0) * 1000)
            logger.info(f"✅ 握手成功 ({results['timing']['hello_ms']}ms)")

            # ── Step 3: PTT 开始 ──
            ptt_start = json.dumps({
                "type": "ptt_start",
                "deviceId": "mock-client-001",
            })
            await ws.send(ptt_start)
            logger.info("📨 发送 ptt_start")

            # ── Step 4: 发送模拟音频 ──
            audio_data = generate_dummy_audio(SIMULATED_AUDIO_DURATION_MS)
            chunk_size = 3200  # 与 llmserve CHUNK_SIZE 一致
            for i in range(0, len(audio_data), chunk_size):
                chunk = audio_data[i:i + chunk_size]
                await ws.send(chunk)
                await asyncio.sleep(0.05)  # 模拟实时发送
            logger.info(f"📨 发送音频: {len(audio_data)} bytes ({SIMULATED_AUDIO_DURATION_MS}ms)")

            # ── Step 5: PTT 停止 ──
            ptt_stop = json.dumps({
                "type": "ptt_stop",
                "duration_ms": SIMULATED_AUDIO_DURATION_MS,
            })
            await ws.send(ptt_stop)
            logger.info("📨 发送 ptt_stop")

            results["timing"]["ptt_cycle_start"] = int((time.time() - t0) * 1000)

            # ── Step 6: 接收响应 ──
            llm_done_received = False
            max_wait = 60  # 最多等 60 秒
            response_timeout = time.time() + max_wait

            while time.time() < response_timeout:
                try:
                    msg = await asyncio.wait_for(ws.recv(), timeout=3)
                except asyncio.TimeoutError:
                    break

                if isinstance(msg, bytes):
                    # TTS 音频帧: 2字节header + JSON头 + PCM数据
                    if len(msg) < 4:
                        continue
                    header_len = struct.unpack(">H", msg[:2])[0]
                    json_header = msg[2:2 + header_len]
                    try:
                        header = json.loads(json_header.decode("utf-8"))
                        if header.get("type") == "tts_audio":
                            pcm_len = len(msg) - 2 - header_len
                            results["tts_audio_received"] = True
                            logger.info(f"🎵 TTS 音频: {pcm_len} bytes PCM ({pcm_len // 2} samples)")

                            # 保存 TTS 音频到 WAV 文件
                            save_tts_wav(msg, header_len)
                    except json.JSONDecodeError:
                        pass
                    continue

                if isinstance(msg, str):
                    data = json.loads(msg)
                    msg_type = data.get("type", "")
                    truncated = json.dumps(data, ensure_ascii=False)[:200]
                    logger.info(f"📥 {msg_type}: {truncated}")

                    if msg_type == "status":
                        status = data.get("status", "")
                        if status == "recording":
                            logger.info("🎤 服务器确认: 录音中")
                        elif status == "processing":
                            logger.info("⚙️ 服务器确认: 处理中")

                    elif msg_type == "asr_interim":
                        logger.info(f"🗣️  ASR 中间结果: {data.get('text', '')}")

                    elif msg_type == "transcript_final":
                        results["asrresult"] = data.get("text", "")
                        logger.info(f"✅ ASR 最终结果: {results['asrresult']}")

                    elif msg_type == "asr_final":
                        results["asrresult"] = data.get("text", "")
                        logger.info(f"✅ ASR 最终结果: {results['asrresult']}")

                    elif msg_type == "cli_summary":
                        results["llm_chunks"] += 1
                        results["cli_summary"] = True
                        assistant_text = data.get("latestAssistantText", "")
                        results["llm_full_text"] = assistant_text
                        if data.get("done"):
                            llm_done_received = True
                            logger.info(f"✅ LLM 完成: {len(assistant_text)} 字符")

                    elif msg_type == "llm_done":
                        results["llm_full_text"] = data.get("full_text", "")
                        llm_done_received = True
                        logger.info(f"✅ LLM done: {len(results['llm_full_text'])} 字符")

                    elif msg_type == "error":
                        results["errors"].append(data.get("message", "unknown error"))
                        logger.error(f"❌ 服务器错误: {data.get('message')}")

                    elif msg_type == "pong":
                        pass  # 忽略 pong

                    elif msg_type == "intent_response":
                        results["intent_response"] = True
                        display_text = data.get("displayText", "")
                        actions = data.get("actions", [])
                        results["intent_actions"] = actions
                        results["llm_full_text"] = display_text
                        logger.info(f"🎯 Intent: displayText='{display_text}', {len(actions)} actions")
                        for action in actions:
                            logger.info(f"   → {action.get('action', 'unknown')}")
                        llm_done_received = True  # intent_response 意味着完成

            results["timing"]["total_ms"] = int((time.time() - t0) * 1000)

    except websockets.exceptions.ConnectionClosed as e:
        results["errors"].append(f"连接断开: {e}")
    except Exception as e:
        results["errors"].append(f"异常: {e}")

    return results


def save_tts_wav(frame: bytes, header_len: int):
    """将 TTS PCM 帧保存为 WAV 文件"""
    pcm_data = frame[2 + header_len:]
    if len(pcm_data) < 2:
        return

    filename = f"/tmp/mock_tts_output_{int(time.time())}.wav"
    try:
        with wave.open(filename, "wb") as wf:
            wf.setnchannels(1)
            wf.setsampwidth(2)  # 16-bit
            wf.setframerate(16000)
            wf.writeframes(pcm_data)
        logger.info(f"🎵 TTS WAV 已保存: {filename}")
    except Exception as e:
        logger.warning(f"保存 TTS WAV 失败: {e}")


def print_test_report(results: dict):
    """打印测试报告"""
    print("\n" + "=" * 60)
    print("  端到端测试报告")
    print("=" * 60)

    checks = [
        ("Hello 握手", results["hello_ack"]),
        ("Server Ready", results["server_ready"]),
        ("ASR 识别", results["asrresult"] is not None and len(results["asrresult"]) > 0),
        ("LLM 回复", results["llm_chunks"] > 0 or len(results["llm_full_text"]) > 0),
        ("CLI Summary / Intent", results["cli_summary"] or results["intent_response"]),
        ("TTS 音频", results["tts_audio_received"]),
    ]

    all_pass = True
    for name, passed in checks:
        status = "✅ PASS" if passed else "❌ FAIL"
        if not passed:
            all_pass = False
        print(f"  {status}  {name}")

    if results["errors"]:
        print(f"\n  ⚠️  错误 ({len(results['errors'])}):")
        for err in results["errors"]:
            print(f"    - {err}")

    print(f"\n  ⏱️  延迟:")
    if "hello_ms" in results["timing"]:
        print(f"    握手: {results['timing']['hello_ms']}ms")
    if "ptt_cycle_start" in results["timing"]:
        print(f"    PTT 循环开始: {results['timing']['ptt_cycle_start']}ms")
    if "total_ms" in results["timing"]:
        print(f"    总耗时: {results['timing']['total_ms']}ms")

    print("=" * 60)
    if all_pass:
        print("  🎉 全部通过！")
    else:
        print("  ❌ 部分失败，请检查上方错误信息")
    print("=" * 60 + "\n")


def main():
    server_url = DEFAULT_SERVER
    if "--server" in sys.argv:
        idx = sys.argv.index("--server")
        if idx + 1 < len(sys.argv):
            server_url = sys.argv[idx + 1]

    print(f"Mock Client — 目标服务器: {server_url}")
    print(f"模拟音频: {SIMULATED_AUDIO_DURATION_MS}ms PCM16 16kHz")
    print()

    # 尝试 UDP discovery
    try:
        import socket as _socket  # noqa: F811
        discovered_url = asyncio.run(run_discovery())
        if discovered_url:
            server_url = discovered_url
            print(f"使用发现地址: {server_url}")
        else:
            print(f"使用默认地址: {server_url}")
    except Exception as e:
        logger.warning(f"Discovery 失败: {e}，使用默认地址")
        print(f"使用默认地址: {server_url}")

    print()

    # 运行完整测试
    results = asyncio.run(run_full_test(server_url))
    print_test_report(results)

    # 退出码
    has_errors = bool(results["errors"])
    missing_core = not results["hello_ack"] or not results["server_ready"]
    sys.exit(1 if has_errors or missing_core else 0)


if __name__ == "__main__":
    main()
