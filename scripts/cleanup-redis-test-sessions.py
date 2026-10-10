"""按明确的测试 player_id 清单，分批清理 Redis Session。

调用流程：
1. 从 TSV 文件读取第二列 player_id。
2. dry-run 只用 SCARD 统计，不修改 Redis。
3. --execute 使用 SSCAN 分批读取会话 ID，GET 主记录并解析旧 token 索引。
4. 使用 UNLINK 异步释放 Session key，最后删除测试玩家的 Session 集合。

脚本不会扫描或删除普通玩家，也不会修改 MySQL 数据。
"""

from __future__ import annotations

import argparse
import json
import socket
import time
from pathlib import Path
from typing import Iterable


class RedisProtocolError(RuntimeError):
    pass


class RedisConnection:
    def __init__(self, host: str, port: int, timeout: float = 30.0):
        self._socket = socket.create_connection((host, port), timeout=timeout)
        self._stream = self._socket.makefile("rb")

    def close(self) -> None:
        self._stream.close()
        self._socket.close()

    def command(self, *parts: object):
        return self.pipeline([parts])[0]

    def pipeline(self, commands: Iterable[Iterable[object]]):
        command_list = list(commands)
        payload = bytearray()
        for command in command_list:
            parts = list(command)
            payload.extend(f"*{len(parts)}\r\n".encode())
            for part in parts:
                raw = str(part).encode("utf-8")
                payload.extend(f"${len(raw)}\r\n".encode())
                payload.extend(raw)
                payload.extend(b"\r\n")
        self._socket.sendall(payload)
        return [self._read_response() for _ in command_list]

    def _read_response(self):
        prefix = self._stream.read(1)
        if not prefix:
            raise RedisProtocolError("Redis connection closed")
        if prefix == b"+":
            return self._read_line().decode("utf-8")
        if prefix == b"-":
            raise RedisProtocolError(self._read_line().decode("utf-8", errors="replace"))
        if prefix == b":":
            return int(self._read_line())
        if prefix == b"$":
            length = int(self._read_line())
            if length < 0:
                return None
            data = self._stream.read(length)
            self._expect_crlf()
            return data.decode("utf-8")
        if prefix == b"*":
            length = int(self._read_line())
            if length < 0:
                return None
            return [self._read_response() for _ in range(length)]
        raise RedisProtocolError(f"Unsupported RESP prefix: {prefix!r}")

    def _read_line(self) -> bytes:
        line = self._stream.readline()
        if not line.endswith(b"\r\n"):
            raise RedisProtocolError("Invalid RESP line")
        return line[:-2]

    def _expect_crlf(self) -> None:
        if self._stream.read(2) != b"\r\n":
            raise RedisProtocolError("Invalid RESP bulk terminator")


def load_player_ids(path: Path) -> list[str]:
    player_ids: list[str] = []
    seen: set[str] = set()
    raw = path.read_bytes()
    encoding = "utf-16" if raw.startswith((b"\xff\xfe", b"\xfe\xff")) else "utf-8"
    for raw_line in raw.decode(encoding).splitlines():
        columns = raw_line.strip().split("\t")
        if not columns:
            continue
        player_id = columns[1] if len(columns) >= 2 else columns[0]
        if player_id and player_id not in seen:
            seen.add(player_id)
            player_ids.append(player_id)
    return player_ids


def count_sessions(connection: RedisConnection, player_ids: list[str], batch_size: int) -> int:
    total = 0
    for offset in range(0, len(player_ids), batch_size):
        batch = player_ids[offset : offset + batch_size]
        counts = connection.pipeline(
            [("SCARD", f"session:player:{player_id}") for player_id in batch]
        )
        total += sum(int(value) for value in counts)
    return total


def cleanup_player(
    connection: RedisConnection,
    player_id: str,
    scan_count: int,
) -> tuple[int, int]:
    cursor = "0"
    sessions_seen = 0
    keys_unlinked = 0
    player_key = f"session:player:{player_id}"

    while True:
        response = connection.command("SSCAN", player_key, cursor, "COUNT", scan_count)
        cursor = str(response[0])
        session_ids = response[1] or []
        if session_ids:
            payloads = connection.pipeline(
                [("GET", f"session:id:{session_id}") for session_id in session_ids]
            )
            unlink_commands: list[tuple[object, ...]] = []
            for session_id, payload in zip(session_ids, payloads):
                unlink_commands.append(("UNLINK", f"session:id:{session_id}"))
                if payload:
                    try:
                        session = json.loads(payload)
                    except json.JSONDecodeError:
                        session = {}
                    token = session.get("token")
                    refresh_token = session.get("refresh_token")
                    if token:
                        unlink_commands.append(("UNLINK", f"session:token:{token}"))
                    if refresh_token:
                        unlink_commands.append(("UNLINK", f"session:refresh:{refresh_token}"))
            results = connection.pipeline(unlink_commands)
            keys_unlinked += sum(int(value) for value in results)
            sessions_seen += len(session_ids)

        if cursor == "0":
            break

    keys_unlinked += int(connection.command("UNLINK", player_key))
    return sessions_seen, keys_unlinked


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--players-file", required=True, type=Path)
    parser.add_argument("--host", default="127.0.0.1")
    parser.add_argument("--port", type=int, default=6379)
    parser.add_argument("--batch-size", type=int, default=500)
    parser.add_argument("--execute", action="store_true")
    args = parser.parse_args()

    player_ids = load_player_ids(args.players_file)
    if not player_ids:
        raise SystemExit("player_id 清单为空，拒绝继续")

    connection = RedisConnection(args.host, args.port)
    started_at = time.perf_counter()
    try:
        total = count_sessions(connection, player_ids, args.batch_size)
        print(f"players={len(player_ids)} identified_sessions={total} execute={args.execute}")
        if not args.execute:
            print("dry-run 完成；确认 player_id 清单后再添加 --execute")
            return

        sessions_seen = 0
        keys_unlinked = 0
        for index, player_id in enumerate(player_ids, start=1):
            player_sessions, player_keys = cleanup_player(
                connection, player_id, args.batch_size
            )
            sessions_seen += player_sessions
            keys_unlinked += player_keys
            if index % 100 == 0 or player_sessions >= 10000:
                elapsed = time.perf_counter() - started_at
                print(
                    f"progress={index}/{len(player_ids)} sessions={sessions_seen} "
                    f"keys_unlinked={keys_unlinked} elapsed={elapsed:.1f}s",
                    flush=True,
                )

        elapsed = time.perf_counter() - started_at
        print(
            f"completed sessions={sessions_seen} keys_unlinked={keys_unlinked} "
            f"elapsed={elapsed:.1f}s"
        )
    finally:
        connection.close()


if __name__ == "__main__":
    main()
