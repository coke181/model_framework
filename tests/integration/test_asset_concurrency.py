"""Cross-check concurrent HTTP outcomes with committed asset and ledger facts."""

import uuid

import pytest

from framework.concurrency import concurrent_posts
from test_mysql_redis_consistency import fetch_one


pytestmark = [pytest.mark.integration, pytest.mark.business]
WORKERS = 8


def test_concurrent_mail_claim_grants_assets_once(integration_admin_api, integration_user, mysql_connection):
    user = integration_user
    mail = integration_admin_api.data(integration_admin_api.post("/mail/send", json={
        "player_id": user.player_id, "title": "concurrent claim", "body": "8 competing claims",
        "rewards": [{"kind": "currency", "resource_id": "gold", "amount": 19}],
    }, expected=201))
    responses = concurrent_posts(user.api, "/mail/claim", [{"mail_id": mail["mail_id"]}] * WORKERS)
    assert [r.status_code for r in responses] == [200] * WORKERS
    claims = [user.api.data(r) for r in responses]
    assert len({claim["grant"]["grant_id"] for claim in claims}) == 1
    row = fetch_one(mysql_connection,
        "SELECT gold FROM player_profile WHERE player_id=%s", (user.player_id,))
    assert int(row["gold"]) == int(user.player["gold"]) + 19
    row = fetch_one(mysql_connection,
        "SELECT COUNT(*) AS n FROM reward_grant WHERE player_id=%s AND source_type='mail' AND source_id=%s",
        (user.player_id, mail["mail_id"]))
    assert row["n"] == 1
    row = fetch_one(mysql_connection,
        "SELECT is_claimed, claim_grant_id FROM mail_message WHERE mail_id=%s", (mail["mail_id"],))
    assert row["is_claimed"] == 1
    assert row["claim_grant_id"] == claims[0]["grant"]["grant_id"]


def test_concurrent_duplicate_grant_id_is_idempotent(integration_admin_api, integration_user, mysql_connection):
    user = integration_user
    grant_id = "it_idempotent_" + uuid.uuid4().hex
    payload = {"player_id": user.player_id, "grant_id": grant_id, "source_type": "integration",
               "rewards": [{"kind": "currency", "resource_id": "gem", "amount": 23}]}
    responses = concurrent_posts(integration_admin_api, "/rewards/grant", [payload] * WORKERS)
    assert [r.status_code for r in responses] == [200] * WORKERS
    assert all(integration_admin_api.data(r)["grant_id"] == grant_id for r in responses)
    row = fetch_one(mysql_connection, "SELECT gem FROM player_profile WHERE player_id=%s", (user.player_id,))
    assert int(row["gem"]) == int(user.player["gem"]) + 23
    assert fetch_one(mysql_connection, "SELECT COUNT(*) AS n FROM reward_grant WHERE grant_id=%s", (grant_id,))["n"] == 1


