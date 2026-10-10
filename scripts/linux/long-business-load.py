#!/usr/bin/env python3
"""Long-running production smoke/load test for login, mail and WebSocket chat.

The script creates its own uniquely-prefixed accounts, so it does not need an
existing administrator credential and does not print passwords or bearer tokens.
It intentionally uses modest concurrency because the current server has 2 vCPUs.
"""

import argparse
import base64
import hashlib
import http.client
import json
import os
import random
import secrets
import socket
import struct
import threading
import time
from collections import defaultdict
from pathlib import Path


class Stats:
    def __init__(self):
        self.lock = threading.Lock()
        self.counts = defaultdict(int)
        self.latencies = defaultdict(list)

    def record(self, route, status, elapsed_ms):
        with self.lock:
            self.counts[f"{route}.status_{status}"] += 1
            self.latencies[route].append(elapsed_ms)

    def error(self, route, kind):
        with self.lock:
            self.counts[f"{route}.error_{kind}"] += 1

    def report(self):
        with self.lock:
            counts = dict(sorted(self.counts.items()))
            samples = {key: list(value) for key, value in self.latencies.items()}
        lines = []
        for route, values in sorted(samples.items()):
            values.sort()
            lines.append(
                f"{route} count={len(values)} "
                f"p50={percentile(values, .50):.2f}ms "
                f"p95={percentile(values, .95):.2f}ms "
                f"p99={percentile(values, .99):.2f}ms "
                f"max={values[-1]:.2f}ms"
            )
        lines.append("counts=" + json.dumps(counts, ensure_ascii=True, sort_keys=True))
        return "\n".join(lines)


def percentile(values, fraction):
    if not values:
        return 0.0
    return values[min(len(values) - 1, int((len(values) - 1) * fraction))]


def http_json(method, path, payload=None, token=None):
    started = time.perf_counter()
    connection = http.client.HTTPConnection("127.0.0.1", 80, timeout=15)
    try:
        headers = {"Accept": "application/json", "Connection": "keep-alive"}
        body = None
        if payload is not None:
            body = json.dumps(payload, separators=(",", ":")).encode()
            headers["Content-Type"] = "application/json"
        if token:
            headers["Authorization"] = "Bearer " + token
        connection.request(method, path, body=body, headers=headers)
        response = connection.getresponse()
        raw = response.read()
        elapsed_ms = (time.perf_counter() - started) * 1000
        data = json.loads(raw.decode() or "{}")
        return response.status, data, elapsed_ms
    finally:
        connection.close()


def register_accounts(base_prefix, count, password, stats):
    accounts = []
    for index in range(count):
        login_name = f"{base_prefix}_{index:02d}"
        status, data, elapsed = http_json(
            "POST",
            "/auth/register",
            {
                "login_name": login_name,
                "display_name": f"LoadTest_{index:02d}",
                "password": password,
                "register_source": "long_business_load",
                "platform_channel": "load_test",
            },
        )
        stats.record("register", status, elapsed)
        if status not in (200, 201):
            raise RuntimeError(f"test account registration failed: status={status}")
        value = data.get("data", {})
        account = value.get("account", {})
        player = value.get("player", {})
        session = value.get("session", {})
        if not account.get("login_name") or not player.get("player_id") or not session.get("token"):
            raise RuntimeError("test account response did not contain expected identity fields")
        accounts.append(
            {
                "login_name": login_name,
                "password": password,
                "player_id": player["player_id"],
                "token": session["token"],
            }
        )
    return accounts


def login_phase(accounts, duration, workers, stats):
    stop = time.monotonic() + duration

    def worker(worker_index):
        index = worker_index
        while time.monotonic() < stop:
            account = accounts[index % len(accounts)]
            index += workers
            try:
                status, _, elapsed = http_json(
                    "POST",
                    "/auth/login",
                    {"login_name": account["login_name"], "password": account["password"], "single_session": True},
                )
                stats.record("login", status, elapsed)
            except Exception as error:
                stats.error("login", type(error).__name__)

    threads = [threading.Thread(target=worker, args=(i,), daemon=True) for i in range(workers)]
    for thread in threads:
        thread.start()
    for thread in threads:
        thread.join()


