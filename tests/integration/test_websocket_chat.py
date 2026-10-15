# -*- coding: utf-8 -*-
"""Exercise the real WebSocket gateway against shared MySQL/Redis state."""

from __future__ import annotations

import time

import pytest

from framework import WsClient
from framework.concurrency import concurrent_posts


pytestmark = [pytest.mark.integration, pytest.mark.realtime]


def authenticate(client, user):
    client.send({"type": "auth", "token": user.session["token"]})
    result = client.receive(expected_type="auth_result")
    assert result["ok"] is True
    assert result["player_id"] == user.player_id


def join_lobby(client):
    client.send({"type": "join", "channel_kind": "lobby", "scope_id": "global"})
    result = client.receive(expected_type="join_result")
    assert result["ok"] is True
    assert result["channel"]["channel_id"] == "lobby:global"


def wait_until(predicate, description, timeout=5):
    deadline = time.monotonic() + timeout
    while not predicate():
        if time.monotonic() >= deadline:
            pytest.fail(description)
        time.sleep(0.05)


def test_websocket_rejects_unauthenticated_and_invalid_token(integration_ws_url):
    with WsClient(integration_ws_url) as client:
        client.send({"type": "join", "channel_kind": "lobby", "scope_id": "global"})
        assert client.receive(expected_type="error")["message"]
        client.send({"type": "history", "channel_kind": "lobby", "scope_id": "global"})
        assert client.receive(expected_type="error")["code"] == "unauthorized"

        client.send({"type": "auth", "token": "invalid.integration.token"})
        result = client.receive(expected_type="auth_result")
        assert result["ok"] is False


def test_websocket_failed_reauth_clears_previous_identity(integration_ws_url, integration_user):
    with WsClient(integration_ws_url) as client:
        authenticate(client, integration_user)
        join_lobby(client)
        client.send({"type": "auth", "token": "invalid.reauth.token"})
        assert client.receive(expected_type="auth_result")["ok"] is False
        client.send({"type": "say", "text": "must not use previous identity"})
        assert client.receive(expected_type="error")["code"] == "unauthorized"


def test_websocket_broadcast_order_and_history(integration_ws_url, make_integration_user):
    sender = make_integration_user("ws_sender")
    receiver = make_integration_user("ws_receiver")
    texts = ["ws_order_{}_{}".format(index, time.time_ns()) for index in range(3)]

    with WsClient(integration_ws_url) as sender_ws, WsClient(integration_ws_url) as receiver_ws:
        authenticate(sender_ws, sender)
        join_lobby(sender_ws)
        authenticate(receiver_ws, receiver)
        join_lobby(receiver_ws)

        for text in texts:
            sender_ws.send({"type": "say", "message_kind": "text", "text": text})

        sender_messages = [
            sender_ws.receive(expected_type="message")["message"] for _ in texts
        ]
        receiver_messages = [
            receiver_ws.receive(expected_type="message")["message"] for _ in texts
        ]
        assert [message["text"] for message in sender_messages] == texts
        assert [message["text"] for message in receiver_messages] == texts
        assert all(message["sender_player_id"] == sender.player_id for message in receiver_messages)
        assert len({message["message_id"] for message in receiver_messages}) == len(texts)

        deadline = time.monotonic() + 5
        while True:
            receiver_ws.send({
                "type": "history",
                "channel_kind": "lobby",
                "scope_id": "global",
                "limit": 20,
            })
            history = receiver_ws.receive(expected_type="history_result")
            history_texts = [message["text"] for message in history["items"]]
            if all(text in history_texts for text in texts):
                break
            if time.monotonic() >= deadline:
                pytest.fail("broadcast messages were not persisted to chat history within 5 seconds")
            time.sleep(0.1)


def test_websocket_reconnect_and_revoked_token(integration_ws_url, make_integration_user):
    user = make_integration_user("ws_reconnect")

    with WsClient(integration_ws_url) as first:
        authenticate(first, user)
        join_lobby(first)

    with WsClient(integration_ws_url) as reconnected:
        authenticate(reconnected, user)
        join_lobby(reconnected)

        user.api.post("/auth/logout", expected=200)
        reconnected.send({"type": "say", "text": "must not be published after logout"})
        assert reconnected.receive(expected_type="error")["code"] == "unauthorized"
        reconnected.send({"type": "history", "channel_kind": "lobby", "scope_id": "global"})
        assert reconnected.receive(expected_type="error")["code"] == "unauthorized"

    with WsClient(integration_ws_url) as revoked:
        revoked.send({"type": "auth", "token": user.session["token"]})
        assert revoked.receive(expected_type="auth_result")["ok"] is False


@pytest.mark.parametrize("invalidation", ["logout", "redis_expiry"])
def test_websocket_passive_receiver_rejects_invalid_session(
    integration_ws_url, make_integration_user, redis_client, invalidation
):
    sender = make_integration_user("push_sender")
    receiver = make_integration_user("push_receiver")
    with WsClient(integration_ws_url) as sender_ws, WsClient(integration_ws_url) as receiver_ws:
        authenticate(sender_ws, sender)
        join_lobby(sender_ws)
        authenticate(receiver_ws, receiver)
        join_lobby(receiver_ws)

        if invalidation == "logout":
            receiver.api.post("/auth/logout", expected=200)
        else:
            # Exercise real Redis TTL expiry rather than manually deleting the session.
            key = "session:id:" + receiver.session["session_id"]
            assert redis_client.pexpire(key, 50)
            wait_until(lambda: not redis_client.exists(key), "session Redis TTL did not expire")

        # The receiver sends no frame after its session becomes invalid.
        text = "push_after_invalidation_{}".format(time.time_ns())
        sender_ws.send({"type": "say", "text": text})
        assert sender_ws.receive(expected_type="message")["message"]["text"] == text
        assert receiver_ws.receive(expected_type="error")["code"] == "unauthorized"
        wait_until(
            lambda: not redis_client.sismember("chat:online:players", receiver.player_id),
            "invalid passive receiver was not marked offline",
        )
        assert not redis_client.sismember("chat:channel:members:lobby:global", receiver.player_id)

        # The invalid subscriber has been removed; a fresh account can reuse this socket.
        sender_ws.send({"type": "say", "text": "after_unsubscribe"})
        sender_ws.receive(expected_type="message")
        fresh = make_integration_user("push_fresh")
        authenticate(receiver_ws, fresh)


