# -*- coding: utf-8 -*-
import pytest


pytestmark = pytest.mark.business


def test_chat_lobby_unread_and_channels(api, make_user):
    user = make_user("chat")

    history = api.data(api.get("/chat/history/lobby", expected=200))
    assert isinstance(history["messages"], list)
    assert isinstance(history["has_more"], bool)
    assert "next_cursor" in history

    unread = user.api.data(user.api.get("/chat/unread", expected=200))
    assert isinstance(unread, list)

    examples = api.data(api.get("/chat/channels/examples", expected=200))
    assert {"lobby", "room", "friend_direct"} <= set(examples.keys())

    presence = api.data(api.post(
        "/chat/presence/player",
        json={"player_id": user.player_id},
        expected=200,
    ))
    assert "online" in presence

    offline = user.api.data(user.api.post("/chat/offline/pull", json={}, expected=200))
    assert isinstance(offline, list)


def test_chat_block_and_blocks_list(make_user):
    first = make_user("block_a")
    second = make_user("block_b")

    first.api.post(
        "/chat/block",
        json={"target_player_id": second.player_id, "blocked": True},
        expected=200,
    )
    blocks = first.api.data(first.api.get("/chat/blocks", expected=200))
    assert any(item["blocked_player_id"] == second.player_id for item in blocks)