def refresh_tokens(accounts, stats):
    """获取一组仍然有效的 bearer token，供后续邮件和聊天阶段使用。"""
    for account in accounts:
        status, data, elapsed = http_json(
            "POST",
            "/auth/login",
            {"login_name": account["login_name"], "password": account["password"], "single_session": False},
        )
        stats.record("login_refresh", status, elapsed)
        token = data.get("data", {}).get("session", {}).get("token", "")
        if status != 200 or not token:
            raise RuntimeError("could not refresh a test session token")
        account["token"] = token


def mail_phase(accounts, duration, workers, stats):
    stop = time.monotonic() + duration

    def worker(worker_index):
        sender = accounts[worker_index % len(accounts)]
        receiver = accounts[(worker_index + 1) % len(accounts)]
        sequence = 0
        while time.monotonic() < stop:
            started = time.monotonic()
            try:
                status, _, elapsed = http_json(
                    "POST",
                    "/mail/send-player",
                    {
                        "to_player_id": receiver["player_id"],
                        "title": f"长期压测邮件 {worker_index}",
                        "body": f"load_test_{worker_index}_{sequence}",
                        "expires_in_seconds": 3600,
                    },
                    sender["token"],
                )
                stats.record("mail_send", status, elapsed)
                sequence += 1
                if sequence % 10 == 0:
                    read_status, _, read_elapsed = http_json(
                        "GET", "/mail?limit=20&offset=0", token=receiver["token"])
                    stats.record("mail_read", read_status, read_elapsed)
            except Exception as error:
                stats.error("mail", type(error).__name__)
            # Limit write pressure so the long run measures stability, not a deliberate DB flood.
            remaining = 0.20 - (time.monotonic() - started)
            if remaining > 0:
                time.sleep(remaining)

    threads = [threading.Thread(target=worker, args=(i,), daemon=True) for i in range(workers)]
    for thread in threads:
        thread.start()
    for thread in threads:
        thread.join()


class WebSocket:
    def __init__(self):
        self.socket = socket.create_connection(("127.0.0.1", 80), timeout=10)
        self.socket.settimeout(2)
        key = base64.b64encode(secrets.token_bytes(16)).decode()
        self.socket.sendall(
            (
                "GET /ws HTTP/1.1\r\n"
                "Host: 127.0.0.1\r\n"
                "Upgrade: websocket\r\n"
                "Connection: Upgrade\r\n"
                f"Sec-WebSocket-Key: {key}\r\n"
                "Sec-WebSocket-Version: 13\r\n\r\n"
            ).encode()
        )
        response = self._read_until(b"\r\n\r\n")
        if b" 101 " not in response:
            raise RuntimeError("WebSocket handshake failed")

    def _read_exact(self, count):
        data = b""
        while len(data) < count:
            chunk = self.socket.recv(count - len(data))
            if not chunk:
                raise ConnectionError("WebSocket closed")
            data += chunk
        return data

    def _read_until(self, marker):
        data = b""
        while marker not in data:
            data += self.socket.recv(4096)
            if len(data) > 65536:
                raise RuntimeError("WebSocket handshake response too large")
        return data

    def send_json(self, value):
        payload = json.dumps(value, separators=(",", ":")).encode()
        mask = secrets.token_bytes(4)
        masked = bytes(byte ^ mask[index % 4] for index, byte in enumerate(payload))
        length = len(payload)
        if length < 126:
            header = bytes([0x81, 0x80 | length])
        elif length < 65536:
            header = bytes([0x81, 0x80 | 126]) + struct.pack("!H", length)
        else:
            header = bytes([0x81, 0x80 | 127]) + struct.pack("!Q", length)
        self.socket.sendall(header + mask + masked)

    def receive_text(self):
        first, second = self._read_exact(2)
        opcode = first & 0x0F
        length = second & 0x7F
        if length == 126:
            length = struct.unpack("!H", self._read_exact(2))[0]
        elif length == 127:
            length = struct.unpack("!Q", self._read_exact(8))[0]
        masked = second & 0x80
        mask = self._read_exact(4) if masked else b""
        payload = self._read_exact(length)
        if masked:
            payload = bytes(byte ^ mask[index % 4] for index, byte in enumerate(payload))
        if opcode == 9:
            self.send_control(10, payload)
            return self.receive_text()
        if opcode == 8:
            raise ConnectionError("WebSocket close frame")
        return opcode, payload

    def send_control(self, opcode, payload=b""):
        mask = secrets.token_bytes(4)
        masked = bytes(byte ^ mask[index % 4] for index, byte in enumerate(payload))
        self.socket.sendall(bytes([0x80 | opcode, 0x80 | len(payload)]) + mask + masked)

    def close(self):
        try:
            self.socket.close()
        except OSError:
            pass


