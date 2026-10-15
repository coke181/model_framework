# -*- coding: utf-8 -*-
"""Cross-check API results against MySQL facts and Redis session indexes.

These tests never start or configure infrastructure. They run only when an
explicit integration base URL and database credentials are supplied.
"""

import json
import pytest


pytestmark = pytest.mark.integration


def fetch_one(connection, sql, params):
    connection.ping()
    with connection.cursor() as cursor:
        cursor.execute(sql, params)
        return cursor.fetchone()


def test_registration_persists_account_and_player(mysql_connection, integration_user):
    account = fetch_one(
        mysql_connection,
        "SELECT account_id, login_name FROM account WHERE account_id=%s",
        (integration_user.account_id,),
    )
    player = fetch_one(
        mysql_connection,
        "SELECT player_id, account_id, nickname FROM player_profile WHERE player_id=%s",
        (integration_user.player_id,),
    )
    assert account == {
        "account_id": integration_user.account_id,
        "login_name": integration_user.login_name,
    }
    assert player["account_id"] == integration_user.account_id
    assert player["nickname"] == integration_user.player["nickname"]


def test_reward_matches_grant_and_player_balance(
    mysql_connection, integration_admin_api, integration_user
):
    initial_gem = int(integration_user.player["gem"])
    grant = integration_admin_api.data(integration_admin_api.post(
        "/rewards/grant",
        json={
            "player_id": integration_user.player_id,
            "source_type": "integration_test",
            "source_id": integration_user.login_name,
            "rewards": [{"kind": "currency", "resource_id": "gem", "amount": 17}],
        },
        expected=200,
    ))

    grant_row = fetch_one(
        mysql_connection,
        "SELECT player_id, source_type, source_id, rewards_json "
        "FROM reward_grant WHERE grant_id=%s",
        (grant["grant_id"],),
    )
    player_row = fetch_one(
        mysql_connection,
        "SELECT gem FROM player_profile WHERE player_id=%s",
        (integration_user.player_id,),
    )
    rewards = grant_row["rewards_json"]
    if isinstance(rewards, str):
        rewards = json.loads(rewards)
    assert grant_row["player_id"] == integration_user.player_id
    assert grant_row["source_type"] == "integration_test"
    assert rewards[0]["amount"] == 17
    assert int(player_row["gem"]) == initial_gem + 17


def test_mail_claim_matches_mysql_state(
    mysql_connection, integration_admin_api, integration_user
):
    mail = integration_admin_api.data(integration_admin_api.post(
        "/mail/send",
        json={
            "player_id": integration_user.player_id,
            "title": "integration consistency",
            "body": "verify mail and claim state",
            "sender_name": "Test",
            "rewards": [{"kind": "currency", "resource_id": "gold", "amount": 9}],
            "expires_in_seconds": 3600,
        },
        expected=201,
    ))
    integration_user.api.post(
        "/mail/claim", json={"mail_id": mail["mail_id"]}, expected=200
    )

    row = fetch_one(
        mysql_connection,
        "SELECT player_id, title, is_claimed, claim_grant_id "
        "FROM mail_message WHERE mail_id=%s",
        (mail["mail_id"],),
    )
    assert row["player_id"] == integration_user.player_id
    assert row["title"] == "integration consistency"
    assert int(row["is_claimed"]) == 1
    assert row["claim_grant_id"]


def test_login_session_has_consistent_redis_indexes(redis_client, integration_user):
    token = integration_user.session["token"]
    session_id = integration_user.session["session_id"]
    assert token.startswith(session_id + ".")
    session = json.loads(redis_client.get("session:id:" + session_id))
    assert session["session_id"] == session_id
    assert session["token"] == token
    assert session["player_id"] == integration_user.player_id
    assert redis_client.sismember(
        "session:player:" + integration_user.player_id,
        session_id,
    )