def test_websocket_private_offline_messages_reconnect_and_pull_once(
    integration_ws_url, make_integration_user, redis_client
):
    sender = make_integration_user("offline_sender")
    receiver = make_integration_user("offline_receiver")
    sender.api.post("/friends/request", json={"target_player_id": receiver.player_id}, expected=201)
    receiver.api.post(
        "/friends/respond", json={"requester_player_id": sender.player_id, "accept": True}, expected=200
    )
    with WsClient(integration_ws_url) as disconnected:
        authenticate(disconnected, receiver)
    wait_until(
        lambda: not redis_client.sismember("chat:online:players", receiver.player_id),
        "disconnected receiver remained online",
    )

    texts = ["offline_{}_{}".format(i, time.time_ns()) for i in range(3)]
    with WsClient(integration_ws_url) as sender_ws:
        authenticate(sender_ws, sender)
        sender_ws.send({"type": "join", "channel_kind": "friend_direct", "friend_player_id": receiver.player_id})
        channel = sender_ws.receive(expected_type="join_result")["channel"]["channel_id"]
        sent = []
        for text in texts:
            sender_ws.send({"type": "say", "text": text})
            sent.append(sender_ws.receive(expected_type="message")["message"])

    offline_key = "chat:offline:" + receiver.player_id
    wait_until(lambda: redis_client.llen(offline_key) == 3, "private messages were not queued offline")
    unread = receiver.api.data(receiver.api.get("/chat/unread", expected=200))
    assert next(item["unread_count"] for item in unread if item["channel_id"] == channel) == 3

    with WsClient(integration_ws_url) as reconnected:
        authenticate(reconnected, receiver)
        first = receiver.api.data(receiver.api.post("/chat/offline/pull", json={"limit": 2}, expected=200))
        assert [item["text"] for item in first] == texts[:2]
        assert redis_client.llen(offline_key) == 1
        # Downloading a notification is not an explicit acknowledgement of reading it.
        unread = receiver.api.data(receiver.api.get("/chat/unread", expected=200))
        assert next(item["unread_count"] for item in unread if item["channel_id"] == channel) == 3
        second = receiver.api.data(receiver.api.post("/chat/offline/pull", json={"limit": 2}, expected=200))
        assert [item["text"] for item in second] == texts[2:]
        assert [item["message_id"] for item in first + second] == [item["message_id"] for item in sent]
        assert receiver.api.data(receiver.api.post("/chat/offline/pull", json={"limit": 2}, expected=200)) == []
        assert redis_client.llen(offline_key) == 0
        unread = receiver.api.data(receiver.api.get("/chat/unread", expected=200))
        assert next(item["unread_count"] for item in unread if item["channel_id"] == channel) == 3
        receiver.api.post("/chat/read", json={"channel_kind": "friend_direct",
            "player_a": sender.player_id, "player_b": receiver.player_id}, expected=200)
        unread = receiver.api.data(receiver.api.get("/chat/unread", expected=200))
        assert all(item["channel_id"] != channel or item["unread_count"] == 0 for item in unread)


@pytest.mark.parametrize("workers", [2, 8])
def test_concurrent_offline_pull_has_no_duplicate_or_missing_messages(
    integration_ws_url, make_integration_user, redis_client, workers
):
    sender, receiver = make_integration_user("pull_sender"), make_integration_user("pull_receiver")
    sender.api.post("/friends/request", json={"target_player_id": receiver.player_id}, expected=201)
    receiver.api.post("/friends/respond", json={"requester_player_id": sender.player_id, "accept": True}, expected=200)
    count = workers * 3
    with WsClient(integration_ws_url) as socket:
        authenticate(socket, sender)
        socket.send({"type": "join", "channel_kind": "friend_direct", "friend_player_id": receiver.player_id})
        channel = socket.receive(expected_type="join_result")["channel"]["channel_id"]
        for iteration in range(3):
            sent = []
            for index in range(count):
                socket.send({"type": "say", "text": "competing_pull_{}_{}_{}".format(iteration, index, time.time_ns())})
                sent.append(socket.receive(expected_type="message")["message"])
            responses = concurrent_posts(receiver.api, "/chat/offline/pull", [{"limit": 3}] * workers)
            assert [r.status_code for r in responses] == [200] * workers
            batches = [receiver.api.data(r) for r in responses]
            received = [message for batch in batches for message in batch]
            assert len(received) == count
            assert len({message["message_id"] for message in received}) == count
            assert {message["message_id"] for message in received} == {message["message_id"] for message in sent}
            order = {message["message_id"]: index for index, message in enumerate(sent)}
            assert all([order[m["message_id"]] for m in batch] == sorted(order[m["message_id"]] for m in batch) for batch in batches)
            assert redis_client.llen("chat:offline:" + receiver.player_id) == 0
            unread = receiver.api.data(receiver.api.get("/chat/unread", expected=200))
            assert next(item["unread_count"] for item in unread if item["channel_id"] == channel) == count * (iteration + 1)
