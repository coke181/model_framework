"""Small JSON WebSocket client for chat gateway integration tests."""

from __future__ import annotations

import json
from typing import Any, Dict, Optional


class WsProtocolError(AssertionError):
    """Raised when a gateway frame violates the expected JSON protocol."""


class WsClient:
    def __init__(self, url: str, *, timeout: float = 5) -> None:
        try:
            import websocket
        except ImportError as exc:
            raise RuntimeError(
                "websocket-client is required; install tests/requirements-integration.txt"
            ) from exc

        self._socket = websocket.create_connection(url, timeout=timeout)

    def close(self) -> None:
        self._socket.close()

    def __enter__(self) -> "WsClient":
        return self

    def __exit__(self, exc_type, exc_value, traceback) -> None:
        self.close()

    def send(self, payload: Dict[str, Any]) -> None:
        self._socket.send(json.dumps(payload, separators=(",", ":")))

    def receive(self, *, expected_type: Optional[str] = None) -> Dict[str, Any]:
        raw = self._socket.recv()
        if not isinstance(raw, str):
            raise WsProtocolError("expected a text WebSocket frame")
        try:
            payload = json.loads(raw)
        except ValueError as exc:
            raise WsProtocolError("WebSocket frame is not valid JSON: {!r}".format(raw)) from exc
        if not isinstance(payload, dict):
            raise WsProtocolError("WebSocket JSON root must be an object")
        if expected_type is not None and payload.get("type") != expected_type:
            raise WsProtocolError(
                "expected frame type {!r}, got {!r}: {!r}".format(
                    expected_type, payload.get("type"), payload
                )
            )
        return payload
