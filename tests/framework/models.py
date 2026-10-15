"""Typed test data returned by shared fixtures."""

from __future__ import annotations

from dataclasses import dataclass
from typing import Any, Dict

from .api_client import ApiClient


@dataclass(frozen=True)
class TestUser:
    login_name: str
    password: str
    account: Dict[str, Any]
    player: Dict[str, Any]
    session: Dict[str, Any]
    api: ApiClient

    @property
    def account_id(self) -> str:
        return str(self.account["account_id"])

    @property
    def player_id(self) -> str:
        return str(self.player["player_id"])