@pytest.mark.parametrize("purchase_limit", [0, 1])
def test_concurrent_purchase_preserves_balance_inventory_and_ledger(
    integration_admin_api, integration_user, mysql_connection, purchase_limit
):
    user = integration_user
    product_id, item_id = "it_product_" + uuid.uuid4().hex[:16], "it_item_" + uuid.uuid4().hex[:16]
    # With no limit the account can afford exactly one; with a limit it can afford all eight.
    initial_gem = int(user.player["gem"])
    budget = initial_gem + 71
    integration_admin_api.post("/rewards/grant", json={
        "player_id": user.player_id, "grant_id": "budget_" + uuid.uuid4().hex,
        "rewards": [{"kind": "currency", "resource_id": "gem", "amount": 71}],
    }, expected=200)
    price = 1 if purchase_limit else budget
    integration_admin_api.post("/admin/shop/products/upsert", json={
        "product_id": product_id, "name": "concurrency probe", "price_currency_id": "gem",
        "price_amount": price, "purchase_limit": purchase_limit, "enabled": True,
        "rewards": [{"kind": "item", "resource_id": item_id, "amount": 1}],
    }, expected=200)
    responses = concurrent_posts(user.api, "/shop/buy", [{"product_id": product_id}] * WORKERS)
    assert [r.status_code for r in responses].count(200) == 1
    for response in responses:
        if response.status_code != 200:
            assert response.status_code == (403 if purchase_limit else 400)
            assert user.api.error_code(response) == ("shop_purchase_limit_reached" if purchase_limit else "currency_not_enough")
    row = fetch_one(mysql_connection, "SELECT gem FROM player_profile WHERE player_id=%s", (user.player_id,))
    assert int(row["gem"]) == budget - price
    row = fetch_one(mysql_connection,
        "SELECT quantity FROM player_inventory WHERE player_id=%s AND item_id=%s", (user.player_id, item_id))
    assert int(row["quantity"]) == 1
    assert fetch_one(mysql_connection,
        "SELECT COUNT(*) AS n FROM shop_purchase WHERE player_id=%s AND product_id=%s", (user.player_id, product_id))["n"] == 1
    assert fetch_one(mysql_connection,
        "SELECT COUNT(*) AS n FROM reward_grant WHERE player_id=%s AND source_type='shop' AND source_id=%s", (user.player_id, product_id))["n"] == 1


@pytest.mark.parametrize("purchase_limit", [0, 1])
def test_purchase_reward_failure_rolls_back_all_assets(
    integration_admin_api, integration_user, mysql_connection, purchase_limit
):
    """Inject a targeted SQL failure only inside the disposable integration DB."""
    user = integration_user
    suffix = uuid.uuid4().hex[:16]
    product_id, item_id, constraint = "it_fault_" + suffix, "it_item_" + suffix, "it_fail_" + suffix
    integration_admin_api.post("/rewards/grant", json={
        "player_id": user.player_id, "grant_id": "budget_" + suffix,
        "rewards": [{"kind": "currency", "resource_id": "gem", "amount": 71}],
    }, expected=200)
    budget = int(user.player["gem"]) + 71
    integration_admin_api.post("/admin/shop/products/upsert", json={
        "product_id": product_id, "name": "rollback probe", "price_currency_id": "gem",
        "price_amount": 7, "purchase_limit": purchase_limit, "enabled": True,
        "rewards": [{"kind": "item", "resource_id": item_id, "amount": 1}],
    }, expected=200)
    # Do not allow fault injection against an arbitrary manually configured database.
    database = fetch_one(mysql_connection, "SELECT DATABASE() AS name", ())["name"]
    assert database.startswith("game_backend_it_"), "Fault injection requires the isolated runner DB"
    with mysql_connection.cursor() as cursor:
        cursor.execute(
            "ALTER TABLE reward_grant ADD CONSTRAINT `{}` "
            "CHECK (NOT (source_type='shop' AND source_id=%s))".format(constraint),
            (product_id,),
        )
    try:
        response = user.api.post("/shop/buy", json={"product_id": product_id})
        assert int(fetch_one(mysql_connection,
            "SELECT gem FROM player_profile WHERE player_id=%s", (user.player_id,))["gem"]) == budget
        for table, condition, params in [
            ("player_inventory", "player_id=%s AND item_id=%s", (user.player_id, item_id)),
            ("reward_grant", "player_id=%s AND source_id=%s", (user.player_id, product_id)),
            ("shop_purchase", "player_id=%s AND product_id=%s", (user.player_id, product_id)),
        ]:
            assert fetch_one(mysql_connection, "SELECT COUNT(*) AS n FROM " + table + " WHERE " + condition, params)["n"] == 0
        assert response.status_code == 500, response.text
    finally:
        with mysql_connection.cursor() as cursor:
            cursor.execute("ALTER TABLE reward_grant DROP CHECK `{}`".format(constraint))
    # A retry after recovery must succeed, including rollback of the reserved purchase limit.
    user.api.post("/shop/buy", json={"product_id": product_id}, expected=200)
    assert int(fetch_one(mysql_connection,
        "SELECT gem FROM player_profile WHERE player_id=%s", (user.player_id,))["gem"]) == budget - 7
