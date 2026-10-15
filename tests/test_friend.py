# -*- coding: utf-8 -*-
import pytest


pytestmark = pytest.mark.business


def test_friend_request_accept_and_list(make_user):
    first = make_user("friend_a")
    second = make_user("friend_b")

    first.api.post(
        "/friends/request",
        json={"target_player_id": second.player_id},
        expected=201,
    )
    incoming = second.api.data(second.api.get("/friends/requests", expected=200))
    assert any(item["requester_player_id"] == first.player_id for item in incoming["incoming"])

    accepted = second.api.data(second.api.post(
        "/friends/respond",
        json={"requester_player_id": first.player_id, "accept": True},
        expected=200,
    ))
    assert accepted["accepted"] is True

    friends = first.api.data(first.api.get("/friends", expected=200))
    assert any(item["friend_player_id"] == second.player_id for item in friends)

    presence = first.api.data(first.api.get("/friends/presence", expected=200))
    assert isinstance(presence["items"], list)
    assert isinstance(presence["has_more"], bool)
    assert any(item["player_id"] == second.player_id for item in presence["items"])