def chat_client(account, duration, stats, client_index):
    try:
        websocket = WebSocket()
        websocket.send_json({"type": "auth", "token": account["token"]})
        opcode, payload = websocket.receive_text()
        if opcode != 1 or not json.loads(payload).get("ok"):
            raise RuntimeError("WebSocket auth failed")
        websocket.send_json({"type": "join", "channel_kind": "lobby", "scope_id": "global"})
        opcode, payload = websocket.receive_text()
        if opcode != 1 or not json.loads(payload).get("ok"):
            raise RuntimeError("WebSocket join failed")
        stop = time.monotonic() + duration
        sequence = 0
        while time.monotonic() < stop:
            started = time.perf_counter()
            websocket.send_json(
                {
                    "type": "say",
                    "message_kind": "text",
                    "text": f"long_load_chat_{client_index}_{sequence}",
                }
            )
            stats.record("chat_send", 200, (time.perf_counter() - started) * 1000)
            sequence += 1
            # Drain at least one broadcast frame. Other clients may produce frames too.
            try:
                websocket.receive_text()
            except socket.timeout:
                stats.error("chat_receive", "timeout")
            time.sleep(0.20)
        websocket.close()
    except Exception as error:
        stats.error("chat", type(error).__name__)


def chat_phase(accounts, duration, clients, stats):
    threads = [
        threading.Thread(target=chat_client, args=(accounts[i % len(accounts)], duration, stats, i), daemon=True)
        for i in range(clients)
    ]
    for thread in threads:
        thread.start()
    for thread in threads:
        thread.join()


def snapshot(path):
    path.write_text(
        "# metrics\n"
        + os.popen("curl -sS http://127.0.0.1:8080/metrics").read()
        + "\n# processes\n"
        + os.popen("ps -eo pid,comm,%cpu,%mem,rss,etime,args | grep -E 'hello_service|hello_chat_gateway|hello_task_worker' | grep -v grep || true").read()
        + "\n# memory\n"
        + os.popen("free -h").read()
        + "\n# disk\n"
        + os.popen("df -h /").read(),
        encoding="utf-8",
    )


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--duration", type=int, default=180)
    parser.add_argument("--accounts", type=int, default=8)
    parser.add_argument("--login-workers", type=int, default=2)
    parser.add_argument("--mail-workers", type=int, default=2)
    parser.add_argument("--chat-clients", type=int, default=4)
    parser.add_argument("--output", default="/tmp/ue-game-long-load")
    args = parser.parse_args()

    output = Path(args.output + "-" + time.strftime("%Y%m%d-%H%M%S"))
    output.mkdir(parents=True, exist_ok=True)
    password = "LoadTest_20261002!A"
    prefix = "load_" + str(int(time.time()))
    stats = Stats()
    accounts = register_accounts(prefix, args.accounts, password, stats)
    (output / "test_identity.txt").write_text(
        "accounts=" + str(len(accounts)) + "\nplayer_ids=" + ",".join(a["player_id"] for a in accounts) + "\n",
        encoding="utf-8",
    )
    snapshot(output / "before.txt")

    phases = [
        ("login", lambda: login_phase(accounts, args.duration, args.login_workers, stats)),
        ("mail", lambda: mail_phase(accounts, args.duration, args.mail_workers, stats)),
        ("chat", lambda: chat_phase(accounts, args.duration, args.chat_clients, stats)),
    ]
    with (output / "report.txt").open("w", encoding="utf-8") as report:
        report.write(f"duration_per_phase={args.duration}\n")
        report.write(f"accounts={args.accounts}\n")
        report.write(f"login_workers={args.login_workers}\nmail_workers={args.mail_workers}\nchat_clients={args.chat_clients}\n")
        for name, phase in phases:
            print(f"phase={name} duration={args.duration}s", flush=True)
            started = time.monotonic()
            phase()
            if name == "login":
                refresh_tokens(accounts, stats)
            elapsed = time.monotonic() - started
            report.write(f"\n[{name}] elapsed={elapsed:.2f}s\n{stats.report()}\n")
            report.flush()
            print(f"phase={name} complete elapsed={elapsed:.2f}s", flush=True)

    snapshot(output / "after.txt")
    print(f"output={output}")
    print((output / "report.txt").read_text(encoding="utf-8"))


if __name__ == "__main__":
    main()
