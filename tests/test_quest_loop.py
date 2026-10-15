# -*- coding: utf-8 -*-
import pytest


pytestmark = pytest.mark.business


def test_quest_full_loop_and_idempotent_claim(admin_api, make_user):
    user = make_user("quest")
    quest_id = "quest_pytest_1"

    definition = admin_api.data(admin_api.post("/admin/quests/upsert", json={
        "quest_id": quest_id,
        "version": 1,
        "title": "pytest quest",
        "claim_mode": "manual",
        "enabled": True,
        "pre_quest_ids": [],
        "required_flags": [],
        "completion_flag": "quest_pytest_done",
        "objectives": [{
            "objective_id": "kill",
            "event_type": "monster_killed",
            "target_id": "slime",
            "target_value": 2,
        }],
        "rewards": [{"kind": "currency", "resource_id": "gold", "amount": 10}],
    }, expected=200))
    assert definition["quest_id"] == quest_id

    accepted = user.api.data(user.api.post("/quests/accept", json={
        "quest_id": quest_id,
        "request_id": "accept_1",
    }, expected=200))
    assert accepted["state"] == "accepted"

    event = user.api.data(user.api.post("/quests/events", json={
        "event_id": "event_1",
        "event_type": "monster_killed",
        "target_id": "slime",
        "amount": 2,
        "source_id": "pytest",
    }, expected=200))
    assert event["snapshot"]["quests"][0]["state"] == "completed"

    claim = user.api.data(user.api.post("/quests/claim", json={
        "quest_id": quest_id,
        "request_id": "claim_1",
    }, expected=200))
    first_gold = claim["player"]["gold"]
    assert claim["quest"]["state"] == "claimed"

    repeated = user.api.data(user.api.post("/quests/claim", json={
        "quest_id": quest_id,
        "request_id": "claim_2",
    }, expected=200))
    assert repeated["player"]["gold"] == first_gold
